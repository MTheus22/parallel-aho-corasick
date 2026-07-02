#!/usr/bin/env bash
# =============================================================================
# run_all.sh — corrida única "um comando", env-agnóstica, com pull + upload +
#              notificação. Funde i5_all.sh e workstation_all.sh.
# =============================================================================
# Faz, em sequência (FOREGROUND, você assistindo — ~poucos min, pode pedir a
# senha do sudo):
#   [0/5] pull opcional do código (AC_GIT_PULL=1)
#   [1/5] pré-flight de dados        (scripts/prepare_data.sh — FATAL se faltar)
#   [2/5] governador de frequência   (cpupower performance; agnóstico, sudo opc.)
#   [3/5] build + correção           (make clean && make FATAL; make test FATAL)
#   [4/5] sanidade de ambiente       (lscpu/free/nproc)
# Depois RELANÇA o sweep LONGO em BACKGROUND, imune a:
#   - suspensão / idle           (systemd-inhibit)
#   - logout / queda de SSH/TTY  (setsid + </dev/null + redirecionamento)
# Ao terminar o sweep, empacota runs/<RUN_DIR> e faz upload/notificação
# best-effort (nunca derruba a corrida).
#
# NÃO precisa instalar nada: setsid/systemd-inhibit/flock já vêm no Ubuntu
# (coreutils/util-linux/systemd). Sudo é OPCIONAL (só para o governador).
#
# ---------------------------------------------------------------------------
# Variáveis (HERDADAS pelo processo desacoplado — defina-as na linha de chamada):
#   RUN_DIR        diretório de saída (default: auto pelo modelo de CPU, igual
#                  ao run_sweep.sh). Ex.: RUN_DIR=runs/workstation.
#   PHASES         fases do sweep (default: "A B C D E G"; passe a run_sweep.sh).
#   MAX_THREADS    teto de threads (default: nproc).
#   THREAD_POINTS  pontos de curva custom (ver run_sweep.sh).
#   AC_GIT_PULL=1  git pull --ff-only antes de medir (best-effort, [0/5]).
#   AC_GIT_PUSH=1  commit + push de runs/<RUN_DIR> ao fim (best-effort).
#   AC_GH_PAT=...  PAT (por AMBIENTE, nunca em disco/argv) p/ o push HTTPS.
#   AC_UPLOAD_CMD  comando de upload; $AC_RESULTS = caminho do .tgz.
#   AC_NOTIFY      comando de notificação independente; roda ao fim com rc/.tgz.
#
# ---------------------------------------------------------------------------
# Uso (workstation Ryzen 9 9950X — grade completa + upload por git + ntfy):
#   RUN_DIR=runs/workstation \
#   AC_GIT_PULL=1 AC_GIT_PUSH=1 AC_GH_PAT=github_pat_xxx \
#   AC_NOTIFY='curl -d "TCC sweep pronto (rc=$AC_RC): $AC_RESULTS" ntfy.sh/SEU-TOPICO' \
#     ./scripts/run_all.sh
#
# Uso (workstation — host efêmero + notificação no celular):
#   RUN_DIR=runs/workstation \
#   AC_UPLOAD_CMD='U=$(curl -sF "file=@$AC_RESULTS" https://0x0.st); \
#     curl -d "TCC sweep: $U" ntfy.sh/SEU-TOPICO' \
#     ./scripts/run_all.sh
#
# Uso (i5-1235U — protocolo máquina-quieta, TTY texto pós-reboot, sem upload):
#   RUN_DIR=runs/i5 PHASES="A B C D E G" ./scripts/run_all.sh
#
# AC_GIT_PUSH, AC_UPLOAD_CMD e AC_NOTIFY são independentes e best-effort: falha
# em qualquer um loga e NÃO altera o rc do sweep.
# =============================================================================
set -uo pipefail

# Caminho absoluto deste script ANTES de qualquer cd (necessário p/ o re-exec).
SELF="$(cd "$(dirname "$0")" && pwd)/$(basename "$0")"
cd "$(dirname "$SELF")/.."          # raiz do repo
ROOT="$(pwd)"

PHASES="${PHASES:-A B C D E G}"

# -------- RUN_DIR: derivado uma vez aqui (mesma lógica do run_sweep.sh) ------
# Fixamos RUN_DIR já no setup e o passamos EXPLÍCITO ao run_sweep.sh, para que
# empacotamento/banner/upload apontem para o mesmo diretório que o sweep usa.
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
if [[ -z "${RUN_DIR:-}" ]]; then
  SLUG=$(cpu_slug); [[ -z "$SLUG" ]] && SLUG=$(hostname -s 2>/dev/null || echo host)
  RUN_DIR="runs/$SLUG"
fi

