# AGENTS.md

Guia rápido para o Codex, o Claude, o OpenCode e qualquer outro agente, com
foco em produtividade neste
repositório. Mantenha este arquivo curto e factual: detalhes profundos
moram em `docs/`.

## O que é o projeto

Laboratório em C para projetar, plugar e avaliar implementações paralelas
do algoritmo **Aho–Corasick** (multi-padrão), comparando cada variante
contra um baseline sequencial. A primeira variante paralela usa
**POSIX Threads (Pthreads)** com paralelismo de dados em memória
compartilhada.

Este é o material de apoio de um TCC focado em **detecção de intrusão
(IDS)**: dicionários derivados de regras Snort/Suricata, corpus em escala
de gigabytes (Enron Email Dataset), e métricas alinhadas com a literatura
(throughput em MB/s ou Gbps, *speedup* vs. nº de threads).

**Foco de desempenho:** grandes dicionários (Snort ET, ~44 k regras,
autômato de ~515 MiB >> L3), corpus grandes (≥ 1 GiB), CPUs multicore
com memória suficiente. O regime de interesse é o *memory-bound* com
cache blowout — não dicionários pequenos que cabem em L2. Optimizações
que só funcionam no regime cache-friendly (ex.: SIMD intra-thread) são
fora de escopo para a tese e estão consolidadas em
`../tcc_notes/sections/notes/methodology.md` e `docs/searchers/README.md`.

## Layout

```
include/             Cabeçalhos públicos (automato, lista de matches,
                     API de searcher, harness de benchmark)
src/                 Núcleo: construção do autômato, lista de matches,
                     registry, benchmark, CLI (main.c)
src/searchers/       Uma implementação por arquivo. Plug-in slot.
tests/               Suíte de correção (todo searcher precisa concordar
                     com o baseline sequencial).
scripts/             Aquisição de corpus, extração de padrões e sweeps
                     de benchmark.
data/                Datasets gerados (Snort, Enron, Wikipedia). Não
                     versionado em git, recriado via scripts.
docs/                Documentação detalhada (arquitetura + searchers).
.agents/skills/      Skills locais do repositório.
.claude/skills       Symlink de compatibilidade para Claude.
.codex/              Configuração local do Codex (agents, mcps, config.toml).
```

Ao transformar resultados do laboratório em material para a escrita do
TCC, atualize primeiro `../tcc_notes/sections/notes/{methodology,results,conclusion}.md`.

## Comandos essenciais

| Comando                    | O que faz                                                              |
|----------------------------|------------------------------------------------------------------------|
| `make`                     | Build release (`-O3 -march=native`)                                    |
| `make debug`               | Build com `-O0 -g3`, asserts habilitados                               |
| `make asan`                | AddressSanitizer + UBSan                                               |
| `make tsan`                | ThreadSanitizer — verifica ausência de data races na fase paralela     |
| `make test`                | Executa `tests/test_correctness.c` contra todos os searchers          |
| `scripts/run_sweep.sh` | **Motor de sweep UNIFICADO e env-agnóstico** (default A–G, 10 paralelos + 2 seq; 562 runs em `MAX_T=32`). Deriva `MAX_T=nproc` e o `RUN_DIR` (slug da CPU). Substitui `run_i5_sweep.sh`/`run_workstation_sweep.sh` (legados). |
| `scripts/prepare_data.sh` | Pré-flight de dados env-agnóstico: gera `enron_x8` + dicts reduzidos; valida `simplewiki.txt`; restaura `patterns_et_32.txt` do git se faltar; imprime `PRONTO`. Substitui `prepare_workstation_data.sh`. |
| `scripts/run_all.sh` | **Wrapper "um comando" env-agnóstico**: pull opc. + pré-flight + governador (amd/intel-pstate) + build + test fatal, depois **desacopla** o sweep (sobrevive a logout/suspensão) e faz upload/notificação best-effort (`AC_GIT_PUSH`/`AC_GH_PAT`, `AC_UPLOAD_CMD`, `AC_NOTIFY`). Ex.: `RUN_DIR=runs/workstation ./scripts/run_all.sh`. Substitui `i5_all.sh`/`workstation_all.sh` (legados). |
| `scripts/extract_sweep_csv.py` + `build_sweep_db.py` | Pós-sweep: logs → `sweep.csv` → SQLite `sweep.db` (consulta token-efficient) |
| `./build/aclab --list`     | Lista todos os searchers registrados                                   |
| `./build/aclab --help`     | Mostra todas as flags do CLI                                           |

## Searchers atualmente registrados

