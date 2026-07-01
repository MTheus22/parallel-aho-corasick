# Task 05 — Documentar construção, protocolo e resultado (docs de apoio)

## Objetivo

Registrar o corpus skew e o achado nos documentos de apoio, **antes** do LaTeX
(convenção do workspace: `tcc_notes/sections/notes/*` primeiro). Cobre a
construção do corpus, o protocolo da fase H e a leitura do resultado da Task 03.

## Escopo

- **In scope:** `runs/<run>/RESULTS.md` (seção "Corpus de carga desigual
  (skew)"); `../tcc_notes/sections/notes/methodology.md`;
  `../tcc_notes/sections/notes/results.md`.
- **Out of scope:** LaTeX (Task 06); `referencias.bib` (Task 06).
- **Atenção:** `tcc_notes/` é vault Obsidian — **não** rodar `git` lá.

## Implementação

1. **`runs/<run>/RESULTS.md` (construção + protocolo):**
   - Como o par uniforme×clustered é gerado (`make_skewed_corpus.sh`): blocos
     frios/quentes, injeção FHBM a ~42%, reordenação, **invariante de bytes+match
     iguais**, fator de skew.
   - Protocolo da fase H: 4 searchers × 2 corpora × 2 dicts, `T=MAX_T`, **REPS≥5
     processos independentes**, `--per-thread`; comando
     `PHASES="H" scripts/run_sweep.sh` (isolado, sem re-rodar A–G).
   - Cole a tabela `skew_analysis.md` (Task 03): `mbps_med`, `spread_med [IQR]`,
     `barrier_idle_med [IQR]`.
2. **`methodology.md` (consolidação):**
   - Nova hipótese **H8**: "sob carga espacialmente desigual, o chunking estático
     desbalanceia (straggler limita o makespan — moldura de Ródenas) e o dispatch
     dinâmico recupera; sob carga uniforme, empatam". Registrar o **invariante de
     controle** (mesmos bytes+matches) como o que torna o spread atribuível a
     balanceamento.
   - Acrescentar `enron_skew_{uniform,clustered}` à tabela de corpora, com o
     papel "estressor de balanceamento (contribuição própria)".
   - Registrar que **pcap/tráfego real é fora de escopo** (throughput, não
     acurácia) — coerente com a memória `real-scans-out-of-scope`.
3. **`results.md` (headline por frente):** nova subseção com o número do skew —
   spread estático × dinâmico no clustered vs uniform e, se confirmado, o
   cenário em que `dynamic`/`dynamic_flat` batem o estático em CPU homogênea.
   Se o efeito for negativo/fraco, registrar como tal (não inflar).
4. **Proveniência:** citar o `RUN_DIR` da coleta (ex.:
  `runs/workstation_skew/`), commit e data, no padrão das outras entradas.

## Validação

- Revisão manual: os três arquivos referenciam os **mesmos números** da tabela
  `runs/<dir>/H_skew/skew_analysis.md` (sem divergência).
- `grep -n "skew" runs/<run>/RESULTS.md ../tcc_notes/sections/notes/methodology.md`
  retorna as novas seções.

## Critérios de Aceite

- `runs/<run>/RESULTS.md` tem a seção com construção + protocolo + tabela.
- `methodology.md` tem a hipótese H8, o invariante de controle, os corpora novos
  e a nota de pcap-fora-de-escopo.
- `results.md` tem a subseção do skew com número (positivo **ou** negativo)
  batendo com a Task 03.
- Nenhum comando `git` executado em `tcc_notes/`.
