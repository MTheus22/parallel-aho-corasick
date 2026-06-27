# TODO — melhorias do estudo experimental (Aho–Corasick paralelo)

Lista de trabalho para fortalecer os resultados antes de fechar o TCC.
Referências de caminho relativas a este diretório (`tcc/`).
Prioridades: **P0** = necessário p/ rigor/justeza · **P1** = fortalece achados ·
**P2** = futuro / nice-to-have.

Fontes de verdade: `parallel-aho-corasick/runs/workstation/sweep.db` (Ryzen) e
`parallel-aho-corasick/runs/i5/sweep.db` (i5). Doc de resultados:
`testes-workstation.md`.

---

## P0 — Tornar o tamanho de tarefa configurável em runtime ✅ FEITO (2026-06-27)

- **Objetivo:** poder variar a granularidade da fila dinâmica sem recompilar.
- **Por quê:** antes `K_PER_THREAD` era `#define 4` em
  `pthread_dynamic.c` **e** `pthread_dynamic_flat.c`. Qualquer sweep de
  granularidade exigiria N recompilações — inviável de automatizar.
- **Como (implementado):** CLI `--tasks-per-thread N` em `src/main.c`
  (passa por `cfg->tasks_per_thread`, novo campo em `ac_searcher_config_t`)
  **e** env `AC_DYN_TASKS_PER_THREAD`. Ambos os searchers resolvem
  CLI → env → default 4 via `resolve_tasks_per_thread()`. `--per-thread`
  continua funcionando.
- **Pronto quando:** ✔ `build/aclab --searcher pthread_dynamic_flat
  --tasks-per-thread 64 ...` faz `num_tasks = 64·nthreads` e o header ecoa
  `tasks_per_thread=64` (texto e CSV). Correção idêntica (`make test`) e
  TSan-clean nas duas variantes. Desbloqueia o item P1 (sweep de granularidade).

## P0 — Rodar `dynamic_flat` no i5 (fechar lacuna da comparação)

- **Objetivo:** comparação i5 × Ryzen justa para a variante campeã do Ryzen.
- **Por quê:** o sweep do i5 só tem `pthread_dynamic` (sem flat). Hoje a 9.6
  compara `v3_flat` (i5) com `dynamic_flat` (Ryzen) — conjuntos diferentes.
- **Como:** acrescentar `pthread_dynamic_flat` ao set de variantes do
  `scripts/run_i5_sweep.sh` (i5) nas fases A (escalabilidade) e B
  (footprint); rerodar essas fases. **Não** rodar `v3/v3_flat` no Ryzen (colapsam
  em `v2`).
- **Pronto quando:** `v_speedup`/`v_best` do i5 incluem `pthread_dynamic_flat`;
  atualizar 9.5/9.6 com a coluna.

## P1 — Sweep de granularidade da fila dinâmica · INFRA PRONTA, falta rodar

- **Objetivo:** achar o ponto ótimo tarefas/thread e medir o trade-off
  (balanceamento vs. contenção no contador atômico + re-leitura de overlap).
- **Por quê:** com 4 tarefas/thread a dinâmica é quase estática (ver 9.8b). É a
  dúvida em aberto: "tarefas menores ajudariam?".
- **Infra (feita 2026-06-27):** `phase_G` em `scripts/run_i5_sweep.sh` varre
  `tasks_per_thread ∈ {1,4,16,64,256}` (via `AC_DYN_TASKS_PER_THREAD`, P0) para
  `dynamic` e `dynamic_flat`, Snort + `enron_corpus`, `T=MAX_T`, com timing +
  `--per-thread` por k. Opt-in: `PHASES="G" scripts/run_i5_sweep.sh`
  (ou `RUN_DIR=runs/i5_granularidade PHASES="G" ./scripts/i5_all.sh`).
  Inventário: `docs/sweep-test-inventory.md` §"Fase G".
- **Falta:** rodar no **i5** (`PHASES="G"`) e no **Ryzen** (adicionar `phase_G`
  análoga a `run_workstation_sweep.sh`, ou rodar via env); consolidar a curva.
- **Expectativa:** ganho pequeno no Ryzen (homogêneo+uniforme); ganho maior no
  **i5** (P/E heterogêneo, onde a dinâmica tem desbalanceamento real a explorar).
  Sanidade preliminar (slice 100 MB, T=12, `dynamic_flat`): k=1 → 1288 MB/s,
  k=64 → 1506 MB/s (≈+17%), coerente com a hipótese.
- **Pronto quando:** nova subseção (ex. 9.8c) com curva vazão × tarefas/thread
  nas duas máquinas.

