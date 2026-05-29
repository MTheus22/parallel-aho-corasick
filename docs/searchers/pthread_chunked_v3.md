# Searcher `pthread_chunked_v3`

Variante focada em **CPUs híbridas** (Alder Lake-class P+E, AMD com SMT
ativo). Mantém o modelo de paralelismo de
[`pthread_chunked_v2`](pthread_chunked_v2.md) (chunks estáticos
contíguos + overlap `max_pattern_len - 1`, loops separados de warm-up e
emissão, padding por cache line) e troca duas decisões:

1. **Afinidade ciente de topologia.** A ordem em que workers são
   pinados nas CPUs deixa de ser `i % nproc`. Em vez disso, o searcher
   lê `/sys/devices/system/cpu/cpuN/topology/thread_siblings_list` e
   `/sys/devices/system/cpu/cpuN/cpufreq/cpuinfo_max_freq`, ordena as
   CPUs colocando primeiro líderes de núcleo físico (sortidos por
   frequência máxima desc.) e só então os irmãos SMT. Threads são
   pinadas nessa ordem.

2. **Chunks ponderados por frequência.** Em vez de cada worker receber
   `text_len / N` bytes, o searcher distribui `text_len` proporcional
   ao `cpuinfo_max_freq` da CPU em que cada worker foi pinado. Cores
   mais rápidos (P-cores a 4.4 GHz) recebem chunks maiores que cores
   mais lentos (E-cores a 3.3 GHz), de modo que todos os workers
   terminem em torno do mesmo instante.

Ambas as decisões são *best-effort*: na ausência de sysfs (containers
restritos, kernels sem cpufreq) o searcher degrada para o
comportamento de `pthread_chunked_v2` (chunks iguais, afinidade
identidade). Correção é preservada — `make test` valida v3 contra o
baseline `sequential`.

- Fonte: [`src/searchers/pthread_chunked_v3.c`](../../src/searchers/pthread_chunked_v3.c)
- Registro: `__attribute__((constructor)) v3_register()`
- Descrição: *Pthreads chunks; topology-aware affinity + freq-weighted chunks*

## Motivação

A afinidade ingênua `i % nproc` (e implicitamente o `pthread_chunked`
sob o scheduler do kernel) tem dois pontos cegos no i5-1235U:

### 1. Em baixo *thread count*, joga threads no mesmo P-core

A enumeração lógica das CPUs no i5-1235U é
`{CPU 0, CPU 1, CPU 2, CPU 3, CPU 4 … CPU 11}` onde:

| CPU lógica | Núcleo físico              | Classe | Max GHz |
|------------|----------------------------|--------|--------:|
| 0          | P-core 0 (HT slot A)       | P      | 4.4     |
| 1          | P-core 0 (HT slot B)       | P      | 4.4     |
| 2          | P-core 1 (HT slot A)       | P      | 4.4     |
| 3          | P-core 1 (HT slot B)       | P      | 4.4     |
| 4 a 11     | E-cores 0 a 7 (sem SMT)    | E      | 3.3     |

A política `cpu_id = i % nproc` faz com que com
**2 threads** a primeira vá para CPU 0 e a segunda para CPU 1 — **o
mesmo P-core físico**. As duas competem por L1, L2 e portas de
execução. O ganho cai de "2× sobre 1 thread" para algo entre 1.4× e
1.6× quando deveria ser próximo de 2×.

### 2. Em 12 threads, dá o mesmo chunk para cores assimétricos

Mesmo quando todas as CPUs estão em uso, a política de chunks iguais
ignora que os 4 logical-CPUs de P-core (0-3, todos a 4.4 GHz) processam
≈1.5× mais bytes/segundo que cada um dos 8 E-cores. Empíricamente
medido em isolação:

```text
taskset -c 0 sequential snort+enron → 160 MB/s   (P-core solo)
taskset -c 4 sequential snort+enron → 109 MB/s   (E-core solo)
                                      ratio 1.47
```

Com chunks iguais, P-cores terminam ~33% antes do que os E-cores e
ficam **ociosos** esperando os E-cores fecharem; o tempo total é gated
pelo E-core mais lento.

## Esquema

### Ordenação de CPUs

Cada CPU online é caracterizada por

```c
typedef struct {
    int       cpu_id;          /* índice lógico em /sys/.../cpuN */
    uint32_t  max_freq_khz;    /* via cpufreq, 0 se indisponível */
    int       sibling_leader;  /* menor cpu_id no grupo SMT */
} v3_cpu_info_t;
```

e o vetor é ordenado por:

1. **Líderes SMT antes dos irmãos** (`cpu_id == sibling_leader` vence).
2. **Frequência desc.** (P-cores antes de E-cores entre líderes).
3. **`cpu_id` crescente** como desempate determinístico.

No i5-1235U isso produz:

```text
posição: 0   1   2  3  4  5  6  7  8   9   10  11
cpu_id:  0   2   4  5  6  7  8  9  10  11  1   3
classe:  PL  PL  EL EL EL EL EL EL EL  EL  PS  PS
(PL = P-core leader, EL = E-core leader, PS = P-core sibling)
```

