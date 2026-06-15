#!/usr/bin/env bash
# =============================================================================
# Pré-flight de dados para o sweep da workstation (run_workstation_sweep.sh).
# =============================================================================
#
# Por que este script existe:
#   O sweep da workstation precisa de 4 arquivos de dados que NÃO são gerados
#   por prepare_datasets.sh e NÃO estão versionados (data/ é gitignored):
#
#     data/enron_x8.txt        (10,59 GiB) — fases A, D, S
#     data/enron_corpus.txt    ( 1,42 GiB) — fases B, E
#     data/patterns_snort.txt              — fases A, B, D, E
#     data/patterns_et_32.txt  (~816 KiB)  — fases A, B, E
#
#   Numa máquina alugada/fresca, rodar o sweep sem estes arquivos faz TODA a
#   fase A/D/E falhar (.FAIL). Como o sweep roda UMA vez, isto é fatal.
#
#   Este script torna o provisionamento idempotente e à prova de falhas:
#     - gera enron_x8.txt = enron_corpus.txt concatenado 8× (com checagem de
#       disco e de tamanho exato);
#     - gera patterns_snort_100/1k.txt via head (igual ao que o sweep faria);
#     - se patterns_snort.txt / enron_corpus.txt faltarem, oferece rodar o
#       prepare_datasets.sh (download Snort + Enron);
#     - patterns_et_32.txt NÃO tem como ser regenerado (sem fonte/script):
#       se faltar, o script PARA com instruções claras de scp.
#
#   Ao fim, imprime um relatório "PRONTO/NÃO PRONTO" com tamanhos. Rode-o
#   ANTES do sweep e só dispare o sweep se o veredito for PRONTO.
#
# Uso:
#   scripts/prepare_workstation_data.sh            # valida + gera o que der
#   AUTO_DOWNLOAD=1 scripts/prepare_workstation_data.sh   # baixa Snort/Enron se faltarem
# =============================================================================

set -uo pipefail

cd "$(dirname "$0")/.."
DATA=data
mkdir -p "$DATA"

RED=$'\033[31m'; GRN=$'\033[32m'; YEL=$'\033[33m'; RST=$'\033[0m'
say()  { printf '%s\n' "$*"; }
ok()   { printf '%s[ ok ]%s %s\n'   "$GRN" "$RST" "$*"; }
warn() { printf '%s[warn]%s %s\n'   "$YEL" "$RST" "$*"; }
err()  { printf '%s[FAIL]%s %s\n'   "$RED" "$RST" "$*" >&2; }

human() { numfmt --to=iec --suffix=B "$1" 2>/dev/null || echo "$1 B"; }
size_of() { stat -c %s "$1" 2>/dev/null || echo 0; }

PROBLEMS=0

# --------------------------------------------------------------------------
# 1. Base: patterns_snort.txt + enron_corpus.txt (geráveis via download)
# --------------------------------------------------------------------------
need_base_download=0
[[ -s "$DATA/patterns_snort.txt" ]] || need_base_download=1
[[ -s "$DATA/enron_corpus.txt"  ]] || need_base_download=1

if (( need_base_download )); then
  warn "patterns_snort.txt e/ou enron_corpus.txt ausentes."
  if [[ "${AUTO_DOWNLOAD:-0}" == "1" ]]; then
    say "  -> AUTO_DOWNLOAD=1: rodando scripts/prepare_datasets.sh (Snort + Enron, ~1,7 GB)"
    if ! ./scripts/prepare_datasets.sh; then
      err "prepare_datasets.sh falhou."
      PROBLEMS=$((PROBLEMS+1))
    fi
  else
    err "Faça uma das opções e rode este script de novo:"
    say "     (a) copie os arquivos do laptop:  scp laptop:.../data/{patterns_snort.txt,enron_corpus.txt} data/"
    say "     (b) gere via download:            AUTO_DOWNLOAD=1 scripts/prepare_workstation_data.sh"
    PROBLEMS=$((PROBLEMS+1))
  fi
fi

[[ -s "$DATA/patterns_snort.txt" ]] && ok "patterns_snort.txt   ($(human "$(size_of "$DATA/patterns_snort.txt")"))"
[[ -s "$DATA/enron_corpus.txt"  ]] && ok "enron_corpus.txt     ($(human "$(size_of "$DATA/enron_corpus.txt")"))"

# --------------------------------------------------------------------------
# 2. patterns_et_32.txt — SEM fonte/script. Só pode vir por cópia.
# --------------------------------------------------------------------------
if [[ -s "$DATA/patterns_et_32.txt" ]]; then
  ok "patterns_et_32.txt   ($(human "$(size_of "$DATA/patterns_et_32.txt")"))"
