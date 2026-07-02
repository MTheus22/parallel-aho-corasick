#!/usr/bin/env python3
# =============================================================================
# Constrói um SQLite local a partir do sweep.csv para consulta token-efficient
# por LLMs (rodar SELECTs e receber só o que precisa, em vez de ler CSV inteiro).
#
# Uso:
#   scripts/build_sweep_db.py runs/i5/sweep.csv -o runs/i5/sweep.db
#
# Gera a tabela `runs` (tipada), a tabela `worker_metrics` quando houver
# logs `--per-thread`, e views derivadas:
#   v_speedup        — speedup vs baseline sequential (fase A)
#   v_self_speedup   — speedup vs o próprio searcher em T=1 (escalabilidade)
#   v_footprint      — fase B: throughput vs tamanho do autômato (KiB)
#   v_build          — fase E: build paralelo vs sequencial (idea 4)
#   v_correctness    — nº de match-counts distintos por (patterns,corpus) [=1 ok]
#   v_best           — melhor searcher (mbps) por (patterns,corpus,thr) na fase A
#   v_worker_balance — resumo de spread entre workers para logs --per-thread
# =============================================================================
import argparse
import csv
import os
import re
import sqlite3

INT_COLS = {"thr", "bytes", "matches", "wrk", "pattern_count",
            "automaton_states", "warmup", "iters", "build_threads"}
REAL_COLS = {"min_ms", "mean_ms", "median_ms", "max_ms", "cv_pct",
             "mbps", "gbps", "automaton_kib", "build_ms"}
TEXT_COLS = ["phase", "patterns", "corpus", "searcher", "threads_label",
             "tag", "log_file"]

ALL_COLS = (TEXT_COLS
            + sorted(INT_COLS - {"build_threads"})
            + sorted(REAL_COLS)
            + ["build_threads"])

WORKER_COLS = [
    "phase", "patterns", "corpus", "searcher", "threads_label", "tag",
    "thr", "log_file", "worker_id", "seconds", "milliseconds",
    "bytes_scanned", "matches_found", "mbps", "cpu",
]

WORKER_INT_COLS = {"thr", "worker_id", "bytes_scanned", "matches_found", "cpu"}
WORKER_REAL_COLS = {"seconds", "milliseconds", "mbps"}

WORKER_LINE_RE = re.compile(
    r"^\s*\[t(?P<worker_id>\d+)\]\s+"
    r"(?P<milliseconds>[\d.]+)\s+ms\s+"
    r"(?P<bytes_scanned>\d+)\s+bytes\s+"
    r"(?P<matches_found>\d+)\s+matches\s+"
    r"(?P<mbps>[\d.]+)\s+MB/s"
    r"(?:\s+cpu=(?P<cpu>-?\d+))?\s*$"
)


def coltype(c):
    if c in INT_COLS:
        return "INTEGER"
    if c in REAL_COLS:
        return "REAL"
    return "TEXT"


def cast(c, v):
    if v is None or v == "":
        return None
    try:
        if c in INT_COLS:
            return int(float(v))
        if c in REAL_COLS:
            return float(v)
    except ValueError:
        return None
    return v


def worker_coltype(c):
    if c in WORKER_INT_COLS:
        return "INTEGER"
    if c in WORKER_REAL_COLS:
        return "REAL"
    return "TEXT"


def resolve_log_path(csv_path, log_file):
    if not log_file:
        return None
    path = os.path.expanduser(log_file)
    if os.path.isabs(path) and os.path.exists(path):
        return path
    if os.path.exists(path):
        return path

    # Older CSVs store log_file relative to the repository root. If the DB is
    # rebuilt from elsewhere, also try resolving from the CSV directory.
    candidate = os.path.join(os.path.dirname(csv_path), log_file)
    if os.path.exists(candidate):
        return candidate
    return None


