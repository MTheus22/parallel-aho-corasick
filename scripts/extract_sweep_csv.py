#!/usr/bin/env python3
# =============================================================================
# Extrai os .log do sweep do i5 (run_i5_sweep.sh) para um CSV único.
#
# Robusto:
#   - Lê o cabeçalho da tabela de cada log para mapear colunas por NOME
#     (lida com layouts antigos de 8 col e novos de 12 col).
#   - Puxa metadados do header (# patterns / # automaton / # build time / etc).
#   - Decompõe o label do arquivo (pat__cor__searcher__T<n>[_tag]).
#   - Ignora *.FAIL e logs de searchers desconhecidos (--known-only).
#
# Uso:
#   scripts/extract_sweep_csv.py [RUN_DIR] [-o saida.csv] [--known-only]
#   scripts/extract_sweep_csv.py runs/i5 -o runs/i5/sweep.csv
# =============================================================================
import argparse
import csv
import os
import re
import sys

# Searchers que existem hoje no registry. Logs de searchers fora desta lista
# (ex.: pthread_affinity, pthread_block_cyclic, sequential_delta2) são de
# versões antigas e poluem a análise — filtra com --known-only.
KNOWN_SEARCHERS = {
    "sequential", "sequential_flat",
    "pthread_chunked", "pthread_chunked_v2", "pthread_chunked_v3",
    "pthread_dynamic", "pthread_dynamic_flat", "pthread_prefetch",
    "pthread_chunked_flat", "pthread_chunked_v3_flat",
    "pthread_2d_sharded_chunked", "pattern_sharded_prefix",
}

# Colunas de saída do CSV (estáveis, independentes do layout do log).
OUT_FIELDS = [
    "phase", "patterns", "corpus", "searcher", "threads_label", "tag",
    "thr", "bytes", "matches",
    "min_ms", "mean_ms", "median_ms", "max_ms", "cv_pct", "mbps", "gbps", "wrk",
    "pattern_count", "automaton_states", "automaton_kib", "build_ms",
    "warmup", "iters", "log_file",
]

# Mapeia nomes de coluna da tabela do aclab -> chave no CSV de saída.
COL_ALIASES = {
    "thr": "thr",
    "bytes": "bytes",
    "min(ms)": "min_ms",
    "mean(ms)": "mean_ms",
    "median(ms)": "median_ms",
    "max(ms)": "max_ms",
    "cv%": "cv_pct",
    "mb/s": "mbps",
    "gbps": "gbps",
    "matches": "matches",
    "wrk": "wrk",
}


def parse_label(label):
    """pat__cor__searcher__T<n>[_tag] -> (pat, cor, searcher, T, tag)."""
    parts = label.split("__")
    if len(parts) < 4:
        return None
    pat, cor, searcher = parts[0], parts[1], parts[2]
    tser = parts[3]
    m = re.match(r"T(\d+)(?:_(.+))?$", tser)
    if not m:
        return pat, cor, searcher, tser, ""
    return pat, cor, searcher, m.group(1), (m.group(2) or "")


def parse_log(path, phase):
    rows = []
    with open(path, "r", errors="replace") as fh:
        lines = fh.readlines()

    meta = {}
    header_idx = None
    col_index = {}  # csv_key -> position in data row

    for i, line in enumerate(lines):
        s = line.strip()
        if s.startswith("# patterns:"):
            m = re.search(r"#\s*patterns:\s*(\d+)", s)
            if m:
                meta["pattern_count"] = m.group(1)
        elif s.startswith("# automaton:"):
            # O binário pode imprimir o tamanho em KiB ou MiB; normaliza p/ KiB.
            m = re.search(r"(\d+)\s*states,\s*([\d.]+)\s*([KM])iB", s)
            if m:
                meta["automaton_states"] = m.group(1)
                size = float(m.group(2)) * (1024.0 if m.group(3) == "M" else 1.0)
                meta["automaton_kib"] = f"{size:.2f}"
        elif s.startswith("# build time:"):
            m = re.search(r"([\d.]+)\s*ms", s)
            if m:
                meta["build_ms"] = m.group(1)
        elif s.startswith("# warmup="):
            m = re.search(r"warmup=(\d+)\s+iters=(\d+)", s)
            if m:
                meta["warmup"] = m.group(1)
                meta["iters"] = m.group(2)
        elif s.startswith("searcher") and "bytes" in s and "matches" in s:
            # Cabeçalho da tabela: mapeia nome de coluna -> índice.
            cols = s.split()
            for pos, name in enumerate(cols):
                key = COL_ALIASES.get(name.lower())
                if key:
                    col_index[key] = pos
            header_idx = i

    if header_idx is None:
        return rows  # log incompleto / sem tabela

    label = os.path.basename(path)
    label = re.sub(r"\.log(\.FAIL)?$", "", label)
    parsed = parse_label(label)

    # Linhas de dados: começam com nome de searcher conhecido (alfanumérico).
    for line in lines[header_idx + 1:]:
        s = line.strip()
        if not s or s.startswith("#"):
            continue
        cols = s.split()
        searcher_name = cols[0]
        if not re.match(r"^[a-z][a-z0-9_]*$", searcher_name):
            continue
        row = {f: "" for f in OUT_FIELDS}
        row["phase"] = phase
        row["log_file"] = os.path.relpath(path)
        if parsed:
            row["patterns"], row["corpus"], _, row["threads_label"], row["tag"] = parsed
        row["searcher"] = searcher_name
        for key, pos in col_index.items():
            if pos < len(cols):
                row[key] = cols[pos]
        for k, v in meta.items():
            row[k] = v
        rows.append(row)
    return rows


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("run_dir", nargs="?", default="runs/i5")
    ap.add_argument("-o", "--output", default=None)
    ap.add_argument("--known-only", action="store_true",
                    help="ignora logs de searchers fora do registry atual")
    ap.add_argument("--include-fail", action="store_true",
                    help="inclui também arquivos *.FAIL")
    args = ap.parse_args()

    out = args.output or os.path.join(args.run_dir, "sweep.csv")
    all_rows = []
    skipped_unknown = 0
    n_files = 0

    for phase in sorted(os.listdir(args.run_dir)):
        pdir = os.path.join(args.run_dir, phase)
        if not os.path.isdir(pdir) or phase == "env":
            continue
        for fn in sorted(os.listdir(pdir)):
            if fn.endswith(".log") or (args.include_fail and fn.endswith(".FAIL")):
                path = os.path.join(pdir, fn)
                n_files += 1
                for row in parse_log(path, phase):
                    if args.known_only and row["searcher"] not in KNOWN_SEARCHERS:
                        skipped_unknown += 1
                        continue
                    all_rows.append(row)

    with open(out, "w", newline="") as fh:
        w = csv.DictWriter(fh, fieldnames=OUT_FIELDS)
        w.writeheader()
        w.writerows(all_rows)

    print(f"[extract] {n_files} logs lidos -> {len(all_rows)} linhas em {out}")
    if args.known_only and skipped_unknown:
        print(f"[extract] {skipped_unknown} linhas de searchers desconhecidos ignoradas")
    searchers = sorted({r["searcher"] for r in all_rows})
    print(f"[extract] searchers no CSV: {', '.join(searchers)}")


if __name__ == "__main__":
    main()
