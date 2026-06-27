#!/usr/bin/env bash
# =============================================================================
# i5_all.sh — corrida única "um comando" no i5-1235U.
# =============================================================================
# Espelha workstation_all.sh, mas para o i5: envolve o motor canônico
# scripts/run_i5_sweep.sh (fases A–E, resume, lock, snapshots de env).
#
# Fluxo: pré-flight de dados -> máquina-quieta -> governador -> build
#        -> make test -> sanidade de env  (tudo em FOREGROUND, você assistindo)
#        -> RELANÇA o sweep LONGO em BACKGROUND, imune a:
#            - suspensão / idle           (systemd-inhibit)
#            - logout / fim da sessão TTY (setsid + </dev/null + redirecionamento)
#
# Pensado para o protocolo de máquina-quieta do TCC:
#   reboot -> Ctrl+Alt+F3 (TTY texto, sem GUI) -> login ->
#   cd .../parallel-aho-corasick -> ./scripts/i5_all.sh
#
# NÃO precisa instalar nada: setsid/systemd-inhibit/flock já vêm no Ubuntu.
# Sudo é OPCIONAL (só para o governador de frequência).
#
# Uso:
#   ./scripts/i5_all.sh                      # roda fases A B C D E (~6h30)
#   PHASES="A D" ./scripts/i5_all.sh         # subconjunto de fases
#   RUN_DIR=runs/i5_granularidade \
#     PHASES="A D" AC_DYN_TASKS_PER_THREAD=64 ./scripts/i5_all.sh
#                                            # ex.: sweep de granularidade (P0/P1)
#
# Variáveis de ambiente (incl. AC_DYN_TASKS_PER_THREAD, AC_BUILD_*, RUN_DIR,
# MAX_THREADS) são HERDADAS pelo processo desacoplado — defina-as na chamada.
# =============================================================================
set -uo pipefail

# Caminho absoluto deste script ANTES de qualquer cd (necessário p/ o re-exec).
SELF="$(cd "$(dirname "$0")" && pwd)/$(basename "$0")"
cd "$(dirname "$SELF")/.."          # raiz do repo
ROOT="$(pwd)"

PHASES="${PHASES:-A B C D E}"
RUN_DIR="${RUN_DIR:-runs/i5}"
RESULT_TGZ="$HOME/i5-results.tgz"

