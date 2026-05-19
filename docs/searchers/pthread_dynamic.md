# Searcher `pthread_dynamic`

Dispatch dinâmico de chunks via contador atômico. Mesma matemática de
overlap do [`pthread_chunked`](pthread_chunked.md), mas a atribuição
de chunks a threads é resolvida em tempo de execução — workers mais
rápidos consomem mais tarefas.

- Fonte: [`src/searchers/pthread_dynamic.c`](../../src/searchers/pthread_dynamic.c)
- Registro: `__attribute__((constructor)) dyn_register()`
- Descrição: *Pthreads dynamic dispatch (atomic counter, 4N tasks)*

## Motivação

`pthread_chunked` particiona o texto em `N` slices estáticos de tamanho
igual. Esse esquema implicitamente assume que cada worker faz o mesmo
trabalho por byte. Na prática, três fontes de assimetria quebram a
hipótese:

1. **CPUs heterogêneas** (Alder Lake/Raptor Lake, ARM big.LITTLE):
   P-cores executam ~1,5–2× mais IPC que E-cores no mesmo loop.
2. **Ruído de scheduler**: preempção, interrupts e migração introduzem
   variância por worker.
3. **Page faults**: a primeira passada paga warm-up de TLB e cache
   distribuído de forma irregular pelo texto.

Em todos esses casos o tempo total é gated pelo worker mais lento.
Dispatch dinâmico converte o problema em "quem terminar uma tarefa
puxa a próxima", então a desigualdade aparece em **quantas tarefas**
cada worker processou, não no tempo total.

## Esquema

```text
text [0..L)  →  K tarefas, K = nthreads · K_PER_THREAD (default 4)
                ┌─────┬─────┬─────┬─────┬─────┬─────┬─────┬ ...
                │  0  │  1  │  2  │  3  │  4  │  5  │  6  │
                └─────┴─────┴─────┴─────┴─────┴─────┴─────┘
                  ^
                  next_task (atomic_size_t, fetch_add no worker)
```

Cada tarefa carrega:

```c
typedef struct {
    size_t core_start;   /* inclusivo */
    size_t core_end;     /* exclusivo */
    size_t scan_start;   /* core_start - overlap, ou 0 para a primeira */
} task_t;
```

O loop do worker é simples:

```c
for (;;) {
    size_t idx = atomic_fetch_add(&next_task, 1);
    if (idx >= num_tasks) break;
    /* warm-up [scan_start, core_start), depois owned [core_start, core_end) */
    ...
}
```

`atomic_fetch_add` é tocado **entre** tarefas, não por byte — uma
tarefa típica tem dezenas de MiB, então o custo do átomo é negligível
no agregado.

## Correção

Idêntica em forma à do `pthread_chunked`: cada tarefa tem `overlap =
max_pattern_len - 1` bytes de warm-up e só emite matches com `end_pos
∈ [core_start, core_end)`. Como a união dos `[core_start, core_end)`
de todas as tarefas cobre `[0, text_len)` exatamente uma vez sem
sobreposição em emissão, o multiset de matches é idêntico ao do
`sequential`.

A diferença é só a **ordem** em que os matches são produzidos — varia
entre runs (race condition de atribuição). `make test` lida com isso
chamando `ac_match_list_sort()` antes de comparar.

## Por que `K = 4 · nthreads`?

| K | Trade-off |
|---|-----------|
| `K = nthreads` | Equivalente ao chunking estático — perde a vantagem |
| `K = 2·nthreads` | Balanceia se algum worker for 2× mais lento, mas só ele se for o último |
| **`K = 4·nthreads`** | Cobre a maioria das assimetrias práticas; overhead aceitável de warm-up extra (4× mais warm-ups, 4×`(L-1)` bytes extra varridos) |
| `K = 16·nthreads` | Excessivo: warm-up começa a roubar mais tempo do que economiza em balanço |

O valor 4 vem da literatura de work-stealing (TBB, Cilk Plus) como
ponto de partida razoável. Não é tunado por workload neste laboratório.

## Limites e quando NÃO ajudar

- **Texto curto**: poucos chunks → fragmentação prejudicial. O searcher
  delega para `sequential` quando `text_len < num_threads · 64`, e
  reduz `num_tasks` automaticamente se `K > text_len / 64`.
- **Workloads uniformes em CPU homogênea**: o ganho colapsa porque
  todos os workers já terminam quase juntos. Aí o custo extra de
  warm-up (4× a região de aquecimento) pode até regredir.
- **Pressão de cache no goto_tbl**: dispatch dinâmico não muda o
  perfil de cache; se o `pthread_chunked` está cache-bound, este
  também está.

## Resultados em `snort + enron_corpus.txt` (1,36 GiB)

Medições com `--warmup 1 --iters 5`, máquina i5-1235U.

| Searcher          | Threads | min (ms) | mean (ms) | MB/s (mean) |
|-------------------|--------:|---------:|----------:|------------:|
| pthread_chunked   |   1     | 7.265    | 7.381     | 183,6       |
| pthread_dynamic   |   1     | 7.275    | 7.662     | 176,9       |
| pthread_chunked   |   2     | 4.573    | 4.701     | 288,3       |
| pthread_dynamic   |   2     | 4.635    | 4.780     | 283,5       |
| pthread_chunked   |  12     | 1.841    | 1.914     | 708,2       |
| **pthread_dynamic**|  12    | **1.680**| 1.835     | 738,7       |

Em 12 threads o `min(ms)` do `pthread_dynamic` é ~9% menor que o do
`pthread_chunked` — coerente com a hipótese de que dispatch dinâmico
reduz o tempo gated-by-slowest em CPUs heterogêneas (8 E-cores +
2 P-cores). No `mean(ms)`, porém, o ganho cai para ~4% — sugerindo
que a melhora aparece principalmente nos runs em que algum worker
tinha sido alocado a um E-core "infeliz", e o efeito é diluído na
média.

Em 1 e 2 threads o dispatch dinâmico não ajuda (e é levemente pior),
o que faz sentido: com poucos workers a chance de assimetria é
pequena e o overhead de manter a fila de tarefas é proporcionalmente
maior.

## Tunables

| Macro              | Default | Significado                                              |
|--------------------|--------:|-----------------------------------------------------------|
| `K_PER_THREAD`     | `4`     | Multiplicador para o número total de tarefas              |

Para experimentar, edite `K_PER_THREAD` em `pthread_dynamic.c` e
recompile. Valores razoáveis: 2, 4, 8.

## Correção

Validado por `make test` em todas as contagens `{1, 2, 3, 4, 7, 8}` da
suíte padrão. Resultado da última execução: **All correctness tests
PASSED.**

## Leitura relacionada

- [`pthread_chunked.md`](pthread_chunked.md) — versão estática de
  particionamento.
- [`pthread_block_cyclic.md`](pthread_block_cyclic.md) — alternativa
  estática para balancear sem contador atômico.
