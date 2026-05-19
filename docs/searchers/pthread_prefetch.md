# Searcher `pthread_prefetch`

Variante do `pthread_chunked` (com loops separados) que adiciona
`__builtin_prefetch` para hidratar o texto duas linhas de cache à
frente do ponteiro de varredura.

- Fonte: [`src/searchers/pthread_prefetch.c`](../../src/searchers/pthread_prefetch.c)
- Registro: `__attribute__((constructor)) pf_register()`
- Descrição: *Pthreads chunks + __builtin_prefetch(text + Δ)*

## Motivação

O hot path do Aho–Corasick tem dois streams de memória:

| Stream                         | Padrão                          | Acessibilidade ao prefetcher de HW |
|--------------------------------|----------------------------------|------------------------------------|
| `text[i]`                      | Stride +1, sequencial            | Excelente — HW prefetcher pega    |
| `goto_tbl[state·256 + c]`      | Stride aleatório (depende de state, c) | Ruim — random walk na tabela  |

A `goto_tbl` na configuração Snort tem ~56 MiB (55.479 estados × 256
entradas × 4 B). Cabe em L3 mas excede L2. Cada miss em L3 custa
~100 ns. O HW prefetcher do x86 só ajuda quando reconhece padrão de
stride — algo que a `goto_tbl` raramente exibe.

Esse searcher **não** tenta prefetchar a `goto_tbl` (precisaria
prever o próximo `state`, o que destrói a determinação do DFA).
Apenas adiciona um prefetch explícito no stream `text[i]`. A hipótese
testada é: **o stride +1 do hardware tem alguma latência de descoberta
que o prefetch explícito elimina**, e o ganho compensa o instruction
overhead.

## Esquema

```c
#define PF_DISTANCE 128  /* 2 cache lines à frente */

for (size_t i = core_start; i < pf_end; i++) {
    __builtin_prefetch(text + i + PF_DISTANCE, 0, 0);
    /* ... mesmo corpo do pthread_chunked_v2 ... */
}
/* tail loop sem prefetch para não ler além de core_end */
```

- `PF_DISTANCE = 128` (= 2 linhas de 64 B). Em x86_64, um único
  prefetch em vôo costuma ser suficiente; aumentar para 256/512 não
  ajudou em testes preliminares.
- `__builtin_prefetch(addr, 0, 0)`: leitura, sem locality (T0). NTA
  para evitar poluir caches superiores não traz benefício no streaming
  load.
- O laço é dividido em "prefetched" + "tail" para garantir que o
  endereço prefetchado nunca passa de `core_end - 1`. Em x86 ler além
  do buffer é geralmente seguro (mmap arredonda para a página), mas
  preferimos não depender disso.

## Custo do prefetch

Cada `__builtin_prefetch` é uma instrução adicional por byte
processado. Em arquiteturas com decode bandwidth abundante (Sunny Cove,
Golden Cove, Zen 3+), o custo é absorvido. Em E-cores ou cores mais
estreitos, pode estreitar o pipeline.

## Resultados em `snort + enron_corpus.txt` (1,36 GiB)

| Searcher           | Threads | min (ms) | mean (ms) | MB/s (mean) |
|--------------------|--------:|---------:|----------:|------------:|
| pthread_chunked    |   1     | 7.265    | 7.381     | 183,6       |
| **pthread_prefetch**|  1     | **6.867**| 6.994     | 193,8       |
| pthread_chunked    |   2     | 4.573    | 4.701     | 288,3       |
| **pthread_prefetch**|  2     | **4.314**| 4.610     | 294,0       |
| pthread_chunked    |  12     | 1.841    | 1.914     | 708,2       |
| pthread_prefetch   |  12     | 1.821    | 1.896     | 714,7       |

Em 1 e 2 threads o prefetch dá um ganho modesto (~5–6% no `min(ms)`).
Em 12 threads o ganho some — quando todos os cores estão concorrendo
por DRAM, o prefetch explícito não traz dados antes do tempo, só
adianta a chegada do *back-pressure* da memória.

Interpretação: o HW prefetcher do i5-1235U já está fazendo um bom
trabalho na configuração com 12 cores ativos. O prefetch explícito
ajuda no regime *bandwidth-disponível* (poucos cores), não no regime
*bandwidth-saturado*.

## Quando ajudar

- CPUs com prefetcher fraco (alguns ARM Cortex-A55/A510, microcontroller
  classes).
- Workloads em que poucos cores ativos disputam a banda — exatamente
  o que vemos em 1–2 threads aqui.
- Texto **frio** (não-mmap'd, não MAP_POPULATE): o prefetch força a
  faulting page antes do scan.

## Quando NÃO ajudar

- Texto **quente** já residente em cache: o prefetch é trabalho
  redundante.
- Cores acomodando carga máxima de DRAM: nada a antecipar.

## Tunables

| Macro            | Default | Significado                                        |
|------------------|--------:|----------------------------------------------------|
| `PF_DISTANCE`    | `128`   | Bytes à frente para prefetchar (= 2 cache lines)   |

## Correção

Validado por `make test` em todas as contagens `{1, 2, 3, 4, 7, 8}`.
Resultado da última execução: **All correctness tests PASSED.**

## Leitura relacionada

- [`pthread_chunked.md`](pthread_chunked.md) — baseline sem prefetch.
- [`pthread_chunked_v2.md`](pthread_chunked_v2.md) — versão com loops
  separados mas sem prefetch — compare para isolar o efeito do
  `__builtin_prefetch`.
