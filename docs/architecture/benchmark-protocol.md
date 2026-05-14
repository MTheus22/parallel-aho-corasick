# Protocolo experimental (TCC)

Conjunto mínimo de instruções para gerar números defensáveis para a
dissertação: como variar parâmetros, o que reportar, o que isolar.

> Pré-requisitos de infraestrutura (dicionário Snort, corpus Enron,
> mmap com overlap, ausência de locks, thread-local matches) já estão
> implementados — ver
> [`overview.md`](overview.md), [`parallelism.md`](parallelism.md) e
> [`datasets.md`](datasets.md).

## 1. Dicionários a comparar

Para expor sensibilidade ao tamanho do autômato (e portanto à pressão
de cache), gere variantes do dicionário com tamanhos crescentes:

| Variante                 | Tamanho alvo       | Como obter                                                                 |
|--------------------------|--------------------|----------------------------------------------------------------------------|
| `patterns_snort_100.txt` | ~100 padrões       | `head -100 data/patterns_snort.txt`                                       |
| `patterns_snort_1k.txt`  | ~1.000 padrões     | `head -1000 data/patterns_snort.txt`                                      |
| `patterns_snort_10k.txt` | ~10.000 padrões    | `head -10000 data/patterns_snort.txt`                                     |
| `patterns_snort.txt`     | todo o ruleset     | output direto de `scripts/extract_snort_patterns.py`                      |

Reporte para cada variante: `num_patterns`, `max_len`, `min_len`,
`num_states`, `KiB` (todos impressos pelo CLI no header `#`).

## 2. Corpus

- **Canônico**: `data/enron_corpus.txt` (~1.4 GiB). Domina-CPU para
  qualquer N até nº de cores físicos.
- **Rápido (iteração)**: `data/simplewiki.txt` (~200 MiB). Use para
  desenvolver e validar; **não** reporte como número final.
- **Sanity sintético**: `scripts/run_benchmarks.sh` com `BYTES=...`.

Em todos os casos, garanta que o arquivo seja `mmap`'d (≥ 64 MiB —
limiar atual em `main.c`).

## 3. Métricas obrigatórias

| Métrica                   | Como calcular                                                              |
|---------------------------|----------------------------------------------------------------------------|
| Throughput (**MiB/s**)    | direto da coluna `MB/s` do CLI (já usa `1 << 20`)                          |
| Throughput (**Gbps**)     | `(bytes_scanned * 8) / (search_seconds_mean * 1e9)`                       |
| Speedup `S(N)`            | `search_seconds_mean(1) / search_seconds_mean(N)`                          |
| Eficiência `E(N)`         | `S(N) / N`                                                                 |
| Footprint do autômato     | `ac_automaton_memory_bytes` — impresso no header `#`                       |

**Reporte sempre o tempo apenas da fase de busca** (`bench_run`). Não
inclua tempo de construção, mmap, ou merge. Isso já é o
comportamento padrão do harness.

## 4. Sweeps recomendados

### Speedup vs. nº de threads (corpus fixo, dicionário fixo)

```text
for T in 1 2 4 8 16 $(nproc); do
    ./build/aclab \
        --patterns data/patterns_snort.txt \
        --input    data/enron_corpus.txt \
        --searcher pthread_chunked \
        --threads  $T \
        --warmup 1 --iters 5
done
```

O script `scripts/run_snort_enron_benchmarks.sh` faz isso. Plote
`S(T) = T_seq / T_par(T)` versus `T`.

### Footprint vs. throughput (varia dicionário)

```text
for D in patterns_snort_100.txt patterns_snort_1k.txt \
         patterns_snort_10k.txt patterns_snort.txt; do
    ./build/aclab \
        --patterns data/$D \
        --input    data/enron_corpus.txt \
        --searcher pthread_chunked \
        --threads  $(nproc) \
        --warmup 1 --iters 5
done
```

Plote `MiB/s` versus `KiB` do autômato. Espera-se queda quando o
autômato sai de L2 / L3.

### Métricas por thread (diagnóstico)

```text
./build/aclab ... --searcher pthread_chunked --threads $(nproc) \
                  --per-thread --warmup 1 --iters 1
```

Use para verificar balanceamento de tempo e bytes entre workers.

## 5. Cuidados de ambiente

- **CPU governor**: `performance` (ou documentar o usado).
- **Pinagem**: prefira rodar com `taskset -c 0-(N-1)` durante o sweep
  para evitar migração entre cores e impacto do escalonador.
- **Vizinhança ruidosa**: feche browsers, IDEs e qualquer carga
  paralela durante as medições. Hyper-Threading: documente se está
  ligado e como você pinou (irmãos lógicos vs. cores físicos
  distintos).
- **Repetições**: `--iters 5` no mínimo. Use `min` para o melhor caso
  publicável e `mean` para o número "típico". Reporte os dois.

## 6. Checklist por execução

- [ ] Dicionário identificado (`num_patterns`, `max_len`).
- [ ] Corpus identificado (`bytes_scanned` impresso).
- [ ] Searcher e nº de threads explícitos.
- [ ] `--warmup ≥ 1` e `--iters ≥ 3`.
- [ ] CPU governor e pinagem documentados.
- [ ] Comparação contra `sequential` no mesmo ambiente.
- [ ] `make tsan` recente sem warnings na variante paralela.

## 7. Itens fora do escopo deste laboratório

- **Latência de pacote** ou métricas de IDS de rede em si
  (`pps`, `false-positives`, etc.) — fora do recorte.
- **Engine de regex**: padrões `pcre:` são intencionalmente
  ignorados pelo extractor.
- **Persistência do autômato em disco**: o autômato é reconstruído a
  cada execução. Se o tempo de build entrar no caminho crítico em
  algum experimento, considere caching binário em uma evolução
  futura.
