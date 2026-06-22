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
| [`pthread_dynamic_flat.md`](pthread_dynamic_flat.md) | `pthread_dynamic_flat`  |
| [`pthread_prefetch.md`](pthread_prefetch.md)         | `pthread_prefetch`      |
| [`sequential_flat.md`](sequential_flat.md)           | `sequential_flat`       |
| [`pthread_chunked_flat.md`](pthread_chunked_flat.md) | `pthread_chunked_flat`  |
| [`pthread_chunked_v3_flat.md`](pthread_chunked_v3_flat.md) | `pthread_chunked_v3_flat` |
| [`pattern_sharded.md`](pattern_sharded.md)           | `pattern_sharded_prefix` |
| [`pthread_2d_sharded_chunked.md`](pthread_2d_sharded_chunked.md) | `pthread_2d_sharded_chunked` |

## Comparação rápida

| Searcher                | Paralelismo | Locks no hot path | Distinção principal                                     | Bom para                                 |
|-------------------------|-------------|-------------------|---------------------------------------------------------|-------------------------------------------|
| `sequential`            | nenhum      | —                 | Single-thread, referência de correção                   | Baseline, validação                       |
| `pthread_chunked`       | data-parallel | nenhum          | Chunks estáticos contíguos + overlap `L-1`              | Primeiro paralelo, robusto                |
| `pthread_chunked_v2`    | data-parallel | nenhum          | Loops separados warm-up/owned + cache-pad worker_t      | Hot path mais limpo, ganho de 3-4%        |
| `pthread_chunked_v3`    | data-parallel | nenhum          | v2 + afinidade topológica + chunks freq-ponderados      | CPUs híbridas P+E-core; melhor em t=2     |
| `pthread_dynamic`       | data-parallel | nenhum no byte   | Fila de tarefas via `atomic_fetch_add` (4N chunks)      | CPUs heterogêneas (P+E cores)             |
| `pthread_dynamic_flat`  | data-parallel | nenhum no byte   | bag of tasks (4N) + emissão flat (idea 5)               | Cores homogêneos (Zen 5/EPYC): dispatch dinâmico + flat |
| `pthread_prefetch`      | data-parallel | nenhum          | `__builtin_prefetch(text + Δ)` no hot loop              | 1-2 threads, regime bandwidth-disponível |
| `pthread_chunked_v3_flat` | data-parallel | nenhum        | v3 (topology + freq weights) + emissão flat (idea 5+7)   | Nicho: Snort + Enron (+2,6 %); regride fora |
| `pthread_2d_sharded_chunked` | data + dict-parallel | nenhum   | 2-D: K shards × N chunks = T workers (idea 6)         | Comparação 2-D; perde p/ chunked_flat em desktop |

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
| `pthread_prefetch`   | **6.867** / 193,8         | 4.314 / 294,0              | 1.821 / 714,7                | 3,65×        |

¹ `pthread_chunked_v3` medido em sweep separado `--warmup 1 --iters 3`; demais searchers com `--iters 5`. Speedup calculado via `mean(sequential)/mean(t=12)` do mesmo sweep (seq mean 6,791 s, v3 mean 1,705 s).

### Composições com flat — sweep multi-regime (2026-05-22)

Sweep `scripts/run_multiregime_flat_compositions.sh` (`--warmup 1 --iters 3`), 4 combinações patterns × corpus, mesmo hardware. **`pthread_chunked_flat` continua sendo o melhor searcher em 3 dos 4 regimes**; F1 (`v3_flat`) só ganha em Snort + Enron (+2,6 %).

| Regime | `chunked_flat` MB/s (T=12) | `v3_flat` Δ |
|---|---:|---:|
| **R1** Snort + SimpleWiki (low density) | **551,11** ⭐ | −0,7 % (empata) |
| **R2** Snort + Enron (DRAM-bound) | 510,37 | **+2,6 %** ⭐ |
| **R3** Snort-100 + SimpleWiki (cache-friendly) | **1.292,08** ⭐ | −26,3 % |
| **R4** ET-32 + SimpleWiki (autômato >> L3) | **165,73** ⭐ | −9,5 % |

