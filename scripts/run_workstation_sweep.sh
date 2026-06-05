#!/usr/bin/env bash
# =============================================================================
# Sweep da workstation reservada — subconjunto relevante do TCC.
# =============================================================================
#
# Alvo: AMD Ryzen 9 9950X (Zen 5), 16 núcleos HOMOGÊNEOS / 32 threads,
#       L3 64 MiB (2 CCDs × 32 MiB, não unificado), DDR5-5600, muita RAM.
#
# Por que este script existe (e difere de run_overnight_sweep.sh):
#   O sweep canônico foi calibrado para o i5-1235U HÍBRIDO (P/E). Em cores
#   homogêneos os searchers topology-aware (pthread_chunked_v3 / v3_flat)
#   perdem o sentido (peso por cpufreq colapsa para uniforme), e o
#   balanceamento de carga relevante passa a ser DINÂMICO (bag of tasks).
#   Esta grade:
#     - descarta v3 / v3_flat;
#     - mantém o bag of tasks (pthread_dynamic) e o candidato a campeão
#       homogêneo pthread_dynamic_flat (bag of tasks + flat, idea 5);
#     - foca o escalonamento em enron_x8 (regime IDS-scale memory-bound);
#     - corta a Fase C (sensibilidade ao corpus, já estabelecida).
#   Justificativa completa: docs/sweep-test-inventory.md.
#
# Reutiliza a infraestrutura resiliente do overnight:
#   - 1 arquivo .log por run em runs/workstation/<fase>/; rerodar PULA o
#     que já completou (resume-from-crash).
#   - falhas individuais viram .FAIL sem derrubar o sweep.
#   - SIGINT/SIGTERM tratados; flock impede instâncias paralelas.
#   - env/start.txt, env/end.txt, env/thermal.tsv para correlacionar
#     throttling com runs.
#
# Plano (fases):
#   A — curva de escalonamento (the headline figure)
#       enron_x8: T ∈ {1,2,4,8,16,24,32} × {snort, et_32}
#       × 4 searchers (v2, chunked_flat, dynamic, dynamic_flat)
#       warmup=2 iters=8 (réplicas maiores p/ apertar variância)
#   B — footprint do autômato vs throughput (cross-over de cache)
#       dicts ∈ {snort_100, snort_1k, snort, et_32}
#       single: {sequential, sequential_flat}; par: {chunked_flat, dynamic_flat}
#       T ∈ {1, MAX_T}, corpus = enron_corpus (footprint é corpus-independente)
#   D — per-thread em T=MAX_T (balanceamento estático vs. dinâmico)
#       {v2, chunked_flat, dynamic, dynamic_flat} em (snort, enron_x8, --per-thread)
#   E — build-time paralelo vs. sequencial (idea 4)
#       dicts × {buildseq, buildpar T∈{2,4,8,16,32}}, corpus = enron_corpus
#   S — (OPCIONAL, fora do default) 2-D + sharding, trabalho futuro
#       {pthread_2d_sharded_chunked, pattern_sharded_prefix} em (snort, enron_x8)
#
# Uso:
#   scripts/run_workstation_sweep.sh                 # roda A B D E
#   scripts/run_workstation_sweep.sh A               # só fase A
#   PHASES="A D" scripts/run_workstation_sweep.sh    # fases A e D
#   PHASES="A B D E S" scripts/run_workstation_sweep.sh   # inclui o opcional
#   MAX_THREADS=32 scripts/run_workstation_sweep.sh  # fixa o teto de threads
#   RUN_DIR=runs/teste scripts/run_workstation_sweep.sh
#
# Antes de iniciar (recomendado, ver pré-flight em docs/sweep-test-inventory.md):
#   sudo cpupower frequency-set -g performance      # Zen 5 = amd-pstate
#   lscpu | grep -E 'CPU\(s\)|Thread|Core|NUMA|L3'  # confirmar 16C/32T + 2 CCD
#   make && make test                               # correção no chip-alvo
#   fechar IDEs/browsers
#   nohup ./scripts/run_workstation_sweep.sh > workstation.out 2>&1 &
# =============================================================================

set -uo pipefail

cd "$(dirname "$0")/.."

BIN=build/aclab
DATA=data
RUN_DIR="${RUN_DIR:-runs/workstation}"
MAX_T="${MAX_THREADS:-$(nproc)}"

