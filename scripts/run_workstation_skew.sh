#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "${BASH_SOURCE[0]}")/.."

TOPIC="${AC_NOTIFY_TOPIC:-workstation-sweep-750a6217d8}"
RUN_DIR="${RUN_DIR:-runs/workstation_skew_$(date +%F_%H%M)}"
PHASES="${PHASES:-H}"
AUTO_DOWNLOAD="${AUTO_DOWNLOAD:-1}"
AC_GIT_PUSH="${AC_GIT_PUSH:-${AC_PUSH:-1}}"

if [[ -z "${AC_NOTIFY:-}" ]]; then
  AC_NOTIFY='curl -fsS -d "workstation skew H terminou (rc=$AC_RC): $AC_RESULTS" https://ntfy.sh/'"$TOPIC"
fi

log() {
  printf '[workstation-skew] %s\n' "$*"
}

die() {
  printf '[workstation-skew] ERRO: %s\n' "$*" >&2
  exit 1
}

https_origin_url() {
  local purl
  purl="$(git remote get-url origin 2>/dev/null || true)"
  [[ -n "$purl" ]] || return 1
  purl="${purl/git@github.com:/https://github.com/}"
  purl="${purl%.git}.git"
  printf '%s\n' "$purl"
}

git_with_pat() {
  git -c credential.helper= \
      -c credential.helper='!f(){ echo username=x-access-token; echo "password=$AC_GH_PAT"; };f' \
      "$@"
}

check_prereqs() {
  local missing=0
  local cmd
  for cmd in git make python3 curl tar; do
    if ! command -v "$cmd" >/dev/null 2>&1; then
      printf '[workstation-skew] ERRO: comando ausente: %s\n' "$cmd" >&2
      missing=1
    fi
  done
  if (( missing )); then
    die "instale os prerequisitos no Ubuntu com: sudo apt update && sudo apt install -y build-essential git python3 curl"
  fi
}

check_git_tree() {
  git rev-parse --is-inside-work-tree >/dev/null 2>&1 ||
    die "este diretorio nao parece ser um checkout git"

  git remote get-url origin >/dev/null 2>&1 ||
    die "remote git 'origin' nao esta configurado"

  git diff --quiet ||
    die "existem mudancas rastreadas nao commitadas; limpe ou commite antes"

  git diff --cached --quiet ||
    die "existem mudancas staged; limpe ou commite antes"
}

check_git_network() {
  log "validando acesso de leitura ao origin"
  if [[ -n "${AC_GH_PAT:-}" ]]; then
    local purl
    purl="$(https_origin_url)" || die "nao foi possivel derivar URL HTTPS do origin"
    git_with_pat fetch --dry-run "$purl" main >/dev/null ||
      die "git fetch --dry-run falhou usando AC_GH_PAT"
  else
    git fetch --dry-run origin >/dev/null ||
      die "git fetch origin falhou; verifique rede/autenticacao"
  fi

  if [[ "$AC_GIT_PUSH" == "1" ]]; then
    log "validando permissao de push com --dry-run"
    if [[ -n "${AC_GH_PAT:-}" ]]; then
      purl="$(https_origin_url)" || die "nao foi possivel derivar URL HTTPS do origin"
      git_with_pat push --dry-run "$purl" HEAD:main >/dev/null ||
        die "git push --dry-run falhou usando AC_GH_PAT"
    else
      git push --dry-run origin HEAD:main >/dev/null ||
        die "git push --dry-run origin HEAD:main falhou"
    fi
  fi
}

sync_code() {
  if [[ -n "${AC_GH_PAT:-}" ]]; then
    local purl
    purl="$(https_origin_url)" || die "nao foi possivel derivar URL HTTPS do origin"
    git_with_pat fetch "$purl" main
    git merge --ff-only FETCH_HEAD
  else
    git pull --ff-only
  fi
}

check_prereqs

if [[ "${1:-}" == "--check" || "${1:-}" == "check" ]]; then
  check_git_tree
  check_git_network
  log "git OK: origin acessivel e push validado"
  exit 0
fi

log "sincronizando codigo"
check_git_tree
sync_code
check_git_tree
check_git_network

log "preparando datasets base"
AUTO_DOWNLOAD="$AUTO_DOWNLOAD" ./scripts/prepare_data.sh

log "compilando binario para gerar/verificar skew"
make

log "gerando/verificando corpora skew"
./scripts/make_skewed_corpus.sh

log "disparando run_all em background: RUN_DIR=$RUN_DIR PHASES=$PHASES"
export RUN_DIR PHASES AUTO_DOWNLOAD AC_GIT_PUSH AC_NOTIFY
export AC_PUSH="$AC_GIT_PUSH"
./scripts/run_all.sh
