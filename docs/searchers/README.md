# Searchers

Esta pasta documenta cada implementação do contrato `ac_searcher_t`
registrada no laboratório. Todo searcher recebe a mesma entrada
(automato read-only + texto + configuração) e produz a mesma saída
(lista de matches), permitindo comparação 1:1 contra o baseline
sequencial.

## Contrato resumido

```c
int (*search)(const ac_automaton_t *aut,
              const char *text, size_t text_len,
              const ac_searcher_config_t *cfg,
              ac_match_list_t *out_matches,
              ac_thread_metric_t **out_thread_metrics,
              size_t *out_num_thread_metrics);
```

- O autômato (`aut`) é **estritamente read-only** após a construção;
  qualquer número de threads pode lê-lo sem sincronização.
- A lista `out_matches` recebe todos os matches encontrados (qualquer
  ordem; o harness compara após `ac_match_list_sort`).
- Métricas por thread são **opcionais** — searchers sequenciais devem
  retornar `NULL/0`.

## Searchers documentados

| Documento                                            | Searcher                |
|------------------------------------------------------|-------------------------|
| [`sequential.md`](sequential.md)                     | `sequential`            |
| [`pthread_chunked.md`](pthread_chunked.md)           | `pthread_chunked`       |
| [`pthread_chunked_v2.md`](pthread_chunked_v2.md)     | `pthread_chunked_v2`    |
| [`pthread_chunked_v3.md`](pthread_chunked_v3.md)     | `pthread_chunked_v3`    |
| [`pthread_dynamic.md`](pthread_dynamic.md)           | `pthread_dynamic`       |
| [`pthread_block_cyclic.md`](pthread_block_cyclic.md) | `pthread_block_cyclic`  |
| [`pthread_affinity.md`](pthread_affinity.md)         | `pthread_affinity`      |
| [`pthread_prefetch.md`](pthread_prefetch.md)         | `pthread_prefetch`      |
| [`sequential_flat.md`](sequential_flat.md)           | `sequential_flat`       |
| [`sequential_delta2.md`](sequential_delta2.md)       | `sequential_delta2`     |
| [`pthread_chunked_flat.md`](pthread_chunked_flat.md) | `pthread_chunked_flat`  |
| [`pattern_sharded.md`](pattern_sharded.md)           | `pattern_sharded` / `_lpt` / `_prefix` |

## Comparação rápida

| Searcher                | Paralelismo | Locks no hot path | Distinção principal                                     | Bom para                                 |
|-------------------------|-------------|-------------------|---------------------------------------------------------|-------------------------------------------|
| `sequential`            | nenhum      | —                 | Single-thread, referência de correção                   | Baseline, validação                       |
| `pthread_chunked`       | data-parallel | nenhum          | Chunks estáticos contíguos + overlap `L-1`              | Primeiro paralelo, robusto                |
| `pthread_chunked_v2`    | data-parallel | nenhum          | Loops separados warm-up/owned + cache-pad worker_t      | Hot path mais limpo, ganho de 3-4%        |
| `pthread_chunked_v3`    | data-parallel | nenhum          | v2 + afinidade topológica + chunks freq-ponderados      | CPUs híbridas P+E-core; melhor em t=2     |
| `pthread_dynamic`       | data-parallel | nenhum no byte   | Fila de tarefas via `atomic_fetch_add` (4N chunks)      | CPUs heterogêneas (P+E cores)             |
| `pthread_block_cyclic`  | data-parallel | nenhum          | Blocos cíclicos round-robin (1 MiB)                     | Distribuir ruído sem custo de átomo       |
| `pthread_affinity`      | data-parallel | nenhum          | Mesmo do v2 + `pthread_setaffinity_np`                  | Reduz migrações, melhora `mean(ms)`        |
| `pthread_prefetch`      | data-parallel | nenhum          | `__builtin_prefetch(text + Δ)` no hot loop              | 1-2 threads, regime bandwidth-disponível |

## Resultados (Snort + Enron, 1,36 GiB, i5-1235U)

`min(ms)` no melhor dos 5 runs (`--warmup 1 --iters 5`). Coluna
"speedup" usa `mean(ms)` contra `sequential mean`.

| Searcher             | t=1 (min ms / MB/s mean) | t=2 (min ms / MB/s mean) | t=12 (min ms / MB/s mean) | Speedup t=12 |
|----------------------|---------------------------|----------------------------|------------------------------|-------------:|
| `sequential`         | 6.075 / 195,9             | —                          | —                            | 1,00×        |
| `pthread_chunked`    | 7.265 / 183,6             | 4.573 / 288,3              | 1.841 / 708,2                | 3,62×        |
| `pthread_chunked_v2` | 7.306 / 183,9             | 4.507 / 295,4              | 1.766 / 732,4                | 3,74×        |
| `pthread_chunked_v3` | 6,821 / 197,0             | **4,050** / 331,0          | **1,631** / 795,0            | 3,98×¹       |
| `pthread_dynamic`    | 7.275 / 176,9             | 4.635 / 283,5              | **1.680** / 738,7            | 3,77×        |
| `pthread_block_cyclic`| 7.288 / 183,4            | 4.597 / 282,6              | 1.692 / 736,1                | 3,76×        |
| `pthread_affinity`   | 7.155 / 177,8             | **4.358** / 300,9          | **1.646** / 746,5            | 3,81×        |
| `pthread_prefetch`   | **6.867** / 193,8         | 4.314 / 294,0              | 1.821 / 714,7                | 3,65×        |

¹ `pthread_chunked_v3` medido em sweep separado `--warmup 1 --iters 3`; demais searchers com `--iters 5`. Speedup calculado via `mean(sequential)/mean(t=12)` do mesmo sweep (seq mean 6,791 s, v3 mean 1,705 s).

Detalhamento por searcher e análise nos respectivos docs.
