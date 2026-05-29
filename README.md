# Parallel Aho–Corasick Research Laboratory

Laboratório modular em C para projetar, plugar e avaliar
implementações paralelas do algoritmo **Aho–Corasick** de
correspondência multi-padrão contra um baseline sequencial. O alvo é
**CPUs multi-core em memória compartilhada**, e a primeira variante
paralela usa a API **POSIX Threads (Pthreads)**.

O projeto serve como base experimental de um TCC orientado a
**detecção de intrusão (IDS)**: o dicionário de padrões vem de regras
reais (Snort/Suricata) e o corpus é grande o suficiente (multi-GiB)
para que o tempo total seja dominado pela fase de varredura, não por
I/O ou pelo custo de criação de threads.

> Para o material conceitual completo (arquitetura, decisões de
> projeto, fluxos e diagramas), veja [`docs/`](docs/). Para um
> arranque rápido como agente do repositório, veja
> [`CLAUDE.md`](CLAUDE.md).

## Objetivos do laboratório

- **Implementar** uma versão paralela da fase de busca (matching) do
  Aho–Corasick, maximizando o throughput em arquivos grandes.
- **Comparar** essa variante de forma justa contra um baseline
  sequencial, em correção (1:1) e em métricas alinhadas com a
  literatura (throughput em MiB/s ou Gbps; speedup vs. nº de threads).
- **Servir de plataforma** para futuras variantes (SIMD, lock-free,
  NUMA-aware, etc.), sem reescrever a infraestrutura.

## Estratégia de paralelização

A implementação é dividida em **duas fases bem separadas no tempo**.

### 1. Construção do autômato (sequencial — master thread)

O master constrói trie, função goto determinística, função de falha e
função de saída a partir do dicionário de padrões. Depois dessa fase,
o autômato torna-se **estritamente read-only**: nenhuma thread o
modifica durante a busca, o que dispensa locks na fase paralela.

### 2. Fase de busca (paralela — pool de threads)

- **Particionamento de dados (chunking)**: o buffer de texto é dividido
  logicamente em chunks de tamanho fixo.
- **Overlap nas fronteiras**: chunks adjacentes têm uma margem de
  sobreposição de `max_pattern_length - 1` bytes (warm-up do DFA),
  garantindo que um padrão que atravessa fronteira é detectado por
  exatamente um worker, sem perdas nem duplicatas.
- **Execução concorrente**: cada worker percorre seu chunk usando o
  **mesmo autômato compartilhado por ponteiro**, sem sincronização.
- **Listas thread-local de matches**: cada worker grava em uma lista
  privada para evitar contenção. Logs/arquivos compartilhados são
  proibidos durante a varredura.
- **Merge sequencial**: após o `pthread_join`, o master concatena as
  listas locais em um resultado unificado.

Detalhamento e argumento formal de correção em
[`docs/architecture/parallelism.md`](docs/architecture/parallelism.md).

## Layout do repositório

```
include/             Cabeçalhos públicos (autômato, lista de matches,
                     API de searcher, harness de benchmark)
src/                 Núcleo (build + match + bench + registry + CLI)
src/searchers/       Uma implementação por arquivo — plug-in slot
tests/               Suite de correção (todo searcher concorda com
                     o baseline sequencial)
scripts/             Sweeps de benchmark e preparação de dados
data/                Datasets gerados (gitignored, recriados via scripts)
docs/                Documentação detalhada
  architecture/      Visão de sistema, autômato, paralelismo, harness
  searchers/         Uma página por searcher registrado
```

## Build

| Alvo            | O que faz                                                                |
|-----------------|---------------------------------------------------------------------------|
| `make`          | Release (`-O3 -march=native`)                                            |
| `make debug`    | Debug (`-O0 -g3`, asserts ligados)                                       |
| `make asan`     | AddressSanitizer + UBSan                                                 |
| `make tsan`     | ThreadSanitizer (valida ausência de data races na fase paralela)        |
| `make test`     | Suíte de correção: cada searcher vs. baseline em vários `nthreads`      |
| `make bench`    | Sweep sintético (`scripts/run_benchmarks.sh`)                            |

Requisitos: compilador C11, pthreads, POSIX 2008.

## CLI: `build/aclab`

O binário recebe um dicionário de padrões, um arquivo de entrada e
um conjunto de opções (searcher, nº de threads, warmup, iterações).
Arquivos grandes (≥ 64 MiB) são abertos via `mmap` com
`MAP_POPULATE` + `madvise(MADV_SEQUENTIAL)` para tirar I/O e page
faults da janela cronometrada.

Para listar os searchers registrados em uma build:

```text
./build/aclab --list
```

Para a referência completa de flags:

```text
./build/aclab --help
```

## Searchers atualmente disponíveis

