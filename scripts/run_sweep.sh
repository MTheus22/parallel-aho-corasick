#!/usr/bin/env bash
# =============================================================================
# Motor de sweep UNIFICADO e env-agnóstico — coleta para o TCC.
# =============================================================================
#
# Promove o antigo `run_i5_sweep.sh` a um único script que roda a **totalidade
# da grade** (fases A–G, 10 searchers paralelos + 2 sequenciais) em **qualquer
# ambiente**, derivando o teto de threads e o nome do diretório de saída do
# hardware corrente. Nenhuma suposição de hardware híbrido (P/E) é embutida.
#
# Inventário exato da grade (em função de MAX_T): docs/sweep-test-inventory.md.
#
# Resiliente:
#   - Cada run grava em um arquivo próprio em <RUN_DIR>/<fase>/.
#   - Rerodar o script PULA runs já concluídas (resume-from-crash).
#   - Falhas individuais não derrubam as demais runs; ficam marcadas com sufixo
#     .FAIL e fazem o processo terminar com rc=1 ao final.
#   - SIGINT / SIGTERM são tratados: tmp files limpos, MASTER.log finalizado.
#   - flock impede duas instâncias simultâneas (mediria com ruído).
#
# Diretório de saída (RUN_DIR):
#   - Se RUN_DIR vier do chamador, é respeitado tal e qual.
#   - Senão, é derivado do modelo de CPU (`lscpu` → slug), ex.:
#       runs/amd_ryzen_9_9950x, runs/intel_core_i5_1235u.
#     Fallback (sem lscpu/Model name): runs/<hostname>.
#   - Os diretórios canônicos `runs/i5` e `runs/workstation` NUNCA são
#     sobrescritos por engano: se o slug colidir com um deles e ele estiver
#     não-vazio, anexa-se a data (runs/<slug>_AAAA-MM-DD) e avisa no MASTER.log.
#
# Estrutura de saída:
#   <RUN_DIR>/
#     MASTER.log                # event log timestampado (append-only)
#     env/start.txt             # lscpu, governor, free, /proc/cpuinfo MHz
#     env/end.txt               # idem no fim do sweep
#     env/thermal.tsv           # snapshot por run: ts \t zone0_temp \t ... \t cpu0_MHz
#     A_speedup_curves/<run>.log
#     B_footprint/<run>.log
#     C_cross_corpus/<run>.log
#     D_per_thread/<run>.log
#     E_build_par/<run>.log
#     G_granularity/<run>.log
#
# Convenção de nome dos .log:
#   <patterns_basename>__<corpus_basename>__<searcher>__T<n>[_<tag>].log
#
# Plano (fases) — pontos de thread escalam com MAX_T (nproc), não fixos em 12:
#   A — curva completa por searcher (the headline figure)
#       enron_corpus: T derivado de MAX_T (passo +2)  × {snort, et_32} × 10 searchers
#       enron_x8:     T derivado de MAX_T (dobrando)   × {snort, et_32} × 10 searchers
#       warmup=2 iters=5
#   B — footprint do autômato vs throughput
#       dicts ∈ {snort_100, snort_1k, snort, et_32} × {seq, v3, flat, dynamic, dynamic_flat}
#       T ∈ {1, MAX_T}, corpus = enron_corpus
#   C — sensibilidade ao corpus
#       (snort) × {simplewiki, enron_corpus} × {seq, seq_flat, v3, flat, dynamic, dynamic_flat}
#       T ∈ {1, MAX_T} para searchers paralelos
#   D — per-thread em T=MAX_T (para tabela de balanceamento)
#       1 run por searcher paralelo em (snort, enron_corpus, T=MAX_T, --per-thread)
#   E — build-time paralelo vs. sequencial (idea 4)
#       dicts ∈ {snort_100, snort_1k, snort, et_32}, build seq vs. par (dobrando até MAX_T)
#   G — granularidade da fila dinâmica
#       AC_DYN_TASKS_PER_THREAD ∈ {1,4,16,64,256} × {dynamic, dynamic_flat}
#       (snort, enron_corpus, T=MAX_T); timing + --per-thread por k
#
# Uso:
#   scripts/run_sweep.sh                       # default (A B C D E G), RUN_DIR auto
#   scripts/run_sweep.sh A                     # roda só fase A
#   PHASES="A C" scripts/run_sweep.sh          # roda fases A e C
#   PHASES="G" scripts/run_sweep.sh            # só o sweep de granularidade (P1)
#   RUN_DIR=runs/teste scripts/run_sweep.sh    # dir custom (override)
#   MAX_THREADS=32 scripts/run_sweep.sh        # teto de threads custom (default nproc)
#   THREAD_POINTS="1 2 4 8 16 24 32" scripts/run_sweep.sh  # pontos de curva custom
#
# Antes de iniciar (governador etc. ficam a cargo do wrapper "um comando";
# aqui só registramos o ambiente em env/start.txt):
#   sudo cpupower frequency-set -g performance   # (fora deste script)
#   fechar IDEs/browsers
#   nohup ./scripts/run_sweep.sh > sweep.out 2>&1 &
#
# Análise pós-execução:
#   Cada .log começa com header `# command: ...` e `# started: ...`, depois
#   contém a saída completa do aclab. A linha de resultado é a do searcher
#   na tabela final. Extração: scripts/extract_sweep_csv.py + build_sweep_db.py.
# =============================================================================

