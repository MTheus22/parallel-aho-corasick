# TODO — melhorias do estudo experimental (Aho–Corasick paralelo)

Lista de trabalho para fortalecer os resultados antes de fechar o TCC.
Referências de caminho relativas à raiz deste repositório.
Prioridades: **P0** = necessário p/ rigor/justeza · **P1** = fortalece achados ·
**P2** = futuro / nice-to-have.

Fontes de verdade: `parallel-aho-corasick/runs/workstation_2026-06-30/sweep.db`
(Ryzen, **canônica**) e `parallel-aho-corasick/runs/i5/sweep.db` (i5, P/E). Doc de
resultados: `runs/workstation_2026-06-30/RESULTS.md`. Runs antigos foram
removidos; ver `runs/MANIFEST.md` antes de citar qualquer base. Piloto
exploratório novo: `runs/i5_2026-07-02/` (R+H, não canônico; análise em
`runs/i5_2026-07-02/RESULTS.md`).

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

## P0 — Rodar `dynamic_flat` nas curvas principais ✅ FEITO (2026-06-30)

- **Objetivo:** comparação i5 × Ryzen justa para a variante campeã do Ryzen.
- **Por quê:** a comparação antiga colocava `v3_flat` (i5) contra
  `dynamic_flat` (Ryzen) — conjuntos diferentes. Sem `dynamic_flat` em A/B/C/D,
  ele aparecia apenas no teste lateral de granularidade, insuficiente para
  defender um campeão canônico.
- **Como (implementado):** `scripts/run_sweep.sh` agora inclui
  `pthread_dynamic_flat` no conjunto principal da fase A e D, e inclui
  `pthread_dynamic`/`pthread_dynamic_flat` nas fases B e C para comparar
  chunking estático vs. bag-of-tasks com e sem flat output.
- **Pronto quando:** ✔ coleta canônica em
  `runs/workstation_2026-06-30/sweep.db`, com `pthread_dynamic_flat` em A/B/C/D/G
  e análise em `runs/workstation_2026-06-30/RESULTS.md`.

## P1 — Sweep de granularidade da fila dinâmica ✅ RODOU no Ryzen; réplicas ainda melhorariam

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
- **Resultado canônico (Ryzen 2026-06-30):** `pthread_dynamic_flat`, Snort/Enron,
  T=32 cresce monotonicamente de tpt001→tpt256 (7.414→7.804 MB/s), com cv
  <0,3%. Ver `runs/workstation_2026-06-30/RESULTS.md` e `runs/QUERY_GUIDE.md`.
- **Lição do i5:** a fase G antiga em i5 com 1 invocação por `k` foi dominada por
  variância entre invocações no regime saturado; o run foi removido e não deve
  ser citado como número. Use a lição metodológica: se a fase G virar claim
  estatístico, repetir cada `k` como **N≥5 processos independentes**.
- **Falta:** repetir a fase G com réplicas se ela entrar como evidência forte, de
  preferência no corpus skewed do Épico 04.
- **Pronto quando:** fase G reporta mediana + IQR/min-max por `k`, ou fica
  explicitamente tratada como evidência exploratória single-run.

## P1 — Métricas dos testes: medir/explicar a variância entre corridas

- **Objetivo:** tornar os números do i5 saturado (T=12) defensáveis, separando
  **precisão** (iter-a-iter) de **reprodutibilidade** (run-a-run).
- **Por quê:** o cv reportado é `stdev/mean` das iterações **dentro de um mesmo
  processo** — vê só `σ²_within`. As alavancas grandes (núcleo P/E em que a
  thread pousou, turbo, colocação de páginas) são fixas dentro de um run e mudam
  **entre** runs → `σ²_between`, que **não medimos**. No i5
  `σ²_between ≫ σ²_within` (cv intra ~1–4% vs. swing entre corridas ~40–65%): por
  isso a fase G antiga do i5 saiu inconclusiva (cada `k` com n=1 confunde
  granularidade com regime). O run antigo foi removido; não cite seus números.
