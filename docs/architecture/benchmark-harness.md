# Harness de benchmark

Documenta como o laboratório mede tempo, throughput e métricas por
thread, e quais cuidados existem para que os números reportados sejam
**comparáveis com a literatura** e **reproduzíveis** entre execuções.

Fontes: [`include/benchmark.h`](../../include/benchmark.h),
[`src/benchmark.c`](../../src/benchmark.c),
[`src/main.c`](../../src/main.c).

## Relógio

Sempre `CLOCK_MONOTONIC`, em nanossegundos:

```c
uint64_t bench_now_ns(void);
```

Razão: imune a passos de NTP, ajustes de fuso e jumps de wallclock. É
o relógio canônico para medir intervalos curtos e repetíveis.

`bench_marker_start/end` é só um par `t0/t1` com um label legível,
útil para medir trechos curtos (por exemplo, a fase de construção do
autômato).

## `bench_run`: o coração das medidas

```c
int bench_run(const ac_searcher_t *s,
              const ac_automaton_t *aut,
              const char *text, size_t text_len,
              const ac_searcher_config_t *cfg,
              int warmup, int iters,
              bench_result_t *out_result,
              ac_match_list_t *out_last_matches);
```

Roteiro interno:

```mermaid
flowchart TD
    A[Inicializa match list reutilizável] --> B[Warm-up: N iterações<br/>matches são descartados]
    B --> C[Loop de medição: M iterações]
    C --> D[reset matches.count = 0<br/>(mantém capacity)]
    D --> E[t0 = CLOCK_MONOTONIC]
    E --> F[s-&gt;search(...)]
    F --> G[t1 = CLOCK_MONOTONIC]
    G --> H[acumula min/mean/max]
    H --> I{mais iterações?}
    I -- sim --> D
    I -- não --> J[grava bench_result_t<br/>devolve última lista]
```

Por que `matches.count = 0` em vez de `ac_match_list_free` entre
iterações? Porque o **capacity** é preservado, então a partir da
segunda iteração os `push` ficam puramente *amortized O(1)* — o que
captura o estado estacionário esperado em produção, sem o ruído de
realocações iniciais.

## Política de warm-up

A flag `--warmup` (default `1`) executa N iterações e **descarta** os
tempos. Isso resolve três problemas distintos:

1. **Page faults**: mesmo com `MAP_POPULATE`, as primeiras passagens
   pelo buffer aquecem entradas de TLB e do prefetcher. Sem warm-up,
   a primeira iteração consistentemente parece mais lenta.
2. **Frequency governor**: em CPUs com escalonamento dinâmico de
   frequência, a primeira execução pode pegar a CPU em modo
   "powersave". Uma iteração de warmup costuma ser suficiente para
   trazer todos os cores para a frequência alvo.
3. **Caches do autômato**: o `goto_tbl` pode ter dezenas de KiB; o
   warm-up traz blocos quentes para L1/L2 antes de cronometrar.

Para benchmarks de TCC, recomendamos `--warmup 1 --iters 3..5`.

## Estrutura do resultado

```c
typedef struct {
    const char *searcher_name;
    int         num_threads;
    size_t      bytes_scanned;
    size_t      num_matches;
    double      build_seconds;          /* não usado por bench_run */
    double      search_seconds_min;
    double      search_seconds_mean;
    double      search_seconds_max;
    double      throughput_mbps_mean;   /* MB = 1 << 20 */
    int         iterations;
} bench_result_t;
```

- **MB/s** é `MiB/s` (`1 << 20`) calculado a partir do **mean**, não
  do **min**. Útil para gráficos de speedup; relate sempre o valor
  reportado (não recompute a partir de min sem dizer).
- Para reportar **Gbps**, multiplique `bytes_scanned * 8` e divida
  pelo `search_seconds_mean * 1e9`. Documente a convenção.

## Métricas por thread (`--per-thread`)

Quando o usuário passa `--per-thread`, `main.c` executa **uma
chamada adicional** ao searcher passando `out_thread_metrics != NULL`.
Cada entrada é:

```c
typedef struct {
    int    thread_id;
    double seconds;        /* só o tempo dentro de worker_main */
    size_t bytes_scanned;  /* core_end - scan_start (inclui warm-up) */
    size_t matches_found;
} ac_thread_metric_t;
```

Diagnóstico típico:

- **Speedup ruim mas todos os workers com tempos próximos**:
  provavelmente memória ou cache estão saturados, não há trabalho
  para ganhar.
- **Worker 0 muito mais lento que os demais**: alguma página ainda não
  foi populada quando ele começa — geralmente arrumado aumentando
  `--warmup`.
- **Distribuição muito desigual**: um worker pegou uma região com
  matches densos. Considere chunks menores ou um esquema de
  balanceamento dinâmico.

## Anatomia da saída do CLI

```
# Aho-Corasick laboratory
# patterns:   12 (max len 11, min len 2)
# automaton:  127 states, 130.62 KiB
# input:      67108864 bytes (64.00 MiB) via mmap
# build time: 0.812 ms
# warmup=1 iters=5 threads=8

searcher                thr        bytes      min(ms)     mean(ms)      max(ms)         MB/s   matches
pthread_chunked            8     67108864       16.483       17.012       17.984      3760.42    1245678
    [t00] 17.110 ms  8388608 bytes  155734 matches  466.65 MB/s
    [t01] 16.992 ms  8388618 bytes  155720 matches  470.05 MB/s
    ...
```

- Cabeçalho `#` é informacional e estável (parseável por scripts).
- Linha de resultado é tab-friendly (alinhada por padding fixo).
- Linhas `[tNN]` aparecem só se `--per-thread` foi pedido.

## Boas práticas para gerar números para a dissertação

1. **Fix the CPU governor**: ideal `performance`. Se for usar
   `schedutil`, documente.
2. **Pin threads** (taskset / cpuset / `pthread_setaffinity_np`):
   evita migração entre cores enquanto a janela paralela acontece.
   No estado atual do laboratório, o pinning fica a cargo do shell
   (`taskset -c 0-7 ./build/aclab ...`).
3. **mmap + MAP_POPULATE**: já feito por `main.c` para arquivos
   ≥ 64 MiB; verifique que o input cai nessa categoria para evitar
   page faults dentro da medida.
4. **Repita 5–10 iterações com warmup ≥ 1** e reporte min/mean/max.
5. **Documente o autômato medido**: `num_states` e
   `ac_automaton_memory_bytes(aut)` — searches em DFAs que cabem em
   L2 se comportam **muito** diferente de DFAs que extrapolam L3.

## Como o test harness usa o benchmark

`tests/test_correctness.c` **não** usa `bench_run`. Ele chama
`s->search` diretamente e compara contra `sequential`. Isto é
intencional: o objetivo do test harness é validar correção, então a
sobrecarga e o warmup do bench harness são irrelevantes.

## Leituras relacionadas

- [`overview.md`](overview.md) — fluxo do CLI end-to-end.
- [`parallelism.md`](parallelism.md) — o que é medido pela janela
  paralela e o que fica de fora.
- [`datasets.md`](datasets.md) — quais corpora usar para resultados
  publicáveis.
