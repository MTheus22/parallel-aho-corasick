# CLAUDE.md

Guia rápido para o Claude (e qualquer agente) atuar com produtividade neste
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
```

## Comandos essenciais

| Comando                    | O que faz                                                              |
|----------------------------|------------------------------------------------------------------------|
| `make`                     | Build release (`-O3 -march=native`)                                    |
| `make debug`               | Build com `-O0 -g3`, asserts habilitados                               |
| `make asan`                | AddressSanitizer + UBSan                                               |
| `make tsan`                | ThreadSanitizer — verifica ausência de data races na fase paralela     |
| `make test`                | Executa `tests/test_correctness.c` contra todos os searchers          |
| `make bench`               | Sweep sintético em `scripts/run_benchmarks.sh`                         |
| `./build/aclab --list`     | Lista todos os searchers registrados                                   |
| `./build/aclab --help`     | Mostra todas as flags do CLI                                           |

## Searchers atualmente registrados

| Nome              | Descrição                                                              |
|-------------------|------------------------------------------------------------------------|
| `sequential`      | Baseline single-thread. Referência de correção.                        |
| `pthread_chunked` | Pthreads + chunking com overlap `max_pattern_len-1`; matches thread-local. |

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
  (`docs/`, `README.md`, `CLAUDE.md`) em português.

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

## Coisas que provavelmente NÃO devem mudar sem discussão

- A assinatura de `ac_searcher_t::search` (quebra todos os searchers).
- A função de transição `goto_tbl[state * 256 + byte]` em forma plana
  (escolha de layout para localidade de cache).
- A política de `overlap = max_pattern_len - 1` (qualquer valor menor
  perde matches em fronteira).
- Tornar `ac_automaton_t` mutável após o build (quebra ausência de locks).