def parse_worker_metrics(csv_path, run_row):
    log_path = resolve_log_path(csv_path, run_row.get("log_file", ""))
    if log_path is None:
        return []

    worker_rows = []
    with open(log_path, "r", errors="replace") as fh:
        for line in fh:
            match = WORKER_LINE_RE.match(line)
            if not match:
                continue
            milliseconds = float(match.group("milliseconds"))
            worker_rows.append({
                "phase": run_row.get("phase", ""),
                "patterns": run_row.get("patterns", ""),
                "corpus": run_row.get("corpus", ""),
                "searcher": run_row.get("searcher", ""),
                "threads_label": run_row.get("threads_label", ""),
                "tag": run_row.get("tag", ""),
                "thr": cast("thr", run_row.get("thr", "")),
                "log_file": run_row.get("log_file", ""),
                "worker_id": int(match.group("worker_id")),
                "seconds": milliseconds / 1000.0,
                "milliseconds": milliseconds,
                "bytes_scanned": int(match.group("bytes_scanned")),
                "matches_found": int(match.group("matches_found")),
                "mbps": float(match.group("mbps")),
                "cpu": int(match.group("cpu")) if match.group("cpu") else None,
            })
    return worker_rows


VIEWS = {
    # Speedup absoluto vs o baseline sequential (mesma combinação pat+corpus,
    # fase A). Esta é a figura headline do TCC.
    "v_speedup": """
        CREATE VIEW v_speedup AS
        SELECT a.patterns, a.corpus, a.searcher, a.thr,
               round(a.mbps, 2)          AS mbps,
               round(b.mbps, 2)          AS seq_mbps,
               round(a.mbps / b.mbps, 3) AS speedup_vs_seq
        FROM runs a
        JOIN runs b
          ON a.patterns = b.patterns AND a.corpus = b.corpus
         AND b.phase = 'A_speedup_curves' AND b.searcher = 'sequential'
        WHERE a.phase = 'A_speedup_curves'
          AND a.searcher NOT IN ('sequential', 'sequential_flat')
        ORDER BY a.patterns, a.corpus, a.searcher, a.thr
    """,
    # Escalabilidade: mbps(T) / mbps(T=1) do MESMO searcher.
    "v_self_speedup": """
        CREATE VIEW v_self_speedup AS
        SELECT a.patterns, a.corpus, a.searcher, a.thr,
               round(a.mbps, 2)           AS mbps,
               round(a.mbps / t1.mbps, 3) AS speedup_vs_t1
        FROM runs a
        JOIN runs t1
          ON a.patterns = t1.patterns AND a.corpus = t1.corpus
         AND a.searcher = t1.searcher
         AND t1.phase = 'A_speedup_curves' AND t1.thr = 1
        WHERE a.phase = 'A_speedup_curves'
          AND a.searcher NOT IN ('sequential', 'sequential_flat')
        ORDER BY a.patterns, a.corpus, a.searcher, a.thr
    """,
    # Fase B: footprint do autômato (KiB) vs throughput.
    "v_footprint": """
        CREATE VIEW v_footprint AS
        SELECT patterns, searcher, thr, automaton_states,
               round(automaton_kib / 1024.0, 2) AS automaton_mib,
               round(mbps, 2) AS mbps
        FROM runs
        WHERE phase = 'B_footprint'
        ORDER BY automaton_kib, searcher, thr
    """,
    # Fase E: tempo de build paralelo vs sequencial (idea 4).
    "v_build": """
        CREATE VIEW v_build AS
        SELECT r.patterns,
               COALESCE(r.build_threads, 1) AS build_threads,
               round(r.build_ms, 2)         AS build_ms,
               round(s.build_ms, 2)         AS seq_build_ms,
               round(s.build_ms / r.build_ms, 3) AS build_speedup
        FROM runs r
        JOIN runs s
          ON s.patterns = r.patterns AND s.phase = 'E_build_par'
         AND s.tag = 'buildseq'
        WHERE r.phase = 'E_build_par'
        ORDER BY r.patterns, build_threads
    """,
    # Sanidade: todos os searchers devem concordar no nº de matches.
    "v_correctness": """
        CREATE VIEW v_correctness AS
        SELECT patterns, corpus,
               COUNT(DISTINCT matches) AS distinct_match_counts,
               MIN(matches) AS matches
        FROM runs
        WHERE phase = 'A_speedup_curves'
        GROUP BY patterns, corpus
    """,
    # Melhor searcher por configuração (fase A).
    "v_best": """
        CREATE VIEW v_best AS
        SELECT r.patterns, r.corpus, r.thr, r.searcher,
               round(r.mbps, 2) AS mbps
        FROM runs r
        WHERE r.phase = 'A_speedup_curves'
          AND r.searcher NOT IN ('sequential', 'sequential_flat')
          AND r.mbps = (
            SELECT MAX(mbps) FROM runs x
            WHERE x.phase = 'A_speedup_curves'
              AND x.patterns = r.patterns AND x.corpus = r.corpus
              AND x.thr = r.thr
              AND x.searcher NOT IN ('sequential', 'sequential_flat'))
        ORDER BY r.patterns, r.corpus, r.thr
    """,
    # Resumo de balanceamento dos logs --per-thread. `spread_pct` mede
    # (worker mais lento - worker mais rapido) / media.
    "v_worker_balance": """
        CREATE VIEW v_worker_balance AS
        SELECT phase, patterns, corpus, searcher, thr, tag, log_file,
               COUNT(*) AS workers,
               round(MIN(milliseconds), 3) AS min_worker_ms,
               round(AVG(milliseconds), 3) AS avg_worker_ms,
               round(MAX(milliseconds), 3) AS max_worker_ms,
               round((MAX(milliseconds) - MIN(milliseconds))
                     / AVG(milliseconds) * 100.0, 3) AS spread_pct,
               round(MAX(milliseconds) / MIN(milliseconds), 4) AS imbalance_ratio,
               round(MIN(mbps), 2) AS min_worker_mbps,
               round(AVG(mbps), 2) AS avg_worker_mbps,
               round(MAX(mbps), 2) AS max_worker_mbps,
               SUM(bytes_scanned) AS total_worker_bytes,
               SUM(matches_found) AS total_worker_matches
        FROM worker_metrics
        GROUP BY phase, patterns, corpus, searcher, thr, tag, log_file
        ORDER BY phase, patterns, corpus, searcher, thr, tag
    """,
}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("csv_path", nargs="?", default="runs/i5/sweep.csv")
    ap.add_argument("-o", "--output", default=None)
    args = ap.parse_args()
    db_path = args.output or os.path.splitext(args.csv_path)[0] + ".db"
    tmp_db_path = f"{db_path}.tmp-{os.getpid()}"

    if os.path.exists(tmp_db_path):
        os.remove(tmp_db_path)
    con = sqlite3.connect(tmp_db_path)
    cur = con.cursor()

    cols_ddl = ",\n  ".join(f"{c} {coltype(c)}" for c in ALL_COLS)
    cur.execute(f"CREATE TABLE runs (\n  {cols_ddl}\n)")

    worker_cols_ddl = ",\n  ".join(f"{c} {worker_coltype(c)}"
                                    for c in WORKER_COLS)
    cur.execute(f"CREATE TABLE worker_metrics (\n  {worker_cols_ddl}\n)")

    with open(args.csv_path, newline="") as fh:
        reader = csv.DictReader(fh)
        rows = []
        worker_rows = []
        for r in reader:
            tag = r.get("tag", "") or ""
            m = re.search(r"buildpar_T(\d+)", tag)
            r["build_threads"] = m.group(1) if m else (
                "1" if tag == "buildseq" else "")
            rows.append([cast(c, r.get(c, "")) for c in ALL_COLS])
            worker_rows.extend(parse_worker_metrics(args.csv_path, r))

    placeholders = ",".join("?" * len(ALL_COLS))
    cur.executemany(f"INSERT INTO runs VALUES ({placeholders})", rows)

    worker_placeholders = ",".join("?" * len(WORKER_COLS))
    cur.executemany(
        f"INSERT INTO worker_metrics VALUES ({worker_placeholders})",
        [[wr.get(c) for c in WORKER_COLS] for wr in worker_rows])

    cur.execute("CREATE INDEX idx_phase ON runs(phase)")
    cur.execute("CREATE INDEX idx_combo ON runs(patterns, corpus, searcher, thr)")
    cur.execute("CREATE INDEX idx_worker_log ON worker_metrics(log_file)")
    cur.execute(
        "CREATE INDEX idx_worker_combo ON worker_metrics("
        "phase, patterns, corpus, searcher, thr)")

    for name, ddl in VIEWS.items():
        cur.execute(ddl)

    con.commit()

    print(f"[db] {len(rows)} runs -> {db_path}")
    print(f"[db] {len(worker_rows)} worker metrics")
    print("[db] views:", ", ".join(VIEWS))
    bad = cur.execute(
        "SELECT patterns, corpus, distinct_match_counts FROM v_correctness "
        "WHERE distinct_match_counts != 1").fetchall()
    if bad:
        print("[db] AVISO: divergência de matches:", bad)
    else:
        print("[db] correctness OK — match-count único por (patterns,corpus)")
    con.close()
    os.replace(tmp_db_path, db_path)


if __name__ == "__main__":
    main()
