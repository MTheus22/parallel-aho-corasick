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
#
# UPLOAD AUTOMÁTICO dos resultados ao fim (para não voltar à faculdade).
# Os resultados são minúsculos (logs + sweep.db, poucos MB). Escolha um canal
# que você CONTROLA e cujo endereço você conhece:
#
#   # (a) git push de runs/workstation (você mencionou "commitar o sweep").
#   #     git PURO + PAT (não depende de `gh`; o token vai por AMBIENTE, não
#   #     fica gravado em disco nem em ps). Gere um PAT fine-grained com escopo
#   #     "Contents: write" SÓ neste repo, e revogue depois da corrida:
#   WS_GIT_PUSH=1 WS_GH_PAT=github_pat_xxx ./scripts/workstation_all.sh
#   #     (sem WS_GH_PAT, faz `git push` simples — exige auth já configurada
#   #      no host: remote HTTPS com PAT embutido, chave SSH, ou credential helper.)
#
#   # (b) sua VPS (scp):
#   WS_UPLOAD_CMD='scp "$WS_RESULTS" usuario@suavps:~/' ./scripts/workstation_all.sh
#
#   # (c) zero-setup: host efêmero + notificação no seu celular (ntfy.sh):
#   WS_UPLOAD_CMD='U=$(curl -sF "file=@$WS_RESULTS" https://0x0.st); \
#     curl -d "TCC sweep pronto: $U" ntfy.sh/SEU-TOPICO-SECRETO' ./scripts/workstation_all.sh
#
# WS_GIT_PUSH e WS_UPLOAD_CMD são independentes e best-effort (não derrubam o
# run). Em WS_UPLOAD_CMD, $WS_RESULTS = caminho do .tgz. Pode combinar os dois.
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

  # --- upload best-effort dos resultados (não-fatal) ----------------------
  if [[ "${WS_GIT_PUSH:-0}" == "1" ]]; then
    echo "[$(date '+%F %T')] upload: git commit + push de runs/workstation"
    git add runs/workstation 2>/dev/null
    if git -c user.name="${WS_GIT_NAME:-workstation}" \
           -c user.email="${WS_GIT_EMAIL:-tcc@workstation}" \
           commit -q -m "resultados: sweep workstation $(date +%F)" 2>/dev/null; then
      if [[ -n "${WS_GH_PAT:-}" ]]; then
        # PAT via AMBIENTE: não grava o token em disco (.git/config) nem em argv/ps.
        # Deriva a URL HTTPS a partir do origin (aceita origin SSH ou HTTPS).
        purl="$(git remote get-url origin 2>/dev/null)"
        purl="${purl/git@github.com:/https://github.com/}"
        purl="${purl%.git}.git"
        if git -c credential.helper= \
               -c credential.helper='!f(){ echo username=x-access-token; echo "password=$WS_GH_PAT"; };f' \
               push "$purl" HEAD:main 2>&1 | tail -3; then
          echo "[upload] git push OK (PAT)"
        else
          echo "[upload] git push FALHOU (PAT/rede?) — use o .tgz ou WS_UPLOAD_CMD"
        fi
      elif git push 2>&1 | tail -3; then
        echo "[upload] git push OK"
      else
        echo "[upload] git push FALHOU — defina WS_GH_PAT, ou use o .tgz / WS_UPLOAD_CMD"
      fi
    else
      echo "[upload] git commit: nada novo a enviar (ou falhou) — confira o .tgz"
    fi
  fi
  if [[ -n "${WS_UPLOAD_CMD:-}" ]]; then
    echo "[$(date '+%F %T')] upload: WS_UPLOAD_CMD"
    if WS_RESULTS="$RESULT_TGZ" bash -c "$WS_UPLOAD_CMD"; then
      echo "[upload] WS_UPLOAD_CMD OK"
    else
      echo "[upload] WS_UPLOAD_CMD FALHOU"
    fi
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