| Nome                  | Descrição                                                                       |
|-----------------------|----------------------------------------------------------------------------------|
| `sequential`          | Baseline single-thread. Referência de correção.                                 |
| `pthread_chunked`     | Pthreads + chunking com overlap `max_pattern_len-1`; matches thread-local.      |
| `pthread_chunked_v2`  | v1 com split warm-up/owned loops e cache-pad em `worker_t`.                     |
| `pthread_chunked_v3`  | v2 + afinidade ciente de topologia + chunks ponderados por `cpufreq` (híbridas).|
| `pthread_dynamic`     | Dispatch dinâmico de chunks via contador atômico (4N tarefas).                  |
| `pthread_dynamic_flat`| `pthread_dynamic` (bag of tasks) + emissão pela tabela achatada (idea 5). **Campeão** em cores homogêneos (workstation 06-30). |
| `pthread_prefetch`    | v2 + `__builtin_prefetch(text + Δ)` para cobrir latência DRAM residual.         |
| `sequential_flat`     | Sequential AC scan lendo a tabela achatada de saídas (idea 5).                  |
| `pthread_chunked_flat`| `pthread_chunked_v2` + emissão pela tabela achatada (idea 5).                   |
| `pthread_chunked_v3_flat`| `pthread_chunked_v3` (topology + freq weights) + emissão flat (idea 5+7).    |
| `pattern_sharded_prefix`| Sharding do dicionário (idea 1), bucketing pelo primeiro byte. K sub-autômatos.|
| `pthread_2d_sharded_chunked`| idea 6 — 2-D K × N (prefix shards × text chunks); default K=2 (env `AC_2D_K`).|

Documentação por searcher: `docs/searchers/<nome>.md`.

## Como adicionar um novo searcher

1. Crie `src/searchers/<nome>.c`.
2. Implemente a função compatível com `ac_searcher_t::search`
   (assinatura em `include/ac_searcher.h`).
3. Registre via `__attribute__((constructor))` chamando
   `ac_searcher_register(...)`.
4. Compile (`make`) — o `Makefile` pega o arquivo por wildcard.
5. `make test` — sua implementação **precisa** concordar com `sequential`
   em todos os casos de teste e em todas as contagens de threads
   `{1,2,3,4,7,8}`.
6. Opcional mas recomendado: `make tsan` para validar a ausência de
   races no automato (que é estritamente read-only após `ac_automaton_build`).
7. `make bench` para medir throughput.

Nenhum outro arquivo precisa mudar — o registry descobre o searcher pelo
construtor.

## Invariantes que NÃO podem ser violadas

- Após `ac_automaton_build()`, o autômato é **estritamente read-only**.
  Nenhum searcher pode mutar `goto_tbl`, `own_out_head`, `dict_suffix`,
  `outputs` ou `patterns`. Toda a paralelização depende disso para
  dispensar locks.
- Fase de busca **não pode** usar locks/mutexes/atomics no caminho quente.
  Matches são coletados em listas thread-local; o merge é feito
  sequencialmente pelo master thread após `pthread_join`.
- Para qualquer particionamento por chunks, o overlap **mínimo** entre
  chunks adjacentes é `max_pattern_len - 1` bytes (warm-up do DFA).
  Matches cujo `end_pos` cai dentro da região de warm-up pertencem ao
  worker anterior e devem ser descartados localmente.

## Convenções de código

- C11, `_POSIX_C_SOURCE=200809L`, `_GNU_SOURCE`.
- Flags de warning agressivas: `-Wall -Wextra -Wpedantic -Wshadow
  -Wstrict-prototypes -Wmissing-prototypes -Wpointer-arith -Wcast-align`.
  Todos os PRs devem compilar limpo.
- Macros úteis em `ac_common.h`: `AC_LIKELY`, `AC_UNLIKELY`,
  `AC_INLINE`, `AC_RESTRICT`, `AC_NIL`, `AC_ALPHABET_SIZE = 256`.
- Códigos de erro são negativos: `AC_OK = 0`, `AC_E_NOMEM`, `AC_E_INVAL`,
  `AC_E_PATTERN_EMPTY`, `AC_E_NOT_FOUND`, `AC_E_THREAD`.
- Comentários: foco em **por que**, não em **o que**. Justifique
  invariantes não óbvias, decisões de layout (cache locality, etc.) e
  pontos de sincronização.
- Idioma: comentários e identificadores em inglês; documentação
(`docs/`, `README.md`, `AGENTS.md`) em português.

## Workflow recomendado

1. Antes de codar, leia `docs/architecture/overview.md` e o arquivo do
   searcher mais próximo do que você quer construir.
2. Use `make tsan` cedo: data races silenciosos no autômato são o erro
   mais difícil de diagnosticar tarde.
3. Rode `make test` a cada commit que toque searcher ou autômato.
4. Para benchmarks, use sempre `--warmup >= 1` (a fase de warm-up paga
   page faults e aquece os caches).

## Sobre datasets

- Corpus pequeno e barato para iteração: `data/simplewiki.txt`
  (`./scripts/acquire_corpus.sh`).
