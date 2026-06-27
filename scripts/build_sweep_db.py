#!/usr/bin/env python3
# =============================================================================
# Constrói um SQLite local a partir do sweep.csv para consulta token-efficient
# por LLMs (rodar SELECTs e receber só o que precisa, em vez de ler CSV inteiro).
#
# Uso:
#   scripts/build_sweep_db.py runs/i5/sweep.csv -o runs/i5/sweep.db
#
# Gera a tabela `runs` (tipada) + views derivadas:
#   v_speedup        — speedup vs baseline sequential (fase A)
#   v_self_speedup   — speedup vs o próprio searcher em T=1 (escalabilidade)
#   v_footprint      — fase B: throughput vs tamanho do autômato (KiB)
#   v_build          — fase E: build paralelo vs sequencial (idea 4)
#   v_correctness    — nº de match-counts distintos por (patterns,corpus) [=1 ok]
#   v_best           — melhor searcher (mbps) por (patterns,corpus,thr) na fase A
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
}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("csv_path", nargs="?", default="runs/i5/sweep.csv")
    ap.add_argument("-o", "--output", default=None)
    args = ap.parse_args()
    db_path = args.output or os.path.splitext(args.csv_path)[0] + ".db"

    if os.path.exists(db_path):
        os.remove(db_path)
    con = sqlite3.connect(db_path)
    cur = con.cursor()

    cols_ddl = ",\n  ".join(f"{c} {coltype(c)}" for c in ALL_COLS)
    cur.execute(f"CREATE TABLE runs (\n  {cols_ddl}\n)")

    with open(args.csv_path, newline="") as fh:
        reader = csv.DictReader(fh)
        rows = []
        for r in reader:
            tag = r.get("tag", "") or ""
            m = re.search(r"buildpar_T(\d+)", tag)
            r["build_threads"] = m.group(1) if m else (
                "1" if tag == "buildseq" else "")
            rows.append([cast(c, r.get(c, "")) for c in ALL_COLS])

    placeholders = ",".join("?" * len(ALL_COLS))
    cur.executemany(f"INSERT INTO runs VALUES ({placeholders})", rows)

    cur.execute("CREATE INDEX idx_phase ON runs(phase)")
    cur.execute("CREATE INDEX idx_combo ON runs(patterns, corpus, searcher, thr)")

    for name, ddl in VIEWS.items():
        cur.execute(ddl)

    con.commit()

    print(f"[db] {len(rows)} runs -> {db_path}")
    print("[db] views:", ", ".join(VIEWS))
    bad = cur.execute(
        "SELECT patterns, corpus, distinct_match_counts FROM v_correctness "
        "WHERE distinct_match_counts != 1").fetchall()
    if bad:
        print("[db] AVISO: divergência de matches:", bad)
    else:
        print("[db] correctness OK — match-count único por (patterns,corpus)")
    con.close()


if __name__ == "__main__":
    main()
