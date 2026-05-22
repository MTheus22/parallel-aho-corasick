#!/usr/bin/env bash
# =============================================================================
# sweep_ideas_345.sh — Sweeps focados nas ideas 3, 4 e 5 (já implementadas).
# =============================================================================
#
# Produz os dados experimentais que suportam as seções 6-9 do
# tcc_notes/tldr_metricas.md. Três sub-sweeps independentes:
#
#   E — δ² × tamanho do dicionário (idea 3)
#       Mede o crossover onde a tabela δ² passa de cache-friendly
#       para memory-bound. Inclui fallback explícito para dicionários
#       maiores que o guard de 1 GiB.
#
#   F — build-time paralelo vs. sequencial (idea 4)
#       Varre T ∈ {seq, 2, 4, 8, 12} para Snort e ET-32.
#       Reporta apenas build time (warmup=0 iters=1 para minimizar
#       tempo total; o número de interesse é "# build time:" no header).
#
#   G — flat A/B: sequential_flat, pthread_chunked_flat (idea 5)
#       Compara cada novo searcher contra seu irmão chain-walk em
#       corpus canônicos (enron_corpus e simplewiki) × {snort, et_32}.
#
# Uso:
#   scripts/sweep_ideas_345.sh                    # roda E, F, G
#   scripts/sweep_ideas_345.sh E                  # apenas δ²
#   scripts/sweep_ideas_345.sh F                  # apenas build paralelo
#   scripts/sweep_ideas_345.sh G                  # apenas flat A/B
#   PHASES="E G" scripts/sweep_ideas_345.sh       # fases E e G
#   RUN_DIR=runs/teste scripts/sweep_ideas_345.sh
#
# Saída:
#   runs/overnight/E_delta2/
#   runs/overnight/F_build_par/
#   runs/overnight/G_flat_ab/
#
# Extratores pós-execução (exemplos):
#
#   # δ² throughput por dicionário
#   grep -h "^sequential" runs/overnight/E_delta2/*.log | \
#     awk '{printf "%s\t%s\t%s MB/s\n", $1, FILENAME, $7}'
#
#   # build times
#   grep -h "^# build time" runs/overnight/F_build_par/*.log
#
#   # flat A/B diff
#   grep -h "^sequential_flat\|^pthread_chunked_flat\|^sequential\b\|^pthread_chunked_v2" \
#     runs/overnight/G_flat_ab/*.log
# =============================================================================

set -uo pipefail

cd "$(dirname "$0")/.."

BIN=build/aclab
DATA=data
RUN_DIR="${RUN_DIR:-runs/overnight}"

# -------- build se necessário -------------------------------------------------
if [[ ! -x "$BIN" ]]; then
  echo "[setup] $BIN ausente — rodando make"
  make -j
fi

# -------- preparação de dicionários reduzidos ---------------------------------
if [[ ! -s "$DATA/patterns_snort_100.txt" ]]; then
  head -100  "$DATA/patterns_snort.txt" > "$DATA/patterns_snort_100.txt"
fi
if [[ ! -s "$DATA/patterns_snort_1k.txt" ]]; then
  head -1000 "$DATA/patterns_snort.txt" > "$DATA/patterns_snort_1k.txt"
fi

# -------- contadores -----------------------------------------------------------
OK=0
SKIPPED=0
FAILED=0

log() { printf '[%s] %s\n' "$(date '+%H:%M:%S')" "$*"; }