- **Como (em ordem de prioridade):**
  - **(A) Replicar invocações. ✅ MOTOR PRONTO; piloto i5 coletado (2026-07-02).**
    Rodar cada config como **N≥5 processos independentes** (não N iterações no
    mesmo processo) e reportar **mediana + IQR / min–max** (ou IC
    não-paramétrico). É o único jeito de estimar `σ²_between`. Implementado no
    `run_sweep.sh`: fase **R** (curvas replicadas, réplicas *intercaladas* —
    rep no laço externo — envs `AC_REP_*`), fase **D** com `AC_PHASE_D_REPS`
    (default 5) e fase **H** com `AC_SKEW_REPS`. Primeira coleta real:
    `runs/i5_2026-07-02/` (R+H, 255 ok / 0 fail; análise em `RESULTS.md`).
    Continua faltando a coleta canônica/limpa na workstation homogênea para
    transformar o achado em evidência principal.
  - **(B) Instrumentar o `--per-thread`** (hoje só `thread_id, seconds, bytes,
    matches`) para **explicar** a variância:
    - ✅ **CPU físico por worker — FEITO (2026-07-02):** `sched_getcpu()` ao
      fim do scan de cada worker → campo `cpu=` na saída `--per-thread` →
      coluna `cpu` na tabela `workers` do `sweep.db`. Classe P/E deriva do
      número (i5-1235U: cpu 0–3 = P/4,4 GHz com HT; cpu 4–11 = E/3,3 GHz).
      Limitação: amostra o CPU no fim do scan — migrações no meio do laço não
      aparecem (ver item de migrações abaixo).
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

## P1 — Corpus de carga desigual / skew (justificar a família dinâmica) · ÉPICO 4 — corpora GERADOS; fase H implementada

> **Especificação completa:** `specs/epic-04/` (context + tasks 01–06). Esta
> entrada é o resumo; o épico é a fonte de verdade da execução.

> **Estado (2026-07-02):** tasks 01–03 **implementadas/validadas no piloto i5**.
> `scripts/make_skewed_corpus.py` gerou os 4 corpora (4 GiB cada:
> `enron_skew_{uniform,s0.5,clustered,s0.1}`) com **paridade exata de bytes e
> matches (94 304 128)** — só a distribuição espacial muda; re-rodar o script
> re-verifica a paridade dos arquivos existentes. Densidade: alvo implementado
> como **0,05 matches/byte no bloco quente** (global 0,022) — o "43–45%" do
> DEFCON é fração de *pacotes* com match, unidade incomparável; registrar essa
> interpretação na metodologia. A `phase_H` do `run_sweep.sh` roda
> {v2, **v3**, chunked_flat, dynamic, dynamic_flat} × {snort, et_32} ×
> {uniform, clustered}, **≥5 réplicas** (processos independentes) com
> `--per-thread` + campo `cpu=` (listas via `AC_SKEW_*`). O **piloto no i5**
> foi coletado em `runs/i5_2026-07-02/`: 255 ok / 0 fail; H confirma o mecanismo
> no i5 (clustered derruba estáticos e favorece dinâmicos), mas permanece
> confundido por P/E. A **coleta canônica continua exigindo a workstation
> homogênea**.

- **Objetivo:** um corpus onde o **custo por chunk** varie muito ao longo do
  texto, para que a divisão estática fique desbalanceada e a fila dinâmica tenha
  o que corrigir — em CPU **homogênea**, isolando o balanceamento do confundidor
  P/E.
