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

## P0 — Rodar `dynamic_flat` nas curvas principais ✅ MOTOR ALINHADO (2026-06-30)

- **Objetivo:** comparação i5 × Ryzen justa para a variante campeã do Ryzen.
- **Por quê:** a comparação antiga colocava `v3_flat` (i5) contra
  `dynamic_flat` (Ryzen) — conjuntos diferentes. Sem `dynamic_flat` em A/B/C/D,
  ele aparecia apenas no teste lateral de granularidade, insuficiente para
  defender um campeão canônico.
- **Como (implementado):** `scripts/run_sweep.sh` agora inclui
  `pthread_dynamic_flat` no conjunto principal da fase A e D, e inclui
  `pthread_dynamic`/`pthread_dynamic_flat` nas fases B e C para comparar
  chunking estático vs. bag-of-tasks com e sem flat output.
- **Falta:** rerodar a grade canônica (`RUN_DIR=runs/workstation
  ./scripts/run_all.sh`) e reconstruir `sweep.csv`/`sweep.db`; depois atualizar
  as seções 9.x com `dynamic_flat` lado a lado.

## P1 — Sweep de granularidade da fila dinâmica · RODOU 1×, ainda inconclusivo

- **Objetivo:** achar o ponto ótimo tarefas/thread e medir o trade-off
  (balanceamento vs. contenção no contador atômico + re-leitura de overlap).
- **Por quê:** com 4 tarefas/thread a dinâmica é quase estática (ver 9.8b). É a
  dúvida em aberto: "tarefas menores ajudariam?".
- **Infra (feita 2026-06-27):** `phase_G` no motor unificado `scripts/run_sweep.sh`
  varre `tasks_per_thread ∈ {1,4,16,64,256}` (via `AC_DYN_TASKS_PER_THREAD`, P0)
  para `dynamic` e `dynamic_flat`, Snort + `enron_corpus`, `T=MAX_T`, com timing +
  `--per-thread` por k. A fase G agora entra no default A–G; para rodá-la
  isolada use `PHASES="G" scripts/run_sweep.sh`
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

## P1 — Corpus de carga desigual / skew (justificar a família dinâmica) · ÉPICO 4

> **Especificação completa:** `specs/epic-04/` (context + tasks 01–06). Esta
> entrada é o resumo; o épico é a fonte de verdade da execução.

- **Objetivo:** um corpus onde o **custo por chunk** varie muito ao longo do
  texto, para que a divisão estática fique desbalanceada e a fila dinâmica tenha
  o que corrigir — em CPU **homogênea**, isolando o balanceamento do confundidor
  P/E.
- **Por quê (a evidência exige):** na máquina canônica (Ryzen 9950X, Enron
  uniforme) o dispatch dinâmico **não vence nada** — perde para o estático-flat
  (ET-32+Enron T=32: `chunked_flat` 18,95× > `dynamic` 17,98×; snort idem: 22,37×
  > 21,86×). O único lugar onde `dynamic` ganha é o i5 (Snort+Enron T=12: 4,79× vs
  `chunked_flat` 4,07×), e ali o ganho vem da **heterogeneidade P/E**, não de
  skew de corpus. Ou seja: hoje a tese **não tem nenhum cenário na máquina
  canônica** que justifique a família dinâmica (`dynamic`, `dynamic_flat`) —
  elas são peso morto. Enron×8 é o mesmo texto replicado 8× → perfeitamente
  uniforme; é *a* razão de a dinâmica não render. Este corpus é o **único dataset
  novo com valor real**: converte um resultado negativo ("dinâmico não ajuda")
  num condicional ("ajuda sse a carga é espacialmente desigual"), justifica a
  família dinâmica e isola o mecanismo do confundidor P/E.
- **Embasamento na RSL (literature-lookup, 2026-06-29):** **nenhum** dos 24
  papers constrói corpus *espacialmente* não-uniforme para AC nem mede ociosidade
  de barreira por thread → o skew espacial + spread por-thread é **contribuição
  própria**, não replicação. Três pilares a citar/adaptar:
  - **FHBM** (Lee & Yang 2017) — *o método* de injetar densidade: prefixos **≥80%
    do comprimento do padrão** (sorteados do set Snort) num corpus limpo a *match
    ratio* controlado. Eles injetam **uniformemente** (1%/8%/32%); a
    não-uniformidade **espacial** é a nossa extensão.
    Resumo: `related_work_summaries/a_flexible_pattern-matching__algorithm...multi-core_processors.md`.
  - **DEFCON17 "good/bad/ugly"** (Aldwairi, Alshboul & Seyam 2018) — *o alvo
    numérico* do bloco quente: traces *ugly* ≈ **43–45%** de pacotes com match.
    Respaldo empírico para o "por que ~40%".
    Resumo: `related_work_summaries/characterizing_realistic_signature-based_intrusion_detection_benchmarks.md`.
  - **Ródenas Picó** — *a moldura* "a maior fatia limita o makespan" (1/29 do
    tempo trava o speedup em ~29×) para a falha do estático. ⚠️ **Alerta bib:** o
    PDF é a tese de *graph matching* do Ródenas, **não** o paper "Parallel AC with
    NFA/OpenMP" que a key possa sugerir; o frame é NPB-MZ, não AC. Conferir a key
    correta em `acceleration-.../referencias.bib` (não inventar keys) e enquadrar
    como "moldura de balanceamento transferível", não precedente de AC.
    Resumo: `related_work_summaries/a_parallel_aho_corasick_algorithm_with_non_determi.md`.