| Nome                  | Descrição                                                                       |
|-----------------------|----------------------------------------------------------------------------------|
| `sequential`          | Baseline single-thread. Referência de correção e desempenho.                    |
| `pthread_chunked`     | Pthreads + chunks com overlap `max_pattern_len - 1`; matches thread-local.      |
| `pthread_chunked_v2`  | v1 com split warm-up/owned loops e cache-pad em `worker_t`.                     |
| `pthread_chunked_v3`  | v2 + afinidade ciente de topologia + chunks ponderados por `cpufreq` (híbridas).|
| `pthread_dynamic`     | Dispatch dinâmico de chunks via contador atômico.                               |
| `pthread_prefetch`    | v2 + `__builtin_prefetch(text + Δ)` para cobrir latência DRAM residual.         |
| `pthread_2d_sharded_chunked` | Idea 6 — 2-D `K × N` (sharding por prefixo × chunking de texto).         |

Documentação por searcher em [`docs/searchers/`](docs/searchers/).

## Correção da fase paralela

`make test` constrói um binário independente que:

1. Constrói o autômato a partir de uma bateria de dicionários.
2. Calcula o conjunto baseline de matches com `sequential`.
3. Roda **todos** os outros searchers registrados com contagens de
   thread `{1, 2, 3, 4, 7, 8}` sobre inputs deliberadamente desenhados
   para colocar matches em **todas** as fronteiras possíveis de chunk.
4. Ordena e compara as duas listas elemento a elemento. Qualquer
   divergência reprova o teste.

Para reforçar a verificação, combine com `make tsan`: o
ThreadSanitizer aponta qualquer escrita acidental no autômato (que é
puramente read-only) ou acesso cruzado às listas locais de matches.

## Adicionando uma nova variante paralela

1. Crie `src/searchers/<nome>.c`.
2. Implemente a função `search(...)` (assinatura em
   [`include/ac_searcher.h`](include/ac_searcher.h)).
3. Registre o searcher via `__attribute__((constructor))` chamando
   `ac_searcher_register(...)`.
4. `make` — o `Makefile` pega o novo arquivo por wildcard.
5. `make test` — sua implementação **precisa** concordar com
   `sequential` em todas as configurações de teste.
6. (Recomendado) `make tsan` para validar ausência de races.
7. `make bench` para medir e comparar throughput.

Nenhum outro arquivo precisa mudar — o registry descobre seu searcher
no momento em que o binário é carregado. Detalhes do contrato e
exemplos extensos em [`docs/searchers/README.md`](docs/searchers/README.md).

## Restrições inegociáveis do projeto

Essas restrições derivam tanto da literatura quanto do contrato
interno do laboratório, e qualquer searcher novo precisa respeitá-las:

- **Zero locks no caminho quente**. As threads de busca operam sobre o
  autômato compartilhado em modo estritamente read-only. Locks/atomics
  no loop de varredura são proibidos.
- **Thread-local storage para matches**. Matches são coletados em
  estruturas isoladas pré-alocadas por thread, seguidas de um merge
  sequencial pelo master após o `pthread_join`.
- **Overlap = `max_pattern_length - 1`**. Reduzir esse valor arrisca
  perda de matches em fronteira; aumentar gasta CPU à toa.

## Métricas e modo de avaliação

O harness foi pensado para gerar números **comparáveis com a
literatura** sobre matching para IDS:

- **Throughput** medido apenas sobre a fase paralela de busca
  (`bench_run`); construção do autômato, mapeamento de arquivos e
  merge final ficam fora do número reportado.
- **Speedup** levantado variando o nº de workers de 1 até o nº de
  cores lógicos disponíveis (`Tempo(1) / Tempo(N)`).
- **Footprint do autômato** reportado por execução (`num_states`,
  `KiB`), para que se possa relacionar performance com pressão de
  cache.

O detalhamento de cada métrica e como interpretá-las está em
[`docs/architecture/benchmark-harness.md`](docs/architecture/benchmark-harness.md).

## Datasets

Para resultados representativos use os datasets canônicos do TCC:

- **Padrões**: Snort 3 Community Rules → `data/patterns_snort.txt`.
- **Corpus**: Enron Email Dataset (~1.4 GiB) → `data/enron_corpus.txt`.

Origem, scripts e tamanhos detalhados em
[`docs/architecture/datasets.md`](docs/architecture/datasets.md) e em
[`data/README.md`](data/README.md). Reprodução com:

```text
./scripts/prepare_datasets.sh
./scripts/run_snort_enron_benchmarks.sh
```

## Documentação adicional

- [`CLAUDE.md`](CLAUDE.md) — guia rápido para agentes (Claude e afins).
- [`docs/architecture/overview.md`](docs/architecture/overview.md) —
  visão de sistema com diagramas.
- [`docs/architecture/automaton.md`](docs/architecture/automaton.md) —
  layout interno do autômato e construção.
- [`docs/architecture/parallelism.md`](docs/architecture/parallelism.md)
  — modelo de paralelismo e argumento de correção.
- [`docs/searchers/`](docs/searchers/) — documentação por searcher.