- Corpus realista para o TCC: `data/enron_corpus.txt` (~1.4 GiB).
- Padrões reais para IDS: `data/patterns_snort.txt`
  (`./scripts/prepare_datasets.sh`).

Detalhes em `data/README.md` e em `docs/architecture/datasets.md`.

## Documentação adicional

- `docs/architecture/overview.md` — visão geral do sistema, fluxo
  master → workers, contratos de memória.
- `docs/architecture/automaton.md` — construção do autômato, função
  goto, falha e link de sufixo de dicionário.
- `docs/architecture/parallelism.md` — estratégia de paralelismo,
  overlap, ownership de matches, merge final.
- `docs/architecture/benchmark-harness.md` — como o harness mede.
- `docs/architecture/benchmark-protocol.md` — protocolo experimental
  do TCC (sweeps, métricas, ambiente).
- `docs/architecture/datasets.md` — origem e preparação dos datasets.
- `docs/searchers/sequential.md` — baseline, passo a passo.
- `docs/searchers/pthread_chunked.md` — paralelo por chunks com overlap.
- `docs/searchers/pthread_chunked_v2.md` — split-loop + cache-pad.
- `docs/searchers/pthread_chunked_v3.md` — topology-aware affinity +
  freq-weighted chunks.
- `docs/searchers/pthread_dynamic.md` — dispatch dinâmico atômico.
- `docs/searchers/pthread_dynamic_flat.md` — bag of tasks (dispatch
  dinâmico) + flat output table (idea 5); **campeão** em cores homogêneos
  (workstation 06-30).
- `docs/searchers/pthread_prefetch.md` — software prefetch do stream
  de texto.
- `docs/searchers/pthread_chunked_v3_flat.md` — composição v3 (topology
  + freq weights) + flat output table (ideas 5 e 7).
- `docs/searchers/pthread_2d_sharded_chunked.md` — 2-D K × N
  (sharding por dicionário × chunking de texto), idea 6.
- `runs/QUERY_GUIDE.md` — schema + views + queries dos `sweep.db` preservados
  (forma token-efficient de consultar resultados via `sqlite3`).
- `runs/MANIFEST.md` — **política dos runs preservados**: só `workstation_2026-06-30/`
  (canônico) e `i5/` (P/E) existem; lacunas conhecidas (single-run por config;
  corpus ~uniforme). Leia antes de citar ou criar runs.
- `runs/workstation_2026-06-30/RESULTS.md` — **análise da coleta canônica**
  (Ryzen 9 9950X, 2026-06-30): run único ponta-a-ponta (commit `c19da78`, 522 ok /
  0 fail), fases A B C D E + G, campeão `pthread_dynamic_flat`; também contém o
  mapa de leitura humana das fases.
- `docs/TODO.md` — melhorias pendentes; vários itens já resolvidos no run 06-30
  (fase G, `dynamic_flat`). Lacunas vivas: réplicas por config, corpus skewed.
- `../tcc_notes/sections/notes/` — consolidação orientada a seção do TCC
  (`methodology`, `results`, `conclusion`).

> **Fonte canônica do TCC = workstation Ryzen 9 9950X** (16C/32T homogêneo).
> Coleta canônica: `runs/workstation_2026-06-30/` (análise em
> `runs/workstation_2026-06-30/RESULTS.md`). Run único ponta-a-ponta no commit
> `c19da78` (**522 ok / 0 skip / 0 fail**, correctness 100%), fases A B C D E + G
> com `pthread_dynamic_flat` em todas. **Campeão: `pthread_dynamic_flat` @ T=32**
> (snort 22,91× / et_32 18,96×; baseline seq 329,2 / 209,8 MB/s). ⚠️ **Single-run
> por configuração** (ver `runs/MANIFEST.md`) — não afirme variância entre corridas
> que não foi medida.
>
> O i5 **deixou de ser canônico**: `runs/i5/sweep.db` (**2026-05-29**) serve só
> aos números que o LaTeX já cita (até a migração) e à **seção de P/E** (única
> máquina híbrida P+E).
>
> **Limpeza 2026-07-01:** os runs interinos/mistos/reduzidos e relatos históricos
> foram **removidos**. Só existem `workstation_2026-06-30/` e `i5/`. Não cite
> runs removidos; runs novos vão em `runs/<slug-data>/` (não sobrescreva um run
> promovido).

## Coisas que provavelmente NÃO devem mudar sem discussão

- A assinatura de `ac_searcher_t::search` (quebra todos os searchers).
- A função de transição `goto_tbl[state * 256 + byte]` em forma plana
  (escolha de layout para localidade de cache).
- A política de `overlap = max_pattern_len - 1` (qualquer valor menor
  perde matches em fronteira).
- Tornar `ac_automaton_t` mutável após o build (quebra ausência de locks).