set -uo pipefail

cd "$(dirname "$0")/.."

BIN=build/aclab
DATA="${DATA:-data}"
MAX_T="${MAX_THREADS:-$(nproc)}"
THREAD_POINTS="${THREAD_POINTS:-}"

# -------- auto-nomeação do ambiente (quando RUN_DIR não vier do chamador) ----
# Deriva um slug do modelo de CPU; fallback para hostname. Não embute nenhuma
# suposição de hardware (P/E, contagem fixa de threads) — apenas um rótulo.
cpu_slug() {
  local model
  model=$(lscpu 2>/dev/null | awk -F: '/Model name/{sub(/^[ \t]+/,"",$2); print $2; exit}')
  if [[ -z "$model" ]]; then
    hostname -s 2>/dev/null || echo "host"
    return
  fi
  printf '%s\n' "$model" \
    | tr '[:upper:]' '[:lower:]' \
    | sed -E 's/\(r\)|\(tm\)//g
              s/ [0-9]+-core processor//
              s/ processor//
              s/ cpu//
              s/ @.*$//
              s/[^a-z0-9]+/_/g
              s/^_+|_+$//g'
}

COLLISION_NOTE=""
if [[ -z "${RUN_DIR:-}" ]]; then
  SLUG=$(cpu_slug)
  [[ -z "$SLUG" ]] && SLUG=$(hostname -s 2>/dev/null || echo host)
  RUN_DIR="runs/$SLUG"
  # Guarda defensiva: nunca sobrescrever os dirs canônicos não-vazios.
  case "$RUN_DIR" in
    runs/i5|runs/workstation)
      if [[ -n "$(ls -A "$RUN_DIR" 2>/dev/null)" ]]; then
        RUN_DIR="runs/${SLUG}_$(date +%F)"
        COLLISION_NOTE="slug colidiu com dir canônico reservado e não-vazio; usando $RUN_DIR"
      fi
      ;;
  esac
fi

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

# -------- preparação de dicionários reduzidos (fase B) ---------------------
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
  # Single source of truth para eventos do sweep.
  local ts; ts=$(date '+%Y-%m-%d %H:%M:%S')
  printf '[%s] %s\n' "$ts" "$*" | tee -a "$MASTER"
}

# -------- trap: limpa tmp files se interrompido ----------------------------
cleanup() {
  local sig="${1:-EXIT}"
  # Remove tmp files de runs que foram interrompidas no meio.
  find "$RUN_DIR" -name '*.log.tmp' -delete 2>/dev/null || true
  if [[ "$sig" != "EXIT" ]]; then
    log "INTERROMPIDO por $sig — ok=$OK skipped=$SKIPPED failed=$FAILED"
  fi
}
trap 'cleanup INT;  exit 130' INT
trap 'cleanup TERM; exit 143' TERM
trap 'cleanup EXIT' EXIT