# -------- runner: 1 execução do aclab -----------------------------------------
# args: <phase_dir> <patterns> <corpus> <searcher> <threads> <warmup> <iters>
#       [extra_tag] [extra_env_prefix] [extra_aclab_args...]
run_aclab() {
  local phase="$1" patterns="$2" corpus="$3" searcher="$4" threads="$5"
  local warmup="$6" iters="$7"
  local tag="${8:-}"
  local env_prefix="${9:-}"
  shift 9
  local extra=("$@")

  local pat_short cor_short suffix label outfile tmp
  pat_short=$(basename "$patterns" .txt)
  cor_short=$(basename "$corpus" .txt)
  suffix=""; [[ -n "$tag" ]] && suffix="_${tag}"
  label="${pat_short}__${cor_short}__${searcher}__T${threads}${suffix}"
  outfile="$RUN_DIR/$phase/${label}.log"
  tmp="${outfile}.tmp"
  mkdir -p "$RUN_DIR/$phase"

  if [[ -s "$outfile" ]] && grep -qE "^${searcher}[[:space:]]" "$outfile" 2>/dev/null; then
    log "SKIP  $phase/$label"
    SKIPPED=$((SKIPPED + 1))
    return 0
  fi
  if [[ ! -s "$patterns" ]]; then
    log "FAIL  $phase/$label (patterns ausente: $patterns)"
    FAILED=$((FAILED + 1))
    return 0
  fi
  if [[ ! -s "$corpus"   ]]; then
    log "FAIL  $phase/$label (corpus ausente: $corpus)"
    FAILED=$((FAILED + 1))
    return 0
  fi

  local start elapsed rc
  start=$(date +%s)
  log "RUN   $phase/$label"

  {
    printf '# command: %s %s --patterns %s --input %s --searcher %s --threads %s --warmup %s --iters %s %s\n' \
           "$env_prefix" "$BIN" "$patterns" "$corpus" "$searcher" "$threads" \
           "$warmup" "$iters" "${extra[*]:-}"
    printf '# started: %s\n' "$(date --iso-8601=seconds)"
    printf '# phase:   %s\n' "$phase"
    printf '# label:   %s\n\n' "$label"
  } > "$tmp"

  env $env_prefix "$BIN" --patterns "$patterns" --input "$corpus" \
       --searcher "$searcher" --threads "$threads" \
       --warmup "$warmup" --iters "$iters" \
       "${extra[@]}" >> "$tmp" 2>&1
  rc=$?

  elapsed=$(($(date +%s) - start))

  if [[ $rc -eq 0 ]] && grep -qE "^${searcher}[[:space:]]" "$tmp" 2>/dev/null; then
    { printf '\n# finished:        %s\n' "$(date --iso-8601=seconds)"
      printf '# elapsed_seconds: %s\n# exit_code:       0\n' "$elapsed"; } >> "$tmp"
    mv "$tmp" "$outfile"
    log "OK    $phase/$label (${elapsed}s)"
    OK=$((OK + 1))
  else
    { printf '\n# finished:        %s\n' "$(date --iso-8601=seconds)"
      printf '# elapsed_seconds: %s\n# exit_code:       %s\n' "$elapsed" "$rc"; } >> "$tmp"
    mv "$tmp" "${outfile}.FAIL"
    log "FAIL  $phase/$label (exit=$rc, ${elapsed}s)"
    FAILED=$((FAILED + 1))
  fi
}

# =============================================================================
# Fase E — δ² × tamanho do dicionário (idea 3)
# =============================================================================
phase_E() {
  log "===== Fase E — δ² × tamanho do dicionário (idea 3) ====="
  local phase="E_delta2"
  # Dicionários em ordem crescente de tamanho (estados):
  #   snort_100   ~1939 estados  →  tabela δ² ~640 MiB  (dentro do guard)
  #   snort_1k   ~11974 estados  →  tabela δ² ~3.7 GiB  (fallback)
  #   snort      ~55479 estados  →  tabela δ² ~17 GiB   (fallback)
  #   et_32     ~508896 estados  →  tabela δ² >>1 TiB   (fallback)
  local dicts=(patterns_snort_100.txt patterns_snort_1k.txt
               patterns_snort.txt      patterns_et_32.txt)
  local cor="enron_corpus.txt"

  for pat in "${dicts[@]}"; do
    # Baseline sequencial e flat como referência
    run_aclab "$phase" "$DATA/$pat" "$DATA/$cor" sequential      0 2 5
    run_aclab "$phase" "$DATA/$pat" "$DATA/$cor" sequential_flat 0 2 5
    # δ² — pode imprimir fallback no stderr se footprint > 1 GiB
    run_aclab "$phase" "$DATA/$pat" "$DATA/$cor" sequential_delta2 0 2 5
  done

  # Corpus simplewiki para cruzar com os números do headline benchmark
  # do docs/searchers/sequential_delta2.md (se disponível)
  if [[ -s "$DATA/simplewiki.txt" ]]; then
    for pat in patterns_snort_100.txt patterns_snort.txt; do
      run_aclab "$phase" "$DATA/$pat" "$DATA/simplewiki.txt" sequential       0 2 5 "wiki"
      run_aclab "$phase" "$DATA/$pat" "$DATA/simplewiki.txt" sequential_flat  0 2 5 "wiki"
      run_aclab "$phase" "$DATA/$pat" "$DATA/simplewiki.txt" sequential_delta2 0 2 5 "wiki"
    done
  fi
}