else
  err "patterns_et_32.txt AUSENTE e NÃO é regenerável (sem script/fonte no repo)."
  say "     Copie do laptop:  scp laptop:.../data/patterns_et_32.txt data/"
  say "     (É o dicionário ET-32, ~816 KiB / ~44708 padrões — segundo regime de cache da fase A.)"
  PROBLEMS=$((PROBLEMS+1))
fi

# --------------------------------------------------------------------------
# 3. Dicionários reduzidos (head) — geráveis localmente.
# --------------------------------------------------------------------------
if [[ -s "$DATA/patterns_snort.txt" ]]; then
  [[ -s "$DATA/patterns_snort_100.txt" ]] || head -100  "$DATA/patterns_snort.txt" > "$DATA/patterns_snort_100.txt"
  [[ -s "$DATA/patterns_snort_1k.txt"  ]] || head -1000 "$DATA/patterns_snort.txt" > "$DATA/patterns_snort_1k.txt"
  ok "patterns_snort_100.txt / patterns_snort_1k.txt"
fi

# --------------------------------------------------------------------------
# 4. enron_x8.txt = enron_corpus.txt × 8 (com checagem de disco e tamanho).
# --------------------------------------------------------------------------
if [[ -s "$DATA/enron_corpus.txt" ]]; then
  base=$(size_of "$DATA/enron_corpus.txt")
  want=$(( base * 8 ))
  have=$(size_of "$DATA/enron_x8.txt")
  if [[ "$have" == "$want" ]]; then
    ok "enron_x8.txt         ($(human "$have")) — já é 8× enron_corpus"
  else
    if [[ -e "$DATA/enron_x8.txt" ]]; then
      warn "enron_x8.txt existe mas tem tamanho inesperado ($(human "$have") != $(human "$want")) — regerando."
      rm -f "$DATA/enron_x8.txt"
    fi
    # Disco: precisa de ~want bytes livres no FS de data/.
    avail=$(df -P -B1 "$DATA" | awk 'NR==2 {print $4}')
    if [[ -n "$avail" ]] && (( avail < want + (1<<30) )); then
      err "Disco insuficiente p/ enron_x8: livre=$(human "$avail"), preciso ≈$(human "$want") (+1 GiB folga)."
      say "     Libere espaço ou copie enron_x8.txt pronto do laptop."
      PROBLEMS=$((PROBLEMS+1))
    else
      say "  -> gerando enron_x8.txt (8× $(human "$base") = $(human "$want"))..."
      : > "$DATA/enron_x8.txt"
      gen_ok=1
      for i in $(seq 8); do
        if ! cat "$DATA/enron_corpus.txt" >> "$DATA/enron_x8.txt"; then
          err "falha ao concatenar (cópia $i/8) — disco cheio?"; gen_ok=0; break
        fi
      done
      got=$(size_of "$DATA/enron_x8.txt")
      if (( gen_ok )) && [[ "$got" == "$want" ]]; then
        ok "enron_x8.txt gerado ($(human "$got"))"
      else
        err "enron_x8.txt incompleto ($(human "$got") != $(human "$want")) — removendo."
        rm -f "$DATA/enron_x8.txt"
        PROBLEMS=$((PROBLEMS+1))
      fi
    fi
  fi
fi

# --------------------------------------------------------------------------
# 5. Veredito
# --------------------------------------------------------------------------
say
say "================ relatório de dados (workstation) ================"
req=(patterns_snort.txt patterns_et_32.txt patterns_snort_100.txt
     patterns_snort_1k.txt enron_corpus.txt enron_x8.txt)
allok=1
for f in "${req[@]}"; do
  if [[ -s "$DATA/$f" ]]; then
    printf '  %s%-26s%s %s\n' "$GRN" "$f" "$RST" "$(human "$(size_of "$DATA/$f")")"
  else
    printf '  %s%-26s%s AUSENTE%s\n' "$RED" "$f" "$RST" "$RST"
    allok=0
  fi
done
say "================================================================="

if (( allok )); then
  ok "PRONTO — todos os datasets presentes. Pode disparar o sweep:"
  say "      sudo cpupower frequency-set -g performance"
  say "      make && make test"
  say "      nohup ./scripts/run_workstation_sweep.sh > workstation.out 2>&1 &"
  exit 0
else
  err "NÃO PRONTO — resolva os itens acima ($PROBLEMS problema(s)) antes do sweep."
  exit 1
fi