# -------- snapshot de ambiente ---------------------------------------------
snapshot_env() {
  local tag="$1"  # "start" ou "end"
  local f="$RUN_DIR/env/${tag}.txt"
  {
    echo "=== $tag $(date --iso-8601=seconds) ==="
    echo
    echo "--- uname -a ---"
    uname -a
    echo
    echo "--- governor ---"
    cat /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor 2>/dev/null | sort -u
    echo
    echo "--- cpufreq atual (cpu0..cpuN MHz) ---"
    grep -E '^cpu MHz' /proc/cpuinfo
    echo
    echo "--- lscpu (resumido) ---"
    lscpu | grep -E 'Architecture|Model name|CPU\(s\)|Thread|Core|Socket|MHz|cache'
    echo
    echo "--- thermal zones ---"
    for z in /sys/class/thermal/thermal_zone*/temp; do
      printf '  %s: %s mC\n' "$z" "$(cat "$z" 2>/dev/null || echo n/a)"
    done
    echo
    echo "--- free -h ---"
    free -h
    echo
    echo "--- loadavg ---"
    cat /proc/loadavg
    echo
    echo "--- aclab commit ---"
    (cd "$(pwd)" && git log -1 --oneline 2>/dev/null || echo "no git")
  } > "$f"
  log "snapshot env -> $f"
}

# Sample mais leve para correlacionar throttling com runs específicas.
# Best-effort: se /sys/class/thermal não existir, a glob não casa e o campo
# de zonas fica vazio — não aborta.
thermal_snapshot() {
  local label="$1"
  local ts; ts=$(date '+%H:%M:%S')
  local zones=""
  for z in /sys/class/thermal/thermal_zone*/temp; do
    [[ -e "$z" ]] || continue
    zones+=$(cat "$z" 2>/dev/null || echo 0)
    zones+=$'\t'
  done
  local mhz; mhz=$(awk '/^cpu MHz/ {sum+=$4; n++} END {if (n>0) printf "%.0f", sum/n}' /proc/cpuinfo)
  printf '%s\t%s\t%s%s\n' "$ts" "$label" "$zones" "$mhz" >> "$RUN_DIR/env/thermal.tsv"
}

# -------- pontos de thread -------------------------------------------------
# Se THREAD_POINTS for fornecido pelo chamador, ele substitui a derivação por
# MAX_T em TODAS as curvas (filtrado para 1..MAX_T, único e ordenado).
thread_points_override() {
  local out=() t seen=" "
  for t in $THREAD_POINTS; do
    [[ "$t" =~ ^[0-9]+$ ]] || continue
    (( t >= 1 && t <= MAX_T )) || continue
    [[ "$seen" == *" $t "* ]] && continue
    out+=("$t"); seen+="$t "
  done
  # ordena numericamente
  printf '%s\n' "${out[@]}" | sort -n | tr '\n' ' '
}

# -------- runner: 1 execução do aclab --------------------------------------
# args: <phase_dir> <patterns> <corpus> <searcher> <threads> <warmup> <iters> [extra_tag] [extra_args...]
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

  # Resume: arquivo .log existente e contendo a linha de resultado do searcher
  # significa que a run foi concluída — pula.
  if [[ -s "$outfile" ]] && grep -qE "^${searcher}[[:space:]]" "$outfile"; then
    log "SKIP  $phase/$label (já completo)"
    SKIPPED=$((SKIPPED + 1))
    return 0
  fi

  # Sanity checks
  if [[ ! -s "$patterns" ]]; then
    log "FAIL  $phase/$label (patterns ausente: $patterns)"
    FAILED=$((FAILED + 1))
    return 0
  fi
  if [[ ! -s "$corpus" ]]; then
    log "FAIL  $phase/$label (corpus ausente: $corpus)"
    FAILED=$((FAILED + 1))
    return 0
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
    {
      echo
      echo "# finished:        $(date --iso-8601=seconds)"
      echo "# elapsed_seconds: $elapsed"
      echo "# exit_code:       0"
    } >> "$tmp"
    mv "$tmp" "$outfile"
    log "OK    $phase/$label (${elapsed}s)"
    OK=$((OK + 1))
  else
    {
      echo
      echo "# finished:        $(date --iso-8601=seconds)"
      echo "# elapsed_seconds: $elapsed"
      echo "# exit_code:       $rc"
    } >> "$tmp"
    mv "$tmp" "${outfile}.FAIL"
    log "FAIL  $phase/$label (exit=$rc, ${elapsed}s)"
    FAILED=$((FAILED + 1))
  fi
  thermal_snapshot "post:$label"
}

