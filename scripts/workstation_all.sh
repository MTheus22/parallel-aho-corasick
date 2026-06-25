#!/usr/bin/env bash
# =============================================================================
# workstation_all.sh — corrida única "um comando".
# =============================================================================
# Faz, em sequência: pré-flight de dados -> governador -> build -> make test
# -> sweep -> empacota resultados.
#
# Setup roda em FOREGROUND (você assistindo, ~poucos min — pode pedir senha do
# sudo). O sweep LONGO segue sozinho em BACKGROUND, imune a:
#   - suspensão da máquina  (systemd-inhibit)
#   - logout / queda de SSH (setsid + </dev/null + redirecionamento)
#
# NÃO precisa instalar nada: nohup/setsid/systemd-inhibit já vêm em qualquer
# Ubuntu (coreutils/util-linux/systemd). Sudo é OPCIONAL (só p/ o governador).
#
# Uso (na workstation, com o repo + data/ JÁ no disco LOCAL, nunca no pendrive):
#   ./scripts/workstation_all.sh                  # roda A B D E S
#   PHASES="A B D E" ./scripts/workstation_all.sh # sem a fase opcional S
# =============================================================================
set -uo pipefail

# Caminho absoluto deste script ANTES de qualquer cd (necessário p/ o re-exec).
SELF="$(cd "$(dirname "$0")" && pwd)/$(basename "$0")"
cd "$(dirname "$SELF")/.."          # raiz do repo
ROOT="$(pwd)"

PHASES="${PHASES:-A B D E S}"
RESULT_TGZ="$HOME/ws-results.tgz"

# --------------------------------------------------------------------------
# Modo interno: relançado em background — roda só o sweep + empacota.
# --------------------------------------------------------------------------
if [[ "${WS_SWEEP_ONLY:-0}" == "1" ]]; then
  echo "[$(date '+%F %T')] sweep iniciado (PHASES=$PHASES)"
  PHASES="$PHASES" ./scripts/run_workstation_sweep.sh
  rc=$?
  echo "[$(date '+%F %T')] sweep terminou (rc=$rc); empacotando"
  if tar czf "$RESULT_TGZ" -C "$ROOT" runs/workstation 2>/dev/null; then
    echo "[ok] $RESULT_TGZ"
  else
    echo "[aviso] falha ao empacotar — os .log seguem em runs/workstation/"
  fi
  echo "[$(date '+%F %T')] FIM. Resultados em $ROOT/runs/workstation/ e $RESULT_TGZ"
  exit "$rc"
fi

# --------------------------------------------------------------------------
# [1/4] Pré-flight de dados (FATAL se NÃO PRONTO).
# --------------------------------------------------------------------------
echo "===== [1/4] pré-flight de dados ====="
if ! ./scripts/prepare_workstation_data.sh; then
  echo >&2
  echo "ABORTADO: dados NÃO PRONTOS (veja acima). Nada foi executado." >&2
  exit 1
fi

# --------------------------------------------------------------------------
# [2/4] Governador de frequência (best-effort; sudo opcional).
# --------------------------------------------------------------------------
echo
echo "===== [2/4] governador de frequência (performance) ====="
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
# [3/4] Build no host (Zen 5) + correção (build FATAL; test não-fatal).
# --------------------------------------------------------------------------
echo
echo "===== [3/4] compilar no host + validar correção ====="
if ! { make clean && make; }; then
  echo >&2
  echo "ABORTADO: build falhou. Nada foi executado." >&2
  exit 1
fi
if make test; then
  echo "[ok] make test passou"
else
  echo "[AVISO] make test FALHOU — o sweep roda mesmo assim;"
  echo "        valide v_correctness=1 amanhã ANTES de usar os números."
fi

# --------------------------------------------------------------------------
# [4/4] Ambiente (sanidade visual antes de sair).
# --------------------------------------------------------------------------
echo
echo "===== [4/4] ambiente ====="
free -h | awk 'NR<=2'
echo "nproc = $(nproc)"

# --------------------------------------------------------------------------
# Relança o sweep DESACOPLADO (sobrevive a suspensão e a logout/queda de SSH).
# --------------------------------------------------------------------------
INHIBIT=()
if command -v systemd-inhibit >/dev/null 2>&1; then
  INHIBIT=(systemd-inhibit --what=idle:sleep:handle-lid-switch --why="AC workstation sweep")
fi
mkdir -p runs/workstation
LOG="$ROOT/runs/workstation/run_all.out"

WS_SWEEP_ONLY=1 PHASES="$PHASES" setsid "${INHIBIT[@]}" "$SELF" \
  > "$LOG" 2>&1 < /dev/null &
PID=$!

echo
echo "============================================================"
echo " Sweep rodando em BACKGROUND (PID $PID). PODE SAIR / DESCONECTAR."
echo "   acompanhar geral:  tail -f $LOG"
echo "   progresso/runs:    tail -f $ROOT/runs/workstation/MASTER.log"
echo "   ao terminar:       runs/workstation/  +  $RESULT_TGZ"
echo "============================================================"
