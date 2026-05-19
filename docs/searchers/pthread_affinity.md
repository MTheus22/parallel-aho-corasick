# Searcher `pthread_affinity`

Variante do `pthread_chunked` (com loops separados de warm-up/owned)
que adiciona **pinning explícito** de cada worker a uma CPU lógica via
`pthread_setaffinity_np`.

- Fonte: [`src/searchers/pthread_affinity.c`](../../src/searchers/pthread_affinity.c)
- Registro: `__attribute__((constructor)) aff_register()`
- Descrição: *Pthreads chunks + pthread_setaffinity_np pinning*

## Motivação

Mesmo quando o `pthread_chunked` está executando, o scheduler do
kernel pode migrar workers entre CPUs lógicas por várias razões:

- Equalizar carga térmica entre P-cores e E-cores.
- Liberar uma CPU para um processo de maior prioridade.
- Reagir a CPU isolation (cgroups).

Cada migração custa o **L1 / L2 que o worker já populou** — em um
loop varrendo ~120 MiB/s de texto, isso é um pedaço material do
working set. Pinning estático elimina o problema na fonte: thread `i`
fica em `cpu_id = i % nproc`. Nenhuma migração possível.

## Política

```c
static void try_pin(int cpu_id) {
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu_id, &set);
    (void)pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
}
```

- **Best-effort**: a chamada pode falhar (cgroups proibindo, taskset
  externo já limitando o conjunto). O searcher ignora o erro — o
  worker continua executando "soltamente" e a correção é preservada.
- **Mapeamento naïve**: `i % nproc`. Em CPUs heterogêneas como o
  i5-1235U (2 P-cores SMT2 + 8 E-cores → CPUs 0,1 são P0-HT,
  2,3 são P1-HT, 4..11 são E-cores), os primeiros 4 workers caem em
  P-cores; do worker 5 em diante caem em E-cores. Não é uma política
  inteligente, mas é determinística — útil para benchmark.

## Mudanças em relação ao `pthread_chunked`

| Aspecto                            | `pthread_chunked`           | `pthread_affinity`                    |
|------------------------------------|-----------------------------|----------------------------------------|
| Pinning                            | Não (scheduler livre)        | Sim (`pthread_setaffinity_np`)         |
| Loop interno                       | Único, com `if (i < core_start)` | Dois loops (igual ao `v2`)        |
| Particionamento                    | Estático contíguo            | Estático contíguo (idêntico)            |
| Política de overlap                | `L - 1`                      | `L - 1`                                |

A presença dos loops separados é deliberada: queremos isolar o efeito
do pinning. Para uma comparação 1-a-1 com o `pthread_chunked`, o
delta é "pinning + split loop". Para comparação "só pinning",
contraste com o `pthread_chunked_v2`.

## Quando pinning ajuda

- **Workloads de longa duração** (segundos a minutos): a chance de
  migração acumula com o tempo.
- **Sistemas com muito ruído** (containers compartilhados, batch jobs
  concorrentes): pinning quase sempre reduz `max(ms)`.
- **CPUs com cache hierarchies fortemente locais** (L2 por core):
  migração custa um cache miss em cascata.

## Quando NÃO ajuda (ou regride)

- **CPUs heterogêneas mal mapeadas**: pinning worker 5 em E-core fixo
  significa que esse worker fica permanentemente lento. Sem pinning,
  o kernel pode pelo menos rebalancear. Para esses casos o
  `pthread_dynamic` é uma resposta melhor.
- **Workloads curtos**: o overhead da chamada do sistema na criação
  da thread vira proporcionalmente significativo.
- **Quando o usuário já fixa via `taskset`**: pinning duplo não
  prejudica, mas também não dá nenhum benefício marginal.

## Resultados em `snort + enron_corpus.txt` (1,36 GiB)

| Searcher              | Threads | min (ms) | mean (ms) | MB/s (mean) |
|-----------------------|--------:|---------:|----------:|------------:|
| pthread_chunked       |   1     | 7.265    | 7.381     | 183,6       |
| pthread_affinity      |   1     | 7.155    | 7.621     | 177,8       |
| pthread_chunked       |   2     | 4.573    | 4.701     | 288,3       |
| **pthread_affinity**  |   2     | **4.358**| 4.505     | 300,9       |
| pthread_chunked       |  12     | 1.841    | 1.914     | 708,2       |
| **pthread_affinity**  |  12     | **1.646**| 1.816     | 746,5       |

Em 12 threads o `min(ms)` melhora ~11% e o `mean(ms)` ~5%. É o melhor
resultado entre as variantes testadas neste sweep, consistente com a
hipótese de que reduzir migrações ajuda quando o working set por
worker excede o L2.

Em 2 threads o ganho também aparece (~5% no `min`); em 1 thread o
searcher cai para `sequential` (fallback degenerado).

## Considerações

- O CLI **não expõe controle do pinning** atualmente — o mapa é
  fixo em `i % nproc`. Para experimentar políticas (só P-cores, só
  E-cores, intercalado) use `taskset` por fora, ou edite
  `pthread_affinity.c`.
- A chamada `pthread_setaffinity_np` é específica de Linux. Em outros
  POSIX o searcher compila mas o pinning é no-op.

## Correção

Validado por `make test` em todas as contagens `{1, 2, 3, 4, 7, 8}`.
Resultado da última execução: **All correctness tests PASSED.**

## Leitura relacionada

- [`pthread_chunked.md`](pthread_chunked.md) — baseline sem pinning.
- [`pthread_chunked_v2.md`](pthread_chunked_v2.md) — mesma estrutura
  de loop, sem pinning. Compare para isolar o efeito do
  `pthread_setaffinity_np`.