# =============================================================================
# Fase F — build-time paralelo vs. sequencial (idea 4)
# =============================================================================
phase_F() {
  log "===== Fase F — build-time paralelo vs. sequencial (idea 4) ====="
  local phase="F_build_par"
  local dicts=(patterns_snort_100.txt patterns_snort_1k.txt
               patterns_snort.txt      patterns_et_32.txt)
  local cor="enron_corpus.txt"
  # warmup=0 iters=1: minimiza tempo total; o número de interesse é o
  # "# build time:" reportado no header do aclab para cada run.
  local wmup=0 iters=1

  for pat in "${dicts[@]}"; do
    local pshort; pshort=$(basename "$pat" .txt)
    # Build sequencial (baseline)
    run_aclab "$phase" "$DATA/$pat" "$DATA/$cor" sequential 0 "$wmup" "$iters" "buildseq" \
              "AC_BUILD_PARALLEL=0"

    # Build paralelo com T ∈ {2, 4, 8, 12}
    for T in 2 4 8 12; do
      run_aclab "$phase" "$DATA/$pat" "$DATA/$cor" sequential 0 "$wmup" "$iters" "buildpar_T${T}" \
                "AC_BUILD_PARALLEL=1 AC_BUILD_THREADS=${T}"
    done
  done
}

# =============================================================================
# Fase G — flat A/B: sequential_flat e pthread_chunked_flat (idea 5)
# =============================================================================
phase_G() {
  log "===== Fase G — flat A/B (idea 5) ====="
  local phase="G_flat_ab"
  local dicts=(patterns_snort_100.txt patterns_snort_1k.txt
               patterns_snort.txt      patterns_et_32.txt)
  local flat_single=(sequential sequential_flat sequential_delta2)
  local flat_parallel=(pthread_chunked_v2 pthread_chunked_flat
                        pthread_chunked_v3 pthread_affinity)

  for corp in enron_corpus.txt simplewiki.txt; do
    [[ -s "$DATA/$corp" ]] || continue
    for pat in "${dicts[@]}"; do
      # Single-thread: sequential vs sequential_flat
      for s in "${flat_single[@]}"; do
        run_aclab "$phase" "$DATA/$pat" "$DATA/$corp" "$s" 0 2 5
      done
      # Multi-thread: v2 vs flat para T ∈ {1, 4, 8, 12}
      for T in 1 4 8 12; do
        for s in "${flat_parallel[@]}"; do
          run_aclab "$phase" "$DATA/$pat" "$DATA/$corp" "$s" "$T" 2 5
        done
      done
    done
  done

  # Sweep per-thread: pthread_chunked_flat T=12 com --per-thread para análise
  # de balanceamento (análogo à Fase D do sweep noturno principal)
  local cor="enron_corpus.txt"
  for pat in patterns_snort.txt patterns_et_32.txt; do
    run_aclab "$phase" "$DATA/$pat" "$DATA/$cor" \
              pthread_chunked_flat 12 1 3 "perthread" "" --per-thread
    run_aclab "$phase" "$DATA/$pat" "$DATA/$cor" \
              pthread_chunked_v2   12 1 3 "perthread" "" --per-thread
  done
}

# =============================================================================
# Orquestração
# =============================================================================
PHASES="${PHASES:-${1:-E F G}}"

log "=== sweep_ideas_345.sh iniciado ==="
log "RUN_DIR=$RUN_DIR  PHASES=$PHASES"

for p in $PHASES; do
  case "$p" in
    E) phase_E ;;
    F) phase_F ;;
    G) phase_G ;;
    *) log "WARN  fase desconhecida ignorada: $p" ;;
  esac
done

TOTAL=$((OK + FAILED + SKIPPED))
log "=== Concluído: ok=$OK skipped=$SKIPPED failed=$FAILED total=$TOTAL ==="

if (( FAILED > 0 )); then
  echo
  echo "Falhas (rerodar o script reexecuta apenas estas):"
  find "$RUN_DIR" -name '*.FAIL' -printf '  %p\n' 2>/dev/null
fi