# =============================================================================
# Fase A — curva completa por searcher
# =============================================================================
phase_A() {
  log "===== Fase A — speedup curves ====="
  local phase="A_speedup_curves"
  local parallel=(pthread_chunked pthread_chunked_v2 pthread_chunked_v3
                  pthread_dynamic
                  pthread_dynamic_flat
                  pthread_prefetch
                  pthread_chunked_flat        # idea 5
                  pthread_chunked_v3_flat     # ideas 5+7
                  pthread_2d_sharded_chunked  # idea 6
                  pattern_sharded_prefix)     # idea 1

  # A1: enron_corpus (1.42 GiB) — curva dinâmica até MAX_T (passo +2)
  local Ts_corpus
  if [[ -n "$THREAD_POINTS" ]]; then
    read -ra Ts_corpus <<< "$(thread_points_override)"
  else
    Ts_corpus=(1)
    for ((i=2; i<MAX_T; i+=2)); do Ts_corpus+=($i); done
    [[ ${Ts_corpus[-1]} -ne $MAX_T ]] && Ts_corpus+=($MAX_T)
  fi

  # A2: enron_x8 (10.59 GiB) — curva reduzida (dobrando até MAX_T)
  local Ts_x8
  if [[ -n "$THREAD_POINTS" ]]; then
    read -ra Ts_x8 <<< "$(thread_points_override)"
  else
    Ts_x8=(1)
    for ((i=4; i<MAX_T; i*=2)); do Ts_x8+=($i); done
    [[ ${Ts_x8[-1]} -ne $MAX_T ]] && Ts_x8+=($MAX_T)
  fi

  for combo in "patterns_snort.txt:enron_corpus.txt" \
               "patterns_et_32.txt:enron_corpus.txt"; do
    local pat="${combo%%:*}" cor="${combo##*:}"
    # sequential baselines (chain-walk e flat — idea 5)
    run_aclab "$phase" "$DATA/$pat" "$DATA/$cor" sequential      0 2 5
    run_aclab "$phase" "$DATA/$pat" "$DATA/$cor" sequential_flat 0 2 5
    for T in "${Ts_corpus[@]}"; do
      for s in "${parallel[@]}"; do
        run_aclab "$phase" "$DATA/$pat" "$DATA/$cor" "$s" "$T" 2 5
      done
    done
  done

  for combo in "patterns_snort.txt:enron_x8.txt" \
               "patterns_et_32.txt:enron_x8.txt"; do
    local pat="${combo%%:*}" cor="${combo##*:}"
    run_aclab "$phase" "$DATA/$pat" "$DATA/$cor" sequential      0 2 5
    run_aclab "$phase" "$DATA/$pat" "$DATA/$cor" sequential_flat 0 2 5
    for T in "${Ts_x8[@]}"; do
      for s in "${parallel[@]}"; do
        run_aclab "$phase" "$DATA/$pat" "$DATA/$cor" "$s" "$T" 2 5
      done
    done
  done
}