- **Scans reais (pcap / tráfego real) ficam FORA de escopo** — decisão, não
  omissão. Regras Snort/ET são escritas contra **tráfego de rede**, não prosa de
  e-mail, então Snort∩Enron são **coincidências de bytes incidentais, não
  detecções semânticas**. Mas para um estudo de **throughput/speedup** o conteúdo
  é irrelevante (o autômato varre todo byte de qualquer forma); pcap só importaria
  para *acurácia de detecção*, que esta tese não reivindica. A densidade que
  *afeta* throughput é melhor controlada por **injeção sintética reprodutível** do
  que por pcap (que só adiciona encanamento pcap→bytes + escala p/ GB, sem achado
  novo). Ver memória `real-scans-out-of-scope`.
- **Como (receita — conteúdo heterogêneo agrupado > densidade de match):**
  1. **Alavanca primária = heterogeneidade de conteúdo** (forte e quase de
     graça): intercalar blocos "frios/baratos" (Enron limpo / runs compressíveis,
     que ficam nos estados rasos e quentes — >97% dos acessos em profundidade ≤4,
     por FHBM) com blocos "quentes/caros" (que empurram o DFA para estados frios →
     cache miss na `goto_tbl`). **Com a flat output table a emissão é barata**,
     então densidade de match sozinha rende spread só **moderado** — o custo frio
     é a alavanca real.
  2. **Densidade (FHBM) como reforço:** injetar prefixos ≥80% de padrões de
     `patterns_snort.txt` até **~40–45%** nos blocos quentes (alvo DEFCON *ugly*).
  3. **Layout = a chave:** **agrupar** os blocos quentes (ex.: no 1º quarto do
     arquivo) para sobrecarregar poucos chunks estáticos e esvaziar o resto.
  4. **Invariante de controle (Ródenas):** manter **bytes totais E matches totais
     iguais** entre a versão uniforme e a skewed — variar *só* a distribuição
     espacial, para que o spread por-thread seja atribuível ao balanceamento, não
     a "mais trabalho". **Fator de skew tunável** → curva "spread estático ×
     dinâmico vs. skew".
  5. **Disciplina:** manipular **só o corpus**; padrões reais (Snort/ET)
     **intactos**. Customizar os dois lados = benchmark circular → perde
     defensibilidade. Reportar a densidade de match obtida.
- **Pré-requisito (não pular):** o experimento **exige** primeiro o P1 de
  métricas — **N≥5 réplicas** (processos independentes) + `--per-thread`
  instrumentado. Single-shot repete o fiasco da fase G do i5 (variância entre
  corridas ±40–65% mascara o efeito). Ver `i5-rerun-2026-06-28.md`.
- **Esboço de implementação:** `scripts/make_skewed_corpus.sh` (ou integrado ao
  `prepare_data.sh`) gera o par uniforme×skewed; **fase nova e isolada** no
  `run_sweep.sh` (ex.: `PHASES="H"`) roda `v2`/`flat` × `dynamic`/`dynamic_flat`
  em T=MAX_T com `--per-thread` nos dois corpora — **sem re-rodar A–G**.
  Documentar em `testes-workstation.md` §4.
- **Medição:** spread de tempo por worker / ociosidade de barreira (contribuição
  própria — a RSL não mede isso para AC), com a moldura "maior fatia limita o
  makespan" de Ródenas.
- **Pronto quando:** Teste 3 (per-thread) nesse corpus com réplicas mostra spread
  estático ≫ dinâmico **e** um cenário onde `dynamic`/`dynamic_flat` batem o
  estático em CPU homogênea (validando o mecanismo).

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
| 2 | `dynamic_flat` nas curvas principais — motor alinhado, falta rerun | P0 | — |
| 3 | Sweep de granularidade (i5 + Ryzen) — infra pronta (`phase_G`), falta rodar | P1 | item 1 |
| 4 | Métricas: replicar invocações + instrumentar `--per-thread` (CPU/P-E, freq, migrações) | P1 | conclui itens 3 e 6 |
| 5 | Corpus de carga desigual / skew (**Épico 4**, `specs/epic-04/`) | P1 | justifica família dinâmica |
| 6 | Mais réplicas no Teste 3 | P1 | — |
| 7 | Mesmo binário/commit nas 2 máquinas | P1 | — |
| 8 | Variante CCD-aware (Ryzen) | P2 | — |
| 9 | Integrar ao TCC + slides | P2 | itens 2–7 idealmente |