`pthread_chunked_v3_flat` é útil apenas em regime DRAM-bound moderado (R2);
fora desse nicho, o overhead de sondagem de sysfs (`cpufreq`,
`thread_siblings_list`) supera o ganho. Doc detalhada em
[`pthread_chunked_v3_flat.md`](pthread_chunked_v3_flat.md).

> O primeiro sweep dedicado apenas ao regime R2
> (`scripts/run_flat_compositions_sweep.sh`, `--iters 5`) havia reportado +15 % para
> o `v3_flat`, mas era ruído térmico em uma janela fria de `chunked_flat`.
> O sweep multi-regime acima é o canônico para o TCC.

### F3 — Stack end-to-end sobre `enron_x8` (10,59 GiB)

O wall-clock end-to-end é **reconstruído do sweep canônico 2026-05-29** (não de um
sweep dedicado): para cada execução o banco já registra a construção (`build_ms`) e
um passe completo de busca sobre o corpus (`mean_ms`), então E2E = `build_ms + mean_ms`.
Isso é auditável via `sqlite3` e usa o **mesmo baseline** da análise de busca.

| Dicionário | Configuração                                          | Wall-clock E2E (ms) | Speedup E2E |
|------------|-------------------------------------------------------|--------------------:|------------:|
| Snort      | baseline (build seq `49` + sequential T=1 `45.125`)   | 45.174              | 1,00×       |
| Snort      | **build seq `54` ⊕ `v3_flat` T=12 `9.974`**           | **10.027**          | **4,50×** ⭐ |
| ET-32      | baseline (build seq `541` + sequential T=1 `86.457`)  | 86.998              | 1,00×       |
| ET-32      | **build seq `538` ⊕ `chunked_flat` T=12 `30.979`**    | **31.517**          | **2,76×** ⭐ |

**Lições:** (a) em ET-32 + corpus grande, `chunked_flat` é o default — `v3_flat`
regride agudamente sob carga, coerente com o resultado negativo de ET-32 no sweep
multi-regime; (b) build paralelo contribui < 1 % do wall-clock E2E (Snort `49`/`45.174`;
ET `541`/`86.998`), métrica operacional separada (recarga de regras); (c) como o build é
desprezível, o E2E acompanha a busca (4,50× vs 4,52×; 2,76× vs 2,79×) — confirma, não
refina. Query auditável em [`apendicei.tex`]; doc canônica: [`results.md`](../../../tcc_notes/sections/notes/results.md) e [`conclusion.md`](../../../tcc_notes/sections/notes/conclusion.md).

---

## Searchers removidos do escopo

Os seguintes searchers existiam no laboratório e foram removidos por não contribuírem para a narrativa do TCC (2026-05-28):

| Searcher removido       | Motivo |
|-------------------------|--------|
| `pthread_block_cyclic`  | Empate com `pthread_chunked_v2` em todos os regimes; sem veredicto próprio para o TCC. |
| `sequential_delta2`     | Só vence para dicionários < ~50 estados (fora do regime alvo Snort/ET). Em Snort full faz fallback automático para `sequential`. |
| `pattern_sharded`       | Round-robin regride ~30% em todos os regimes; `pattern_sharded_prefix` cobre o mesmo eixo com resultado real. |
| `pattern_sharded_lpt`   | LPT empiricamente equivale ao round-robin no hardware testado; descartável. |
| `pthread_affinity`      | Pinning ingênuo `i % nproc`; subsumido por `pthread_chunked_v3` (afinidade topológica é estritamente superior). Sem veredicto próprio — mesmo critério do `pthread_block_cyclic`. |
| `pthread_prefetch_flat` | Negativo-sobre-negativo: regride em 4/4 regimes e a lição "SW prefetch de texto não ajuda no x86 moderno" já é entregue pelo `pthread_prefetch` (não-flat). Redundante. |

O código, docs e números dessas variantes estão preservados no histórico git se necessários futuramente.

Detalhamento por searcher e análise nos respectivos docs.