# =============================================================================
# Fase B — footprint do autômato
# =============================================================================
phase_B() {
  log "===== Fase B — footprint sweep ====="
  local phase="B_footprint"
  local dicts=(patterns_snort_100.txt patterns_snort_1k.txt patterns_snort.txt patterns_et_32.txt)
  # single-thread baselines
  local seq_searchers=(sequential sequential_flat)
  # parallel searchers (incluindo flat — idea 5)
  local par_searchers=(pthread_chunked_v3 pthread_chunked_flat
                       pthread_dynamic pthread_dynamic_flat)
  local cor="enron_corpus.txt"

  for pat in "${dicts[@]}"; do
    # Single-thread: sequential, sequential_flat
    for s in "${seq_searchers[@]}"; do
      run_aclab "$phase" "$DATA/$pat" "$DATA/$cor" "$s" 0 2 5
    done
    # Multi-thread: T ∈ {1, MAX_T}
    for s in "${par_searchers[@]}"; do
      for T in 1 "$MAX_T"; do
        run_aclab "$phase" "$DATA/$pat" "$DATA/$cor" "$s" "$T" 2 5
      done
    done
  done
}

# =============================================================================
# Fase C — sensibilidade ao corpus
# =============================================================================
phase_C() {
  log "===== Fase C — cross-corpus ====="
  local phase="C_cross_corpus"
  local seq_searchers=(sequential sequential_flat)
  local par_searchers=(pthread_chunked_v3 pthread_chunked_flat
                       pthread_dynamic pthread_dynamic_flat)
  local pat="patterns_snort.txt"

  for cor in "simplewiki.txt" "enron_corpus.txt"; do
    for s in "${seq_searchers[@]}"; do
      run_aclab "$phase" "$DATA/$pat" "$DATA/$cor" "$s" 0 2 5
    done
    for s in "${par_searchers[@]}"; do
      for T in 1 "$MAX_T"; do
        run_aclab "$phase" "$DATA/$pat" "$DATA/$cor" "$s" "$T" 2 5
      done
    done
  done
}

# =============================================================================
# Fase D — per-thread em T=MAX_T (diagnóstico de balanceamento)
# =============================================================================
phase_D() {
  log "===== Fase D — per-thread (T=$MAX_T) ====="
  local phase="D_per_thread"
  local parallel=(pthread_chunked pthread_chunked_v2 pthread_chunked_v3
                  pthread_dynamic
                  pthread_dynamic_flat
                  pthread_prefetch
                  pthread_chunked_flat        # idea 5
                  pthread_chunked_v3_flat     # ideas 5+7
                  pthread_2d_sharded_chunked  # idea 6
                  pattern_sharded_prefix)     # idea 1
  local pat="patterns_snort.txt"
  local cor="enron_corpus.txt"

  for s in "${parallel[@]}"; do
    run_aclab "$phase" "$DATA/$pat" "$DATA/$cor" "$s" "$MAX_T" 1 3 "perthread" --per-thread
  done
}

