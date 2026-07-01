# Épico 04 — Corpus de carga desigual (skew) — Progress

## Tasks
- [ ] Task 01 — Gerador de corpus skew (`make_skewed_corpus.sh`) (`task-01.md`)
- [ ] Task 02 — Fase H (skew) isolada no `run_sweep.sh`, com réplicas (`task-02.md`)
- [ ] Task 03 — Ingestão + análise do spread por worker (`task-03.md`)
- [ ] Task 04 — (OPCIONAL) Instrumentar `--per-thread` com CPU físico (`task-04.md`)
- [ ] Task 05 — Documentar construção/protocolo/resultado (docs de apoio) (`task-05.md`)
- [ ] Task 06 — Complementar o texto do TCC (LaTeX) (`task-06.md`)

## Status
Not started (spec escrita 2026-07-01).

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
- **Réplicas ≥5** (processos independentes) — sem isso repete o inconclusivo da
  fase G do i5 (`docs/i5-rerun-2026-06-28.md`).
- **Ponto de decisão do usuário:** se a Task 03 mostrar que nem sob skew o
  dinâmico vence (efeito fraco por flat barato), **não** forçar narrativa —
  reportar negativo e perguntar se a família dinâmica sai do texto.
- Contexto/evidência que motiva o épico: `docs/TODO.md` (seção skew), memórias
  `skew-corpus-epic-04` e `real-scans-out-of-scope`.
