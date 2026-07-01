# Task 02 — Fase H (skew) isolada no `run_sweep.sh`, com réplicas

## Objetivo

Adicionar uma **fase H** ao motor `scripts/run_sweep.sh` que roda o confronto
estático × dinâmico (× plain × flat) nos corpora uniforme e skewed em `T=MAX_T`,
com **N réplicas independentes** e `--per-thread`, gerando logs em `H_skew/`.
Deve ser **isolável** (`PHASES="H"`) para validar o achado **sem re-rodar A–G**.

## Escopo

- **In scope:** `scripts/run_sweep.sh` — nova função `phase_H` + entrada no
  dispatcher; reuso do helper `run_aclab` (linha ~252) e de `MAX_T`/`RUN_DIR`.
- **Out of scope:** o gerador (Task 01, pré-requisito); análise (Task 03);
  qualquer mudança em searchers; incluir a fase H no default A–G (fica **opt-in**
  para não inflar as corridas canônicas).

## Implementação

1. **Searchers do confronto** (2×2 estático/dinâmico × plain/flat):
   `pthread_chunked_v2` (estático, sem flat), `pthread_chunked_flat` (estático,
   flat), `pthread_dynamic` (dinâmico, sem flat), `pthread_dynamic_flat`
   (dinâmico, flat).
2. **Corpora:** `data/enron_skew_uniform.txt` e `data/enron_skew_clustered.txt`
   (Task 01). Padrões: `patterns_snort.txt` (regime ~L3) **e**
   `patterns_et_32.txt` (regime memory-bound severo) — a hipótese é que o skew
   pesa mais quando o custo por byte já é alto.
3. **Réplicas:** `REPS=${AC_SKEW_REPS:-5}`. Para cada `(searcher, corpus,
   patterns)` invoque `run_aclab` **REPS vezes**, cada uma um **processo
   independente** (não `--iters` dentro de um processo), com tag `repNN` e
   `--per-thread`. Use `warmup=1 iters=3` por invocação (o sinal de réplica vem
   das REPS invocações, não das iters).
4. **Função** (modele em `phase_D`/`phase_G`, linhas ~450 e ~518):
   ```bash
   phase_H() {
     log "===== Fase H — corpus skew (T=$MAX_T, REPS=${AC_SKEW_REPS:-5}) ====="
     local phase="H_skew"
     local reps="${AC_SKEW_REPS:-5}"
     local searchers=(pthread_chunked_v2 pthread_chunked_flat
                      pthread_dynamic pthread_dynamic_flat)
     local corpora=(enron_skew_uniform enron_skew_clustered)
     local pats=(patterns_snort patterns_et_32)
     for pat in "${pats[@]}"; do
       for cor in "${corpora[@]}"; do
         for s in "${searchers[@]}"; do
           for r in $(seq 1 "$reps"); do
             run_aclab "$phase" "$DATA/$pat.txt" "$DATA/$cor.txt" \
                       "$s" "$MAX_T" 1 3 "rep$(printf '%02d' "$r")_perthread" --per-thread
           done
         done
       done
     done
   }
   ```
   (Ajuste `$DATA`/extensões ao padrão real do script — confira como `phase_D`
   monta os caminhos.)
5. **Dispatcher:** adicione `H) phase_H ;;` no `case "$p" in …` (linha ~555).
   **Não** inclua `H` no default `PHASES="${PHASES:-${1:-A B C D E G}}"`.
6. **Cabeçalho de doc:** atualize o bloco de comentário do topo do script
   (lista de fases) descrevendo a fase H e o `AC_SKEW_REPS`.

## Validação

```bash
make            # binário atual basta (dynamic_flat já registrado)
./scripts/make_skewed_corpus.sh                     # Task 01 (pré-requisito)
# corrida isolada da fase H (NÃO re-roda A–G):
RUN_DIR=runs/workstation_skew MAX_T=$(nproc) PHASES="H" AC_SKEW_REPS=5 ./scripts/run_sweep.sh
ls runs/workstation_skew/H_skew/ | head       # logs rep01..rep05 por config
grep -c '^\[t' runs/workstation_skew/H_skew/*rep01_perthread.log | head  # linhas per-thread presentes
```

## Critérios de Aceite

- `PHASES="H" scripts/run_sweep.sh` roda **apenas** a fase H (nenhum log de
  A–G é criado/tocado).
- `H_skew/` contém, por `(patterns, corpus, searcher)`, **REPS** logs distintos
  (`rep01`…`rep05`), cada um com a linha de resultado do searcher **e** as linhas
  `[tNN] … ms … MB/s` per-thread.
- Cobre os 4 searchers × 2 corpora × 2 conjuntos de padrões.
- `H` **não** está no conjunto default de fases (é opt-in).
- O resume do `run_aclab` funciona (reexecutar pula logs completos).