Workers 0..N-1 são pinados nessa ordem. Assim:

- Com 2 threads: CPUs **0 e 2** (P-cores físicos distintos — sem
  contenção HT).
- Com 4 threads: CPUs 0, 2, 4, 5 (2 P-cores físicos + 2 E-cores
  físicos — também sem SMT pareado).
- Com 10 threads: todos os 10 cores físicos exclusivos.
- Com 12 threads: os irmãos SMT (CPUs 1 e 3) entram por último.

### Pesos por frequência

Para cada worker `i`:

```c
weight[i] = cpufreq[ cpu_assigned_to[i] ]    /* em kHz */
```

Se o grupo SMT do worker `i` tiver mais de uma thread mapeada (ou seja,
o par HT está em uso), aplica-se um fator `V3_HT_FACTOR` ao peso —
defina-se em 1.0 (sem derate) para o i5-1235U porque a medição
empírica mostra throughput-por-thread quando HT-pareado
≈ `max_freq` × 1, igualando o ratio freq P/E. Em CPUs onde o
throughput por thread HT-pareada cai mais agressivamente, esse fator
pode ser reduzido (faixa típica 0.65–0.85).

Os bytes de cada worker são proporcionais ao seu peso normalizado:

```c
share[i] = text_len * weight[i] / sum(weight[0..N))
```

Arredondado para múltiplos de 64 bytes (uma cache line) para que
fronteiras de chunks nunca cortem uma linha de cache no meio.

### Loop interno

Idêntico ao de `pthread_chunked_v2`: warm-up state-only em
`[scan_start, core_start)`, emissão em `[core_start, core_end)`.
Mesma regra de ownership (matches emitidos por exatamente um worker,
overlap = `max_pattern_len - 1`).

## Invariantes preservados de v2

1. Zero locks no caminho quente.
2. Autômato compartilhado e estritamente read-only.
3. Listas thread-local de matches, merge sequencial após `pthread_join`.
4. Overlap = `max_pattern_len - 1` bytes; matches em região de warm-up
   pertencem ao worker anterior.
5. `worker_t` padded para uma cache line completa via
   `__attribute__((aligned(64)))` para evitar *false sharing*.

## Casos degenerados

- `nthreads == 1` → delega para `sequential`.
- `text_len <= 2 * overlap` → delega para `sequential`.
- `text_len < nthreads * 64` → delega para `sequential`.
- sysfs inacessível ou sem `cpufreq` → todos pesos = 1, comportamento
  idêntico ao `pthread_chunked_v2`.

## Validação per-thread (12 threads, snort + enron 1.36 GiB)

Output do CLI com `--per-thread` ilustra o efeito do peso por
frequência:

```text
[t00] 799 ms  142 MB  170 MB/s   <- worker 0 → P-core leader CPU 0
[t01] 799 ms  142 MB  170 MB/s   <- worker 1 → P-core leader CPU 2
[t02] 918 ms  107 MB  111 MB/s   <- worker 2 → E-core CPU 4
[t03] 896 ms  107 MB  114 MB/s   <- worker 3 → E-core CPU 5
[t04] 889 ms  107 MB  114 MB/s
[t05] 885 ms  107 MB  115 MB/s
[t06] 890 ms  107 MB  114 MB/s
[t07] 876 ms  107 MB  116 MB/s
[t08] 860 ms  107 MB  118 MB/s
[t09] 868 ms  107 MB  117 MB/s
[t10] 794 ms  142 MB  171 MB/s   <- worker 10 → P-core sibling CPU 1
[t11] 806 ms  142 MB  168 MB/s   <- worker 11 → P-core sibling CPU 3
wall: 988 ms (1372 MB/s)
```

P-cores e E-cores convergem para tempos próximos (~800 ms vs ~880 ms),
evitando o regime de "P-cores ociosos esperando E-cores" da afinidade
ingênua `i % nproc` (que daria ~1026 ms para os E-cores e ~700 ms para
os P-cores HT-pareados — gap de ~330 ms).

## Resultados em `snort + enron_corpus.txt` (1.36 GiB)

Sweep com `--warmup 1 --iters 3`, i5-1235U (2P+8E, 12 threads
lógicas). Variância inter-iter é alta (~3–6% por thermal throttling
do laptop); o `min(ms)` é o indicador mais confiável de "melhor que a
máquina consegue produzir nessa configuração".

| Searcher              | Threads | min (ms) | mean (ms) | MB/s (mean) |
|-----------------------|--------:|---------:|----------:|------------:|
| sequential            |   1     | 6.596    | 6.791     | 200         |
| pthread_chunked_v2    |   1     | 6.779    | 6.885     | 197         |
| pthread_chunked_v3    |   1     | 6.821    | 6.881     | 197         |
| pthread_chunked_v2    |   2     | 4.085    | 4.235     | 320         |
| **pthread_chunked_v3**|   2     | **4.050**| **4.098** | **331**     |
| pthread_dynamic       |   2     | 4.188    | 4.267     | 318         |
| pthread_chunked_v2    |  12     | 1.657    | 1.701     | 797         |
| **pthread_chunked_v3**|  12     | **1.631**| 1.705     | 795         |
| pthread_dynamic       |  12     | 1.672    | 1.712     | 792         |