# Caminho absoluto do RUN_DIR (aceita RUN_DIR relativo ao repo OU absoluto)
# para logs/banner não duplicarem o prefixo do repo.
case "$RUN_DIR" in
  /*) RUN_DIR_ABS="$RUN_DIR" ;;
  *)  RUN_DIR_ABS="$ROOT/$RUN_DIR" ;;
esac

# --------------------------------------------------------------------------
# Modo interno: relançado em background — roda só o sweep + empacota.
# --------------------------------------------------------------------------
if [[ "${I5_SWEEP_ONLY:-0}" == "1" ]]; then
  echo "[$(date '+%F %T')] sweep iniciado (PHASES=$PHASES RUN_DIR=$RUN_DIR)"
  PHASES="$PHASES" RUN_DIR="$RUN_DIR" ./scripts/run_i5_sweep.sh
  rc=$?
  echo "[$(date '+%F %T')] sweep terminou (rc=$rc); empacotando"
  if tar czf "$RESULT_TGZ" -C "$ROOT" "$RUN_DIR" 2>/dev/null; then
    echo "[ok] $RESULT_TGZ"
  else
    echo "[aviso] falha ao empacotar — os .log seguem em $RUN_DIR/"
  fi
  echo "[$(date '+%F %T')] FIM. Resultados em $ROOT/$RUN_DIR/ e $RESULT_TGZ"
  exit "$rc"
fi

ok=1   # vira 0 se um passo FATAL falhar

# --------------------------------------------------------------------------
# [1/5] Pré-flight de dados (FATAL se faltar arquivo base).
# --------------------------------------------------------------------------
echo "===== [1/5] pré-flight de dados ====="
# snort_100 / snort_1k são derivados pelo próprio sweep (head do snort.txt);
# então só exigimos os arquivos base não-deriváveis aqui.
need=(data/patterns_snort.txt data/patterns_et_32.txt
      data/enron_corpus.txt data/enron_x8.txt data/simplewiki.txt)
missing=0
for f in "${need[@]}"; do
  if [[ -s "$f" ]]; then
    printf "  ok   %-28s %s\n" "$f" "$(du -h "$f" 2>/dev/null | cut -f1)"
  else
    printf "  MISS %s\n" "$f"; missing=1
  fi
done
if [[ "$missing" == "1" ]]; then
  echo >&2
  echo "ABORTADO: dados ausentes. Recrie via scripts/prepare_datasets.sh /" >&2
  echo "          scripts/acquire_corpus.sh. Nada foi executado." >&2
  exit 1
fi

# --------------------------------------------------------------------------
# [2/5] Máquina quieta (best-effort; só AVISA — você está em TTY texto).
# --------------------------------------------------------------------------
echo
echo "===== [2/5] máquina quieta ====="
noisy=$(pgrep -c -i -E 'chrome|chromium|firefox|brave|code|electron|slack' 2>/dev/null || echo 0)
if [[ "$noisy" -gt 0 ]]; then
  echo "[AVISO] $noisy processo(s) de GUI/IDE ainda vivos — competem por core/cache/DRAM"
  echo "        em T=12 e inflam a variância. Em TTY texto, idealmente: 0."
  pgrep -a -i -E 'chrome|chromium|firefox|brave|code|electron|slack' 2>/dev/null \
    | awk '{printf "          %s %s\n", $1, $2}' | head -8
else
  echo "[ok] nenhum navegador/IDE detectado"
fi
# Indexador do GNOME (tracker) é ladrão clássico de CPU/IO; suspende se ativo.
if pgrep -x tracker-miner-fs-3 >/dev/null 2>&1 || pgrep -x tracker-extract-3 >/dev/null 2>&1; then
  echo "[info] tracker (indexador) ativo — suspendendo durante a corrida"
  tracker3 daemon -t >/dev/null 2>&1 || true
fi
echo "loadavg: $(cut -d' ' -f1-3 /proc/loadavg)"

# --------------------------------------------------------------------------
# [3/5] Governador de frequência (best-effort; sudo opcional).
# --------------------------------------------------------------------------
echo
echo "===== [3/5] governador de frequência (performance) ====="
if command -v cpupower >/dev/null 2>&1; then
  echo "  (pode pedir a senha do sudo; sem sudo, o passo é pulado e o sweep segue)"
  if sudo cpupower frequency-set -g performance; then
    echo "[ok] governador = performance"
  else
    echo "[aviso] sem sudo/cpupower — seguindo no governador atual (fica em env/start.txt)"
  fi
else
  echo "[aviso] cpupower ausente — seguindo no governador atual"
fi

# --------------------------------------------------------------------------
# [4/5] Build + correção (build FATAL; test não-fatal mas em destaque).
# --------------------------------------------------------------------------
echo
echo "===== [4/5] compilar + validar correção ====="
if ! { make clean && make; }; then
  echo >&2
  echo "ABORTADO: build falhou. Nada foi executado." >&2
  exit 1
fi
if make test; then
  echo "[ok] make test passou"
else
  echo "[AVISO] make test FALHOU — o sweep roda mesmo assim;"
  echo "        valide a correção ANTES de usar os números."
  ok=0
fi

# --------------------------------------------------------------------------
# [5/5] Ambiente (sanidade visual antes de sair).
# --------------------------------------------------------------------------
echo
echo "===== [5/5] ambiente ====="
free -h | awk 'NR<=2'
echo "nproc = $(nproc)"
echo "governor: $(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null || echo n/a)"
echo "git: $(git log -1 --oneline 2>/dev/null || echo n/a)"
[[ -n "${AC_DYN_TASKS_PER_THREAD:-}" ]] && \
  echo "AC_DYN_TASKS_PER_THREAD=$AC_DYN_TASKS_PER_THREAD (granularidade da fila dinâmica)"

# --------------------------------------------------------------------------
# Relança o sweep DESACOPLADO (sobrevive a suspensão e a logout da TTY).
# --------------------------------------------------------------------------
INHIBIT=()
if command -v systemd-inhibit >/dev/null 2>&1; then
  INHIBIT=(systemd-inhibit --what=idle:sleep:handle-lid-switch --why="AC i5 sweep")
fi
mkdir -p "$RUN_DIR_ABS"
LOG="$RUN_DIR_ABS/run_all.out"

I5_SWEEP_ONLY=1 PHASES="$PHASES" RUN_DIR="$RUN_DIR" \
  setsid "${INHIBIT[@]}" "$SELF" \
  > "$LOG" 2>&1 < /dev/null &
PID=$!

echo
echo "============================================================"
if [[ "$ok" == "1" ]]; then
  echo " Tudo OK. Sweep rodando em BACKGROUND (PID $PID)."
else
  echo " Sweep rodando em BACKGROUND (PID $PID) — REVISE o aviso do make test."
fi
echo " PODE FAZER LOGOUT / SAIR DA TTY — o sweep segue sozinho."
echo
echo "   acompanhar geral:   tail -f $LOG"
echo "   progresso/runs:     tail -f $RUN_DIR_ABS/MASTER.log"
echo "   contar concluídos:  find $RUN_DIR_ABS -name '*.log' | wc -l"
echo "   ver falhas:         find $RUN_DIR_ABS -name '*.FAIL'"
echo "   está vivo?          ps -p $PID -o pid,etime,cmd"
echo "   ao terminar:        $RUN_DIR_ABS/  +  $RESULT_TGZ"
echo "============================================================"
