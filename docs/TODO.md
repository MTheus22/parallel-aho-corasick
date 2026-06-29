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
  `scripts/run_sweep.sh` (motor unificado) nas fases A (escalabilidade) e B
  (footprint); rerodar essas fases no i5 (`RUN_DIR=runs/i5`). **Não** rodar
  `v3/v3_flat` no Ryzen (colapsam em `v2`).
- **Pronto quando:** `v_speedup`/`v_best` do i5 incluem `pthread_dynamic_flat`;
  atualizar 9.5/9.6 com a coluna.

## P1 — Sweep de granularidade da fila dinâmica · RODOU 1×, ainda inconclusivo

- **Objetivo:** achar o ponto ótimo tarefas/thread e medir o trade-off
  (balanceamento vs. contenção no contador atômico + re-leitura de overlap).
- **Por quê:** com 4 tarefas/thread a dinâmica é quase estática (ver 9.8b). É a
  dúvida em aberto: "tarefas menores ajudariam?".
- **Infra (feita 2026-06-27):** `phase_G` no motor unificado `scripts/run_sweep.sh`
  varre `tasks_per_thread ∈ {1,4,16,64,256}` (via `AC_DYN_TASKS_PER_THREAD`, P0)
  para `dynamic` e `dynamic_flat`, Snort + `enron_corpus`, `T=MAX_T`, com timing +
  `--per-thread` por k. Opt-in: `PHASES="G" scripts/run_sweep.sh`
  (ou `RUN_DIR=runs/i5_granularidade PHASES="G" ./scripts/run_all.sh`).
  Inventário: `docs/sweep-test-inventory.md` §"Fase G".
- **Rodou no i5 (2026-06-28):** fase G executada em `runs/i5_2026-06-28/`
  (parte do re-run completo A–E+G). **Resultado inconclusivo:** com 1 invocação
  por `k`, a curva é não-física (`dynamic` cai 1362→748 MB/s de k=1→k=4) —
  dominada pela variância **entre invocações** no i5 saturado, não por
  granularidade. cv intra-run baixo (0,3–2%) **não** captura esse ruído. Parecer
  completo: [`i5-rerun-2026-06-28.md`](i5-rerun-2026-06-28.md).
- **Falta:** repetir cada `k` **N≈5×** (mediana) para mediar o ruído de regime;
  idealmente sobre um corpus de **carga desigual** (ver P1 abaixo), senão prova
  pouco. Depois rodar no **Ryzen** (a `phase_G` já está no motor unificado
  `run_sweep.sh`: basta `RUN_DIR=runs/workstation PHASES="G" ./scripts/run_all.sh`)
  e consolidar a curva.
- **Expectativa:** ganho pequeno no Ryzen (homogêneo+uniforme); ganho maior no
  **i5** (P/E heterogêneo). Sanidade preliminar (slice 100 MB, T=12,
  `dynamic_flat`): k=1 → 1288 MB/s, k=64 → 1506 MB/s (≈+17%) — mas o re-run
  completo mostra que **1 amostra por k é ruído**; tratar como hipótese.
- **Pronto quando:** nova subseção (ex. 9.8c) com curva vazão × tarefas/thread
  (com repetições/mediana) nas duas máquinas.

## P1 — Métricas dos testes: medir/explicar a variância entre corridas

- **Objetivo:** tornar os números do i5 saturado (T=12) defensáveis, separando
  **precisão** (iter-a-iter) de **reprodutibilidade** (run-a-run).
- **Por quê:** o cv reportado é `stdev/mean` das iterações **dentro de um mesmo
  processo** — vê só `σ²_within`. As alavancas grandes (núcleo P/E em que a
  thread pousou, turbo, colocação de páginas) são fixas dentro de um run e mudam
  **entre** runs → `σ²_between`, que **não medimos**. No i5
  `σ²_between ≫ σ²_within` (cv intra ~1–4% vs. swing entre corridas ~40–65%): por
  isso a fase G saiu inconclusiva (cada `k` com n=1 confunde granularidade com
  regime). Ver `i5-rerun-2026-06-28.md`.
- **Como (em ordem de prioridade):**
  - **(A) Replicar invocações.** Rodar cada config como **N≥5 processos
    independentes** (não N iterações no mesmo processo) e reportar **mediana +
    IQR / min–max** (ou IC não-paramétrico). É o único jeito de estimar
    `σ²_between`. Aplica-se à fase G e ao Teste 3.
  - **(B) Instrumentar o `--per-thread`** (hoje só `thread_id, seconds, bytes,
    matches`) para **explicar** a variância:
    - **CPU físico + classe P/E** por worker (`sched_getcpu()` amostrado ou campo
      `processor` de `/proc/<tid>/stat`) — a variável escondida nº 1 no híbrido.
    - **Frequência efetiva no laço quente** via `APERF/MPERF` (o `thermal.tsv`
      amostra só pre/post ociosos; perde o clock em-loop).
    - **Migrações / context switches involuntários** (`getrusage.ru_nivcsw`,
      `nonvoluntary_ctxt_switches` em `/proc/<tid>/status`, ou
      `perf stat cpu-migrations,context-switches`) — mede a interferência do
      escalonador, a causa suspeita.
  - **(C) Opcional — controlar** em vez de só medir: fixar as threads num cpuset
    determinístico no harness (`taskset`/`pthread_setaffinity`/cgroup `cpuset`)
    para todo run pegar o mesmo mapeamento P/E. Colapsa `σ²_between`, mas muda o
    que se mede (melhor-caso vs. realista) — declarar na metodologia.
  - **(D) Opcional — atribuir o teto:** contadores `perf` de LLC-miss e banda de
    DRAM (uncore) para fechar a narrativa memory-bound (autômato ≫ L3).
- **Nota:** o Ryzen homogêneo provavelmente quase não precisa de (B)/(C) —
  `σ²_between` deve ser pequeno por não haver heterogeneidade P/E para o
  escalonador embaralhar. O foco é o **i5**.
- **Pronto quando:** fase G e Teste 3 reportam mediana+spread sobre N réplicas; o
  `--per-thread` registra CPU/P-E (e, idealmente, freq efetiva) por worker.

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
| 4 | Métricas: replicar invocações + instrumentar `--per-thread` (CPU/P-E, freq, migrações) | P1 | conclui itens 3 e 6 |
| 5 | Corpus de carga desigual | P1 | — |
| 6 | Mais réplicas no Teste 3 | P1 | — |
| 7 | Mesmo binário/commit nas 2 máquinas | P1 | — |
| 8 | Variante CCD-aware (Ryzen) | P2 | — |
| 9 | Integrar ao TCC + slides | P2 | itens 2–7 idealmente |