# -------- build se necessário ----------------------------------------------
if [[ ! -x "$BIN" ]]; then
  echo "[setup] build/aclab ausente — rodando make"
  make -j
fi

mkdir -p "$RUN_DIR/env"

# -------- lock para impedir instâncias paralelas ---------------------------
LOCK="$RUN_DIR/.lock"
exec 9>"$LOCK"
if ! flock -n 9; then
  echo "ERRO: outra instância do sweep já está rodando (lock em $LOCK)" >&2
  exit 1
fi

# -------- preparação de dicionários reduzidos (fase B/E) -------------------
if [[ ! -s "$DATA/patterns_snort_100.txt" ]]; then
  head -100  "$DATA/patterns_snort.txt" > "$DATA/patterns_snort_100.txt"
fi
if [[ ! -s "$DATA/patterns_snort_1k.txt" ]]; then
  head -1000 "$DATA/patterns_snort.txt" > "$DATA/patterns_snort_1k.txt"
fi

# -------- contadores -------------------------------------------------------
OK=0
SKIPPED=0
FAILED=0
START_TS=$(date +%s)

MASTER="$RUN_DIR/MASTER.log"

log() {
  local ts; ts=$(date '+%Y-%m-%d %H:%M:%S')
  printf '[%s] %s\n' "$ts" "$*" | tee -a "$MASTER"
}

# -------- trap: limpa tmp files se interrompido ----------------------------
cleanup() {
  local sig="${1:-EXIT}"
  find "$RUN_DIR" -name '*.log.tmp' -delete 2>/dev/null || true
  if [[ "$sig" != "EXIT" ]]; then
    log "INTERROMPIDO por $sig — ok=$OK skipped=$SKIPPED failed=$FAILED"
  fi
}
trap 'cleanup INT;  exit 130' INT
trap 'cleanup TERM; exit 143' TERM
trap 'cleanup EXIT' EXIT

# -------- gerador de lista de threads --------------------------------------
# Filtra candidatos <= MAX_T e garante que MAX_T entra. Mantém a grade
# adaptável caso o script rode num chip com menos de 32 threads.
gen_threads() {
  local out=() t has=0
  for t in "$@"; do (( t <= MAX_T )) && out+=("$t"); done
  for t in "${out[@]:-}"; do [[ "$t" -eq "$MAX_T" ]] && has=1; done
  (( has == 0 )) && out+=("$MAX_T")
  printf '%s\n' "${out[@]}" | sort -n -u | tr '\n' ' '
}

# -------- snapshot de ambiente ---------------------------------------------
snapshot_env() {
  local tag="$1"
  local f="$RUN_DIR/env/${tag}.txt"
  {
    echo "=== $tag $(date --iso-8601=seconds) ==="
    echo
    echo "--- uname -a ---"; uname -a
    echo
    echo "--- governor ---"
    cat /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor 2>/dev/null | sort -u
    echo
    echo "--- cpufreq atual (cpu0..cpuN MHz) ---"; grep -E '^cpu MHz' /proc/cpuinfo
    echo
    echo "--- lscpu (resumido) ---"
    lscpu | grep -E 'Architecture|Model name|CPU\(s\)|Thread|Core|Socket|NUMA|MHz|cache|L3'
    echo
    echo "--- topologia (CCD/cache) ---"
    lscpu -C 2>/dev/null || true
    echo
    echo "--- thermal zones ---"
    for z in /sys/class/thermal/thermal_zone*/temp; do
      printf '  %s: %s mC\n' "$z" "$(cat "$z" 2>/dev/null || echo n/a)"
    done
    echo
    echo "--- free -h ---"; free -h
    echo
    echo "--- loadavg ---"; cat /proc/loadavg
    echo
    echo "--- aclab commit ---"
    (git log -1 --oneline 2>/dev/null || echo "no git")
  } > "$f"
  log "snapshot env -> $f"
}

thermal_snapshot() {
  local label="$1"
  local ts; ts=$(date '+%H:%M:%S')
  local zones=""
  for z in /sys/class/thermal/thermal_zone*/temp; do
    zones+=$(cat "$z" 2>/dev/null || echo 0)
    zones+=$'\t'
  done
  local mhz; mhz=$(awk '/^cpu MHz/ {sum+=$4; n++} END {if (n>0) printf "%.0f", sum/n}' /proc/cpuinfo)
  printf '%s\t%s\t%s%s\n' "$ts" "$label" "$zones" "$mhz" >> "$RUN_DIR/env/thermal.tsv"
}