# =============================================================================
# Fase E — build-time paralelo vs. sequencial (idea 4)
# =============================================================================
phase_E() {
  log "===== Fase E — build-time paralelo vs. sequencial (idea 4) ====="
  local phase="E_build_par"
  local dicts=(patterns_snort_100.txt patterns_snort_1k.txt
               patterns_snort.txt      patterns_et_32.txt)
  # Usa enron_corpus como corpus (mas warmup=0, iters=1 — só nos interessa
  # o "# build time:" no header do log). O searcher é sempre "sequential"
  # porque queremos isolar o custo de build, não de busca.
  local cor="enron_corpus.txt"

  for pat in "${dicts[@]}"; do
    # Build sequencial
    run_aclab "$phase" "$DATA/$pat" "$DATA/$cor" sequential 0 0 1 "buildseq"
    # Build paralelo: dobrando até MAX_T (ou THREAD_POINTS override)
    local Ts_build
    if [[ -n "$THREAD_POINTS" ]]; then
      read -ra Ts_build <<< "$(thread_points_override)"
    else
      Ts_build=()
      for ((i=2; i<=MAX_T; i*=2)); do Ts_build+=($i); done
      [[ ${#Ts_build[@]} -eq 0 || ${Ts_build[-1]} -ne $MAX_T ]] && Ts_build+=($MAX_T)
    fi

    for T in "${Ts_build[@]}"; do
      # Passa variáveis de ambiente via env dentro de run_aclab.
      # Sobrescrevemos temporariamente AC_BUILD_PARALLEL e AC_BUILD_THREADS.
      (
        export AC_BUILD_PARALLEL=1
        export AC_BUILD_THREADS="$T"
        run_aclab "$phase" "$DATA/$pat" "$DATA/$cor" sequential 0 0 1 "buildpar_T${T}"
      )
    done
  done
}

# =============================================================================
# Fase G — granularidade da fila dinâmica (tasks-per-thread)
# =============================================================================
# Responde à pergunta em aberto do P1 ("tarefas menores ajudariam a dinâmica?").
# Varre AC_DYN_TASKS_PER_THREAD ∈ {1,4,16,64,256} (num_tasks = k·T) para os dois
# searchers de dispatch dinâmico, no regime headline (Snort + enron_corpus,
# T=MAX_T). Duas passadas por k: (i) timing → curva vazão × k; (ii) --per-thread
# → spread de tempo entre workers (o que a granularidade de fato rebalanceia).
#
# Depende do P0 (binário lê --tasks-per-thread / AC_DYN_TASKS_PER_THREAD; o valor
# efetivo aparece no header do log como `tasks_per_thread=N`).
phase_G() {
  log "===== Fase G — granularidade tasks-per-thread (T=$MAX_T) ====="
  local phase="G_granularity"
  local searchers=(pthread_dynamic pthread_dynamic_flat)
  local pat="patterns_snort.txt"
  local cor="enron_corpus.txt"
  local tpts=(1 4 16 64 256)

  for s in "${searchers[@]}"; do
    for k in "${tpts[@]}"; do
      local ktag; ktag="tpt$(printf '%03d' "$k")"   # tpt001..tpt256 (ordena bem)
      # (i) timing: ponto da curva vazão × tasks/thread
      (
        export AC_DYN_TASKS_PER_THREAD="$k"
        run_aclab "$phase" "$DATA/$pat" "$DATA/$cor" "$s" "$MAX_T" 2 5 "$ktag"
      )
      # (ii) per-thread: spread de tempo entre workers nesse k
      (
        export AC_DYN_TASKS_PER_THREAD="$k"
        run_aclab "$phase" "$DATA/$pat" "$DATA/$cor" "$s" "$MAX_T" 1 3 "${ktag}_perthread" --per-thread
      )
    done
  done
}

# =============================================================================
# Orquestração
# =============================================================================
PHASES="${PHASES:-${1:-A B C D E G}}"

log "=== Sweep iniciado ==="
log "RUN_DIR=$RUN_DIR  MAX_T=$MAX_T  PHASES=$PHASES"
[[ -n "$THREAD_POINTS" ]] && log "THREAD_POINTS override=$(thread_points_override)"
[[ -n "$COLLISION_NOTE" ]] && log "WARN  $COLLISION_NOTE"
snapshot_env start

for p in $PHASES; do
  case "$p" in
    A) phase_A ;;
    B) phase_B ;;
    C) phase_C ;;
    D) phase_D ;;
    E) phase_E ;;
    G) phase_G ;;
    *) log "WARN  fase desconhecida ignorada: $p" ;;
  esac
done

snapshot_env end
ELAPSED=$(($(date +%s) - START_TS))
HRS=$((ELAPSED / 3600))
MIN=$(((ELAPSED % 3600) / 60))
log "=== Sweep concluído ==="
log "Resumo: ok=$OK skipped=$SKIPPED failed=$FAILED  duração=${HRS}h${MIN}m"

# Atalho rápido para o usuário ao acordar:
if (( FAILED > 0 )); then
  echo
  echo "Falhas (rerodar o script reexecuta apenas estas):"
  find "$RUN_DIR" -name '*.FAIL' -printf '  %p\n'
  exit 1
fi