### Em 2 threads

`pthread_chunked_v3` é o melhor (3% melhor que `pthread_chunked_v2`) —
é exatamente onde a *afinidade ciente de topologia* faz diferença: as
duas threads vão para P-cores físicos **diferentes** (CPU 0 e CPU 2)
em vez de compartilharem um par HT (CPU 0 e CPU 1). Sem contenção SMT
no L1/L2 e portas de execução.

A medição per-thread confirma o contraste com o pinning ingênuo
`i % nproc`:

```text
v3 (topology-aware)  t=2:  [t00]@CPU0 149 MB/s   [t01]@CPU2 149 MB/s   → 4.6 s
i % nproc (naive)    t=2:  [t00]@CPU0 131 MB/s   [t01]@CPU1 131 MB/s   → 5.2 s
                                                        ^^^^ HT pair
```

Por thread, ir de 131 → 149 MB/s representa um ganho de 14% só pela
escolha do CPU físico.

### Em 12 threads

Todas as 12 CPUs lógicas são usadas; a vantagem de pinning desaparece
(qualquer ordem cobre todas elas) e a discussão se desloca para o
*balanceamento de carga* entre P e E cores. O `min(ms)` do v3 é o
**melhor** do conjunto (1.631 ms), mas o `mean(ms)` empata com os
demais dentro da variância de thermal/freq scaling do laptop. Em
runs single-iter cold-thermals o v3 atinge **988 ms (1.372 MB/s) —
o melhor número absoluto** observado nesse setup.

A teoria prediz que com chunks freq-ponderados o `wall clock` ideal
nessa configuração é ~888 ms (com taxas medidas P-pair 170 MB/s,
E-core 115 MB/s):

```text
T_balanced = text_len / (4 * 170 + 8 * 115) MB/s
           = 1421 MB / 1600 MB/s ≈ 888 ms
```

contra afinidade ingênua `i % nproc` com chunks iguais:

```text
T_naive = 118 MB / 115 MB/s ≈ 1026 ms   (gated pelos E-cores)
```

Empíricamente o gap se realiza apenas no min(ms) sob janelas frias; o
mean sofre por thermal/freq scaling do laptop, o que efetivamente
nivela o sweep em regime sustentado.

## Trade-offs e limitações

- **Heurística fixa de HT compensation.** O fator `V3_HT_FACTOR` é
  empírico (1.0 para o i5-1235U). Em outras famílias de CPU
  (Sandy Bridge–Skylake com SMT mais agressivo, Zen 2+) o ratio
  per-thread paired-vs-solo pode justificar valores 0.65–0.85 — exige
  re-calibração.
- **Pesos por freq subestimam IPC.** P-core tem IPC maior que E-core
  além da diferença de freq. A medição mostra T_p_solo/T_e_solo ≈ 1.47
  (não 1.33 da freq). O peso bruto subestima ligeiramente os P-cores
  em baixo *thread count*. Uma melhoria futura seria detectar classes
  (P vs E) explicitamente e aplicar um boost de IPC.
- **Variância de thermal/freq scaling no laptop.** O i5-1235U opera em
  TDP variável (até 55 W em boost curto, depois throttle para ~15 W).
  Runs sustentados (>3 s contínuos) divergem dos primeiros runs.
  Resultados em CPU desktop com TDP fixo serão menos voláteis.
- **Apenas Linux.** `pthread_setaffinity_np`, sysfs em
  `/sys/devices/system/cpu` e o esquema `thread_siblings_list` são
  Linux-específicos. Em outros POSIX o searcher cai para os fallbacks.

## Tunables

| Macro                       | Default | Significado                                                                                   |
|-----------------------------|--------:|-----------------------------------------------------------------------------------------------|
| `V3_CACHE_LINE`             | `64`    | Tamanho de cache line (alinhamento de `worker_t` e arredondamento de chunks).                 |
| `V3_MAX_CPUS`               | `256`   | Capacidade dos vetores estáticos de topologia (`cpu_order`, `weights`).                      |
| `V3_HT_FACTOR_NUM/DEN`      | `100/100` | Fator multiplicativo de peso para threads em par HT. 100/100 = sem derate.                  |

## Correção

`make test` passa em todas as contagens `{1, 2, 3, 4, 7, 8}` da suíte
padrão (`classic`, `overlap`, `boundary`, `dense_a`, `lorem_5k`).
`make tsan` (com `setarch -R` para contornar incompatibilidade de
ASLR em kernels recentes) também passa — nenhum data race entre
workers.

## Leitura relacionada

- [`pthread_chunked_v2.md`](pthread_chunked_v2.md) — base direta
  (loops separados + cache-pad).
- [`pthread_dynamic.md`](pthread_dynamic.md) — alternativa via
  contador atômico, que também ataca assimetria de cores mas a custo
  de coordenação no hot path.
- [`../architecture/parallelism.md`](../architecture/parallelism.md) —
  invariantes herdados (overlap, ownership, read-only automaton).