- **Por quê (a evidência exige):** na coleta canônica (Ryzen 9950X, 06-30,
  Enron ~uniforme) o campeão `pthread_dynamic_flat` vence o estático
  `chunked_flat` por margens que **não demonstram o mecanismo**: +1,9%
  (snort/enron: 22,91× vs 22,48×), +1,6% (snort/x8), +0,7% (et_32/enron) e
  **−0,2%** (et_32/x8 — empate técnico). ⚠️ Os números interinos que esta seção
  citava (22,37×/21,86× etc.) eram do run 06-29 removido — não reintroduzir.
  Num corpus uniforme a fila dinâmica quase não tem o que corrigir; o único
  lugar onde a família dinâmica ganha com folga é o i5 (Snort+Enron T=12:
  4,79× vs 4,07× do `chunked_flat`), e ali o ganho vem da **heterogeneidade
  P/E**, não de skew de corpus. Enron e Enron×8 são ~uniformes por construção.
  Este corpus é o dataset novo com valor real: converte "empata/vence por
  margem dentro do ruído" num condicional testado ("ajuda sse a carga é
  espacialmente desigual") e isola o mecanismo do confundidor P/E.
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
     `patterns_snort.txt` nos blocos quentes. *Como implementado:* alvo de
     **0,05 matches/byte** no bloco quente (o "43–45%" do DEFCON *ugly* é
     fração de pacotes com match — unidade não transponível a matches/byte).
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
- **Pré-requisito (SATISFEITO 2026-07-02):** o experimento exige **N≥5
  réplicas** (processos independentes) + `--per-thread` instrumentado — ambos
  implementados: `phase_H` roda `AC_SKEW_REPS` (default 5) processos por
  config e o `--per-thread` agora emite `cpu=` por worker (coluna `cpu` no
  `sweep.db`). Single-shot repetiria o problema metodológico da fase G antiga
  do i5 (variância entre corridas mascara o efeito).
- **Implementação (FEITA; era o esboço):** `scripts/make_skewed_corpus.{sh,py}`
  gera e re-valida os corpora (fator de skew tunável via env `SKEWS`;
  `HOT_FRAC`, `DENSITY`, `SIZE`); `PHASES="H" scripts/run_sweep.sh` roda a
  fase isolada — **sem re-rodar A–G** — com listas em
  `AC_SKEW_SEARCHERS/AC_SKEW_PATS/AC_SKEW_CORPORA`. Piloto i5 documentado em
  `runs/i5_2026-07-02/RESULTS.md`. Falta: rodar na workstation, documentar no
  `RESULTS.md` do run promovido e consolidar em
  `../tcc_notes/sections/notes/{methodology,results}.md` (tasks 04–06 do épico).
- **Medição:** spread de tempo por worker / ociosidade de barreira (contribuição
  própria — a RSL não mede isso para AC), com a moldura "maior fatia limita o
  makespan" de Ródenas.
- **Pronto quando:** Teste 3 (per-thread) nesse corpus com réplicas mostra spread
  estático ≫ dinâmico **e** um cenário onde `dynamic`/`dynamic_flat` batem o
  estático em CPU homogênea. O piloto i5 já confirma a direção do mecanismo, mas
  ainda não substitui a coleta homogênea.

## P1 — Mais réplicas na decomposição por thread (Teste 3)

- **Objetivo:** dar robustez estatística aos números por-worker.
- **Por quê:** a fase `D_per_thread` rodou só **1 aquecimento + 3 medidas** (4
  execuções). As conclusões de spread/cauda repousam sobre amostra fina.
- **Como:** subir para ≥ 5 medidas; reportar mediana e desvio do spread entre
  réplicas. Confirmar que o spread (4,8–8,0%) é estável, não ruído.
- **Estado (2026-07-02):** `phase_D` agora roda `AC_PHASE_D_REPS` (default 5)
  réplicas por searcher — implementado, **falta coletar na workstation** (a
  fase D canônica de 06-30 ainda é single-run).
- **Pronto quando:** 9.8/9.8b com cv entre réplicas reportado.

## P1 — Transparência do i5 na seção P/E ✅ ATENDIDO no texto (2026-07-02)

> A seção P/E escrita pelo epic-03 (task 06; `partes/results.tex`,
> `subsec:heterogeneous`) e a conclusão enquadram o i5 como **diagnóstico de
> heterogeneidade**, não como segundo headline nem disputa i5×Ryzen. Mantido
> aqui como guarda-corpo para edições futuras.

