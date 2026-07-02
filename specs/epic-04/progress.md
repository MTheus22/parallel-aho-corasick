# Épico 04 — Corpus de carga desigual (skew) — Progress

## Tasks
- [x] Task 01 — Gerador de corpus skew (`make_skewed_corpus.sh`) (`task-01.md`)
- [x] Task 02 — Fase H (skew) isolada no `run_sweep.sh`, com réplicas (`task-02.md`)
- [ ] Task 03 — Ingestão + análise do spread por worker (`task-03.md`)
- [x] Task 04 — Instrumentar `--per-thread` com CPU físico (`task-04.md`)
- [ ] Task 05 — Documentar construção/protocolo/resultado (docs de apoio) (`task-05.md`)
- [ ] Task 06 — Complementar o texto do TCC (LaTeX) (`task-06.md`)

## Status
**Infra completa; piloto i5 em execução; coleta canônica aguarda a workstation.**

- **Task 01 (2026-07-02):** os 4 corpora existem em `data/` (4 GiB cada:
  `enron_skew_{uniform,s0.5,clustered,s0.1}`; clustered = skew 0.25) com
  **paridade exata verificada**: 4 294 967 296 bytes e 94 304 128 matches em
  todos (re-rodar `make_skewed_corpus.py` re-verifica arquivos existentes).
  Densidade implementada: ~0,05 matches/byte no bloco quente (global 0,022) —
  interpretação do alvo DEFCON, que é fração de *pacotes* (unidade diferente).
- **Task 02 (2026-07-02):** `phase_H` no `run_sweep.sh` com `AC_SKEW_REPS`
  (default 5) e listas em `AC_SKEW_SEARCHERS/AC_SKEW_PATS/AC_SKEW_CORPORA`.
  **`pthread_chunked_v3` foi adicionado ao default** para o contraste no i5
  (corrige desbalanceamento da máquina, cego ao do conteúdo); no Ryzen
  homogêneo, omitir via `AC_SKEW_SEARCHERS` se quiser economizar.
- **Task 04 (2026-07-02):** `--per-thread` emite `cpu=` por worker
  (`sched_getcpu()` no fim do scan) e `build_sweep_db.py` ingere a coluna
  `cpu`. Limitação: não captura migrações no meio do laço nem freq efetiva.
- **Piloto i5 — PRONTO PARA DISPARAR:** comando, pré-condições e variações em
  `docs/i5-replicas-skew-command.txt` (`PHASES="R H"`: fase R = curvas
  replicadas snort×enron; fase H completa). ⚠️ **Confundido por P/E** — serve
  para debugar o pipeline e para o contraste within-machine
  (uniform×clustered com o confundidor ~constante); **não substitui** a
  coleta canônica homogênea. Rodar **só com a máquina ociosa** (GUI
  desligada): um lançamento às 03:17 de 2026-07-02 foi abortado e descartado
  por uso interativo concorrente. O dir `runs/workstation_skew/` (piloto
  abortado das 02:01, mal-nomeado — rodou no i5) pode ser removido.
- A coleta oficial (`PHASES=H`) segue aguardando a workstation Ryzen 9950X
  para isolar o balanceamento do confundidor P/E.

## Execution Order
1. **Task 01** (gerador) — produz o par uniforme×clustered com paridade
   bytes+matches. Bloqueia tudo.
2. **Task 02** (fase H) — **depende de Task 01** (consome os corpora). Roda na
   **workstation Ryzen 9950X**, `PHASES="H"`, sem re-rodar A–G.
3. **Task 03** (análise) — **depende de Task 02** (consome `H_skew/`).
4. **Task 05** (docs) — **depende de Task 03** (consome `skew_analysis.md`).
5. **Task 06** (LaTeX) — **depende de Task 05** (mesmos números; keys BibTeX).
6. **Task 04** (instrumentação CPU) — **opcional / paralela**. Se for feita,
   **deve preceder a Task 02** (para a fase H capturar `cpu=`). Dispensável no
   Ryzen homogêneo — pular e anotar aqui se for o caso.

## Notas
- **Máquina obrigatória:** coleta na workstation Ryzen 9950X (homogênea) para o
  argumento isolar balanceamento do confundidor P/E. Governador `performance`.
- **Invariante inegociável:** par uniforme×clustered com bytes E matches
  idênticos (moldura de Ródenas). Task 01 garante por construção (mesmos blocos,
  reordenados); Task 03 reporta a paridade.
- **Réplicas ≥5** (processos independentes) — sem isso repete o problema
  metodológico observado na fase G antiga do i5: uma única invocação por config
  pode confundir efeito experimental com variação entre corridas.
- **Ponto de decisão do usuário:** se a Task 03 mostrar que nem sob skew o
  dinâmico vence (efeito fraco por flat barato), **não** forçar narrativa —
  reportar negativo e perguntar se a família dinâmica sai do texto.
- Contexto/evidência que motiva o épico: `docs/TODO.md` (seção skew), memórias
  `skew-corpus-epic-04` e `real-scans-out-of-scope`.