# Caminho absoluto do RUN_DIR (aceita relativo ao repo OU absoluto).
case "$RUN_DIR" in
  /*) RUN_DIR_ABS="$RUN_DIR" ;;
  *)  RUN_DIR_ABS="$ROOT/$RUN_DIR" ;;
esac
SLUG="$(basename "$RUN_DIR")"
RESULT_TGZ="$HOME/${SLUG}-results.tgz"

# ===========================================================================
# Modo interno: relançado em background — roda só o sweep + empacota + upload.
# ===========================================================================
if [[ "${AC_SWEEP_ONLY:-0}" == "1" ]]; then
  echo "[$(date '+%F %T')] sweep iniciado (PHASES=$PHASES RUN_DIR=$RUN_DIR)"
  PHASES="$PHASES" RUN_DIR="$RUN_DIR" ./scripts/run_sweep.sh
  rc=$?
  echo "[$(date '+%F %T')] sweep terminou (rc=$rc); pós-processando"

  # --- pós-processamento: logs → sweep.csv → sweep.db (best-effort) --------
  if python3 scripts/extract_sweep_csv.py "$RUN_DIR" -o "$RUN_DIR/sweep.csv" \
     && python3 scripts/build_sweep_db.py "$RUN_DIR/sweep.csv"; then
    echo "[ok] pós-processamento: $RUN_DIR/sweep.csv + sweep.db"
  else
    echo "[aviso] pós-processamento falhou — rode extract_sweep_csv.py / build_sweep_db.py manualmente"
  fi

  echo "[$(date '+%F %T')] empacotando"
  if tar czf "$RESULT_TGZ" -C "$ROOT" "$RUN_DIR" 2>/dev/null; then
    echo "[ok] $RESULT_TGZ"
  else
    echo "[aviso] falha ao empacotar — os .log seguem em $RUN_DIR/"
  fi

  # --- upload best-effort: git push de runs/<RUN_DIR> (não-fatal) ----------
  if [[ "${AC_GIT_PUSH:-0}" == "1" ]]; then
    echo "[$(date '+%F %T')] upload: git commit + push de $RUN_DIR"
    git add "$RUN_DIR" 2>/dev/null
    if git -c user.name="${AC_GIT_NAME:-matheus}" \
           -c user.email="${AC_GIT_EMAIL:-matheus.acdebarros@gmail.com}" \
           commit -q -m "resultados: sweep $SLUG $(date +%F)" 2>/dev/null; then
      if [[ -n "${AC_GH_PAT:-}" ]]; then
        # PAT via AMBIENTE: não grava o token em disco (.git/config) nem em argv/ps.
        # Deriva a URL HTTPS a partir do origin (aceita origin SSH ou HTTPS).
        purl="$(git remote get-url origin 2>/dev/null)"
        purl="${purl/git@github.com:/https://github.com/}"
        purl="${purl%.git}.git"
        # parallel-aho-corasick trabalha na main; runs/ é commitado/empurrado lá.
        if git -c credential.helper= \
               -c credential.helper='!f(){ echo username=x-access-token; echo "password=$AC_GH_PAT"; };f' \
               push "$purl" HEAD:main 2>&1 | tail -3; then
          echo "[upload] git push OK (PAT)"
        else
          echo "[upload] git push FALHOU (PAT/rede?) — use o .tgz ou AC_UPLOAD_CMD"
        fi
      elif git push 2>&1 | tail -3; then
        echo "[upload] git push OK"
      else
        echo "[upload] git push FALHOU — defina AC_GH_PAT, ou use o .tgz / AC_UPLOAD_CMD"
      fi
    else
      echo "[upload] git commit: nada novo a enviar (ou falhou) — confira o .tgz"
    fi
  fi

  # --- upload best-effort: comando arbitrário (não-fatal) -----------------
  if [[ -n "${AC_UPLOAD_CMD:-}" ]]; then
    echo "[$(date '+%F %T')] upload: AC_UPLOAD_CMD"
    if AC_RESULTS="$RESULT_TGZ" bash -c "$AC_UPLOAD_CMD"; then
      echo "[upload] AC_UPLOAD_CMD OK"
    else
      echo "[upload] AC_UPLOAD_CMD FALHOU"
    fi
  fi

  # --- notificação independente do upload (não-fatal) ---------------------
  if [[ -n "${AC_NOTIFY:-}" ]]; then
    echo "[$(date '+%F %T')] notificação: AC_NOTIFY (rc=$rc)"
    if AC_RC="$rc" AC_RESULTS="$RESULT_TGZ" bash -c "$AC_NOTIFY"; then
      echo "[notify] AC_NOTIFY OK"
    else
      echo "[notify] AC_NOTIFY FALHOU"
    fi
  fi

  echo "[$(date '+%F %T')] FIM. Resultados em $RUN_DIR_ABS/ e $RESULT_TGZ"
  exit "$rc"
fi

# --------------------------------------------------------------------------
# [0/5] Pull opcional do código (best-effort, não-fatal).
# --------------------------------------------------------------------------
echo "===== [0/5] pull do código (opcional) ====="
if [[ "${AC_GIT_PULL:-0}" == "1" ]]; then
  if git rev-parse --is-inside-work-tree >/dev/null 2>&1; then
    if git pull --ff-only 2>&1 | tail -3; then
      echo "[ok] git pull --ff-only"
    else
      echo "[aviso] git pull falhou (rede/divergência?) — seguindo com o código atual"
    fi
  else
    echo "[aviso] não é um repo git — pulando pull"
  fi
else
  echo "[skip] AC_GIT_PULL!=1 — usando o código já no disco"
fi

# --------------------------------------------------------------------------
# [1/5] Pré-flight de dados (FATAL se NÃO PRONTO).
# --------------------------------------------------------------------------
echo
echo "===== [1/5] pré-flight de dados ====="
if ! ./scripts/prepare_data.sh; then
  echo >&2
  echo "ABORTADO: dados NÃO PRONTOS (veja acima). Nada foi executado." >&2
  exit 1
fi

# --------------------------------------------------------------------------
# [2/5] Governador de frequência (agnóstico; best-effort; sudo opcional).
# --------------------------------------------------------------------------
echo
echo "===== [2/5] governador de frequência (performance) ====="
# cpupower serve para intel_pstate E amd-pstate — nada específico de fabricante.
if command -v cpupower >/dev/null 2>&1; then
  echo "  (pode pedir a senha do sudo; sem sudo, o passo é pulado e o sweep segue)"
  if sudo cpupower frequency-set -g performance; then
    echo "[ok] governador = performance"
  else
    echo "[aviso] sem sudo/cpupower — seguindo no governador atual (fica em env/start.txt)"
  fi
else
  echo "[aviso] cpupower ausente — seguindo no governador atual (fica em env/start.txt)"
fi

# --------------------------------------------------------------------------
# [3/5] Build + correção (build e test FATAIS).
# --------------------------------------------------------------------------
echo
echo "===== [3/5] compilar + validar correção ====="
if ! { make clean && make; }; then
  echo >&2
  echo "ABORTADO: build falhou. Nada foi executado." >&2
  exit 1
fi
if make test; then
  echo "[ok] make test passou"
else
  echo >&2
  echo "ABORTADO: make test falhou. Nada foi executado." >&2
  exit 1
fi

# --------------------------------------------------------------------------
# [4/5] Sanidade de ambiente (antes de sair).
# --------------------------------------------------------------------------
echo
echo "===== [4/5] ambiente ====="
lscpu 2>/dev/null | grep -E '^(CPU\(s\)|Thread|Core|Socket|NUMA|Model name)|L3' || true
free -h | awk 'NR<=2'
echo "nproc = $(nproc)"
echo "governor: $(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null || echo n/a)"
echo "git: $(git log -1 --oneline 2>/dev/null || echo n/a)"

# --------------------------------------------------------------------------
# Relança o sweep DESACOPLADO (sobrevive a suspensão e a logout/queda de SSH).
# --------------------------------------------------------------------------
INHIBIT=()
if command -v systemd-inhibit >/dev/null 2>&1; then
  INHIBIT=(systemd-inhibit --what=idle:sleep:handle-lid-switch --why="AC sweep")
fi
mkdir -p "$RUN_DIR_ABS"
LOG="$RUN_DIR_ABS/run_all.out"

# AC_SWEEP_ONLY/PHASES/RUN_DIR são explícitos; AC_*/MAX_THREADS/THREAD_POINTS
# já estão no ambiente e são herdados pelo processo desacoplado.
AC_SWEEP_ONLY=1 PHASES="$PHASES" RUN_DIR="$RUN_DIR" \
  setsid "${INHIBIT[@]}" "$SELF" \
  > "$LOG" 2>&1 < /dev/null &
PID=$!

echo
echo "============================================================"
echo " Tudo OK. Sweep rodando em BACKGROUND (PID $PID)."
echo " PODE FAZER LOGOUT / DESCONECTAR — o sweep segue sozinho."
echo
echo "   acompanhar geral:   tail -f $LOG"
echo "   progresso/runs:     tail -f $RUN_DIR_ABS/MASTER.log"
echo "   contar concluídos:  find $RUN_DIR_ABS -name '*.log' | wc -l"
echo "   ver falhas:         find $RUN_DIR_ABS -name '*.FAIL'"
echo "   está vivo?          ps -p $PID -o pid,etime,cmd"
echo "   ao terminar:        $RUN_DIR_ABS/  +  $RESULT_TGZ"
echo "============================================================"