## P1 — Corpus de carga desigual (exercitar de fato o balanceamento)

- **Objetivo:** um corpus onde a densidade de matches varie muito ao longo do
  texto, para que a divisão estática fique desbalanceada e a dinâmica tenha o
  que corrigir.
- **Por quê:** Enron×8 é o mesmo texto replicado 8× → perfeitamente uniforme e
  periódico. É *a* razão de a dinâmica não render aqui (matches por fatia ~iguais,
  ver ciclo de 4 na 9.8b). Sem isso, o sweep de granularidade prova pouco.
- **Como:** concatenar corpora heterogêneos (ex.: trechos densos em assinaturas +
  trechos "limpos"), ou ordenar o texto por densidade para criar hot spots.
  Documentar a construção em `testes-workstation.md` §4.
- **Pronto quando:** repetir Teste 3 nesse corpus; esperar spread estático ≫
  dinâmico (validando o mecanismo).

## P1 — Mais réplicas na decomposição por thread (Teste 3)

- **Objetivo:** dar robustez estatística aos números por-worker.
- **Por quê:** a fase `D_per_thread` rodou só **1 aquecimento + 3 medidas** (4
  execuções). As conclusões de spread/cauda repousam sobre amostra fina.
- **Como:** subir para ≥ 5 medidas; reportar mediana e desvio do spread entre
  réplicas. Confirmar que o spread (4,8–8,0%) é estável, não ruído.
- **Pronto quando:** 9.8/9.8b com cv entre réplicas reportado.

## P1 — Reprodutibilidade: mesmo binário/commit nas duas máquinas

- **Objetivo:** garantir que i5 e Ryzen medem **o mesmo código**.
- **Por quê:** os sweeps são de datas diferentes (i5 2026-05-29; Ryzen
  2026-06-25) e o i5 tem variantes (`v3`, `prefetch`) ausentes no Ryzen. Confirmar
  que as implementações compartilhadas não divergiram entre os commits.
- **Como:** registrar `git rev-parse HEAD` no `env/` de cada corrida (já há
  `git:` no header dos logs — conferir se batem); idealmente recompilar e
  re-rodar ambas do mesmo commit/flags.
- **Pronto quando:** commit e flags de build idênticos anotados nos dois `sweep.db`.

## P2 — Variante CCD-aware para o Ryzen (ângulo de topologia homogênea)

- **Objetivo:** explorar o L3 **não unificado** (2 CCD × 32 MiB) do Ryzen.
- **Por quê:** núcleos são homogêneos, mas a cache não — atravessar CCDs custa
  caro (Infinity Fabric). Pode deslocar o ponto de saturação (Teste 2, ET-32) e
  render um achado novo, análogo ao que `v3` rende no i5.
- **Como:** testar pinagem (preencher 1 CCD = 8 núcleos antes de transbordar p/
  o outro) vs. espalhar; comparar vazão em ET-32 (autômato ≫ L3 por CCD).
- **Pronto quando:** medição de "1 CCD vs. 2 CCD" para ET-32 documentada.

## P2 — Integrar números ao TCC e aos slides

- **Objetivo:** levar os resultados da workstation para o texto final.
- **Por quê:** `testes-workstation.md` é doc de apoio; o TCC ainda precisa
  consumir esses números.
- **Como:** portar tabelas-chave para
  `acceleration-of-.../partes/results.tex` (e `conclusion.tex`); atualizar
  `apresentacao/slides.md` (regenerar html/pdf via Marp); sincronizar
  `tcc_notes/sections/notes/{results,conclusion}.md`. Garantir que todo headline
  bate com `sweep.db`.
- **Pronto quando:** results.tex e slides citam os números do Ryzen, consistentes
  com as fontes canônicas.

---

## Resumo de prioridades

| # | Item | Prioridade | Bloqueia |
|---|---|---|---|
| 1 | Tamanho de tarefa em runtime ✅ | P0 (feito) | sweep de granularidade |
| 2 | `dynamic_flat` no i5 | P0 | — |
| 3 | Sweep de granularidade (i5 + Ryzen) — infra pronta (`phase_G`), falta rodar | P1 | item 1 |
| 4 | Corpus de carga desigual | P1 | — |
| 5 | Mais réplicas no Teste 3 | P1 | — |
| 6 | Mesmo binário/commit nas 2 máquinas | P1 | — |
| 7 | Variante CCD-aware (Ryzen) | P2 | — |
| 8 | Integrar ao TCC + slides | P2 | itens 2–6 idealmente |