- **Objetivo:** deixar claro que `runs/i5/sweep.db` é evidência P/E histórica,
  não comparação headline contra a workstation canônica.
- **Por quê:** o i5 é a única máquina híbrida P+E preservada, mas não foi coletado
  no mesmo protocolo/commit da workstation 2026-06-30. Usá-lo como ranking entre
  máquinas seria metodologicamente fraco; usá-lo para explicar `chunked_v3` e
  `chunked_v3_flat` é aceitável.
- **Como:** na seção P/E, citar commit/protocolo do i5 se disponível no DB/logs,
  declarar que a evidência é histórica e limitar a conclusão ao efeito de
  topologia/heterogeneidade.
- **Pronto quando:** o texto do TCC não apresenta i5×Ryzen como disputa direta e
  toda menção ao i5 remete à política de `runs/MANIFEST.md`.

## P2 — Variante CCD-aware para o Ryzen (ângulo de topologia homogênea)

- **Objetivo:** explorar o L3 **não unificado** (2 CCD × 32 MiB) do Ryzen.
- **Por quê:** núcleos são homogêneos, mas a cache não — atravessar CCDs custa
  caro (Infinity Fabric). Pode deslocar o ponto de saturação (Teste 2, ET-32) e
  render um achado novo, análogo ao que `v3` rende no i5.
- **Como:** testar pinagem (preencher 1 CCD = 8 núcleos antes de transbordar p/
  o outro) vs. espalhar; comparar vazão em ET-32 (autômato ≫ L3 por CCD).
- **Pronto quando:** medição de "1 CCD vs. 2 CCD" para ET-32 documentada.

## P2 — Integrar números ao TCC e aos slides — LaTeX ✅ FEITO; slides/notes pendentes

- **Objetivo:** levar os resultados da workstation para o texto final.
- **Feito (epic-03, concluído 2026-07-02):** `methodology.tex`, `results.tex`,
  `conclusion.tex`, abstract e introdução **já citam os números canônicos do
  06-30** (auditados nº a nº contra o `sweep.db`).
- **Pendente:** `apresentacao/slides.md` (regenerar html/pdf via Marp),
  `tcc_notes/sections/notes/{results,conclusion}.md` e
  `docs/tcc-synthesis.html` — ainda alinhados ao i5/2026-05-29; migrar num
  passo coordenado. Garantir que todo headline bate com `sweep.db`.
- **Pronto quando:** slides e fontes de apoio citam os números do Ryzen,
  consistentes com as fontes canônicas.

---

## Resumo de prioridades

| # | Item | Prioridade | Bloqueia |
|---|---|---|---|
| 1 | Tamanho de tarefa em runtime ✅ | P0 (feito) | sweep de granularidade |
| 2 | `dynamic_flat` nas curvas principais ✅ (coleta canônica 06-30) | P0 (feito) | — |
| 3 | Sweep de granularidade ✅ rodou no Ryzen (single-run); réplicas só se virar claim forte | P1 | item 1 |
| 4 | Métricas: réplicas ✅ motor (fases R/D/H) + `cpu=` ✅; piloto i5 R+H coletado; freq/migrações e workstation pendentes | P1 | conclui itens 3 e 6 |
| 5 | Corpus skew (**Épico 4**): corpora ✅ + fase H ✅; piloto i5 coletado/analisado (`runs/i5_2026-07-02/RESULTS.md`); falta coleta canônica (workstation) | P1 | justifica família dinâmica |
| 6 | Mais réplicas no Teste 3 — fase D com reps ✅ motor; falta coletar na workstation | P1 | — |
| 7 | Mesmo binário/commit nas 2 máquinas | P1 | — |
| 8 | Variante CCD-aware (Ryzen) | P2 | — |
| 9 | Integrar ao TCC ✅ (LaTeX, epic-03); slides + notes/synthesis pendentes | P2 | itens 2–7 idealmente |