# -------- runner: 1 execução do aclab --------------------------------------
# args: <phase_dir> <patterns> <corpus> <searcher> <threads> <warmup> <iters> [tag] [extra_args...]
run_aclab() {
  local phase="$1" patterns="$2" corpus="$3" searcher="$4" threads="$5"
  local warmup="$6" iters="$7"
  shift 7
  local tag="${1:-}"
  [[ $# -gt 0 ]] && shift
  local extra=("$@")

  local pat_short cor_short label outfile tmp suffix
  pat_short=$(basename "$patterns" .txt)
  cor_short=$(basename "$corpus" .txt)
  suffix=""
  [[ -n "$tag" ]] && suffix="_${tag}"
  label="${pat_short}__${cor_short}__${searcher}__T${threads}${suffix}"
  outfile="$RUN_DIR/$phase/${label}.log"
  tmp="${outfile}.tmp"
  mkdir -p "$RUN_DIR/$phase"

  if [[ -s "$outfile" ]] && grep -qE "^${searcher}[[:space:]]" "$outfile"; then
    log "SKIP  $phase/$label (já completo)"
    SKIPPED=$((SKIPPED + 1))
    return 0
  fi

  if [[ ! -s "$patterns" ]]; then
    log "FAIL  $phase/$label (patterns ausente: $patterns)"
    FAILED=$((FAILED + 1)); return 0
  fi
  if [[ ! -s "$corpus" ]]; then
    log "FAIL  $phase/$label (corpus ausente: $corpus)"
    FAILED=$((FAILED + 1)); return 0
  fi

  local start elapsed rc
  start=$(date +%s)
  log "RUN   $phase/$label"
  thermal_snapshot "pre:$label"

  {
    echo "# command: $BIN --patterns $patterns --input $corpus --searcher $searcher --threads $threads --warmup $warmup --iters $iters ${extra[*]:-}"
    echo "# started: $(date --iso-8601=seconds)"
    echo "# phase:   $phase"
    echo "# label:   $label"
    echo
  } > "$tmp"

  "$BIN" --patterns "$patterns" --input "$corpus" \
         --searcher "$searcher" --threads "$threads" \
         --warmup "$warmup" --iters "$iters" \
         "${extra[@]}" >> "$tmp" 2>&1
  rc=$?

  elapsed=$(($(date +%s) - start))

  if [[ $rc -eq 0 ]] && grep -qE "^${searcher}[[:space:]]" "$tmp"; then
    { echo; echo "# finished:        $(date --iso-8601=seconds)"
      echo "# elapsed_seconds: $elapsed"; echo "# exit_code:       0"; } >> "$tmp"
    mv "$tmp" "$outfile"
    log "OK    $phase/$label (${elapsed}s)"
    OK=$((OK + 1))
  else
    { echo; echo "# finished:        $(date --iso-8601=seconds)"
      echo "# elapsed_seconds: $elapsed"; echo "# exit_code:       $rc"; } >> "$tmp"
    mv "$tmp" "${outfile}.FAIL"
    log "FAIL  $phase/$label (exit=$rc, ${elapsed}s)"
    FAILED=$((FAILED + 1))
  fi
  thermal_snapshot "post:$label"
}

# Conjunto-núcleo de searchers paralelos (sem v3 / v3_flat: cores homogêneos).
PARALLEL_CORE=(pthread_chunked_v2 pthread_chunked_flat
               pthread_dynamic pthread_dynamic_flat)

# =============================================================================
# Fase A — curva de escalonamento (enron_x8)
# =============================================================================
phase_A() {
  log "===== Fase A — escalonamento (enron_x8) ====="
  local phase="A_speedup_curves"
  local cor="enron_x8.txt"
  local Ts; read -ra Ts <<< "$(gen_threads 1 2 4 8 16 24 32)"
  log "Fase A: T = ${Ts[*]}"

  for pat in patterns_snort.txt patterns_et_32.txt; do
    # baselines single-thread (denominador de speedup)
    run_aclab "$phase" "$DATA/$pat" "$DATA/$cor" sequential      1 2 8
    run_aclab "$phase" "$DATA/$pat" "$DATA/$cor" sequential_flat 1 2 8
    for T in "${Ts[@]}"; do
      for s in "${PARALLEL_CORE[@]}"; do
        run_aclab "$phase" "$DATA/$pat" "$DATA/$cor" "$s" "$T" 2 8
      done
    done
  done
}

# =============================================================================
# Fase B — footprint do autômato (cross-over de cache)
# =============================================================================
phase_B() {
  log "===== Fase B — footprint sweep (enron_corpus) ====="
  local phase="B_footprint"
  local cor="enron_corpus.txt"
  local dicts=(patterns_snort_100.txt patterns_snort_1k.txt patterns_snort.txt patterns_et_32.txt)
  local seq_searchers=(sequential sequential_flat)
  # estático-flat vs dinâmico-flat, ambos campeões do seu eixo
  local par_searchers=(pthread_chunked_flat pthread_dynamic_flat)

  for pat in "${dicts[@]}"; do
    for s in "${seq_searchers[@]}"; do
      run_aclab "$phase" "$DATA/$pat" "$DATA/$cor" "$s" 1 2 5
    done
    for s in "${par_searchers[@]}"; do
      for T in 1 "$MAX_T"; do
        run_aclab "$phase" "$DATA/$pat" "$DATA/$cor" "$s" "$T" 2 5
      done
    done
  done
}

# =============================================================================
# Fase D — per-thread em T=MAX_T (balanceamento estático vs. dinâmico)
# =============================================================================
phase_D() {
  log "===== Fase D — per-thread (T=$MAX_T, enron_x8) ====="
  local phase="D_per_thread"
  local pat="patterns_snort.txt"
  local cor="enron_x8.txt"
  for s in "${PARALLEL_CORE[@]}"; do
    run_aclab "$phase" "$DATA/$pat" "$DATA/$cor" "$s" "$MAX_T" 1 3 "perthread" --per-thread
  done
}

# =============================================================================
# Fase E — build paralelo vs. sequencial (idea 4)
# =============================================================================
phase_E() {
  log "===== Fase E — build-time paralelo vs. sequencial (idea 4) ====="
  local phase="E_build_par"
  local cor="enron_corpus.txt"
  local dicts=(patterns_snort_100.txt patterns_snort_1k.txt
               patterns_snort.txt      patterns_et_32.txt)
  local Ts_build; read -ra Ts_build <<< "$(gen_threads 2 4 8 16 32)"
  log "Fase E: build T = ${Ts_build[*]}"

  for pat in "${dicts[@]}"; do
    run_aclab "$phase" "$DATA/$pat" "$DATA/$cor" sequential 1 0 1 "buildseq"
    for T in "${Ts_build[@]}"; do
      (
        export AC_BUILD_PARALLEL=1
        export AC_BUILD_THREADS="$T"
        run_aclab "$phase" "$DATA/$pat" "$DATA/$cor" sequential 1 0 1 "buildpar_T${T}"
      )
    done
  done
}

# =============================================================================
# Fase S — OPCIONAL: 2-D + sharding (trabalho futuro da conclusão)
# =============================================================================
phase_S() {
  log "===== Fase S (opcional) — 2-D / sharding (enron_x8) ====="
  local phase="S_2d_sharding"
  local pat="patterns_snort.txt"
  local cor="enron_x8.txt"
  for s in pthread_2d_sharded_chunked pattern_sharded_prefix; do
    for T in 1 "$MAX_T"; do
      run_aclab "$phase" "$DATA/$pat" "$DATA/$cor" "$s" "$T" 2 5
    done
  done
}

# =============================================================================
# Orquestração
# =============================================================================
PHASES="${PHASES:-${1:-A B D E}}"

log "=== Sweep workstation iniciado ==="
log "RUN_DIR=$RUN_DIR  PHASES=$PHASES  MAX_T=$MAX_T"
snapshot_env start

for p in $PHASES; do
  case "$p" in
    A) phase_A ;;
    B) phase_B ;;
    D) phase_D ;;
    E) phase_E ;;
    S) phase_S ;;
    *) log "WARN  fase desconhecida ignorada: $p" ;;
  esac
done

snapshot_env end
ELAPSED=$(($(date +%s) - START_TS))
HRS=$((ELAPSED / 3600))
MIN=$(((ELAPSED % 3600) / 60))
log "=== Sweep workstation concluído ==="
log "Resumo: ok=$OK skipped=$SKIPPED failed=$FAILED  duração=${HRS}h${MIN}m"

if (( FAILED > 0 )); then
  echo
  echo "Falhas (rerodar o script reexecuta apenas estas):"
  find "$RUN_DIR" -name '*.FAIL' -printf '  %p\n'
fi
