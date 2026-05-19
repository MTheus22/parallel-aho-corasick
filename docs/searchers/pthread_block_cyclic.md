# Searcher `pthread_block_cyclic`

Distribuição estática round-robin de blocos. Worker `i` processa
blocos `{ i, i+N, i+2N, ... }` onde cada bloco tem 1 MiB por padrão.

- Fonte: [`src/searchers/pthread_block_cyclic.c`](../../src/searchers/pthread_block_cyclic.c)
- Registro: `__attribute__((constructor)) bc_register()`
- Descrição: *Pthreads static round-robin blocks (1 MiB), thread-local lists*

## Posicionamento

Este searcher fica entre `pthread_chunked` (slice único contíguo por
worker) e `pthread_dynamic` (fila atômica de tarefas):

| Aspecto                    | `pthread_chunked` | **`pthread_block_cyclic`** | `pthread_dynamic` |
|----------------------------|-------------------|----------------------------|-------------------|
| Atribuição                 | Estática contígua | Estática cíclica           | Dinâmica          |
| Atômico no hot path        | Não               | Não                        | Sim (entre tarefas)|
| Distribui ruído transiente | Mal               | Razoavelmente              | Bem                |
| Locality da streaming load | Excelente         | Quebrada (stride = N·B)    | Excelente          |
| Warm-up por bloco          | Único (por worker)| Por bloco (~N× mais)       | Por tarefa (~N·K_PER_THREAD× mais) |

A hipótese é: o **stride cíclico** distribui o ruído transitório
(preempção, page-fault) por todos os workers em vez de concentrá-lo
em poucos, sem precisar de coordenação atômica entre eles.

## Esquema

Bloco `k` ocupa bytes `[k·B, (k+1)·B)`. Worker `i` itera
`k = i, i+N, i+2N, ...` enquanto `k·B < text_len`.

```text
text [0..L), B = 1 MiB, N = 4 threads:

block:  0   1   2   3   4   5   6   7   ...
owner:  W0  W1  W2  W3  W0  W1  W2  W3  ...
```

Cada bloco tem seu próprio warm-up de `overlap = max_pattern_len - 1`
bytes antes de `core_start`. Mantém a regra de ownership: matches com
`end_pos` em `[core_start, core_end)` são emitidos pelo worker que
escolheu aquele bloco; matches em `[scan_start, core_start)` são
descartados localmente.

## Por que 1 MiB de bloco?

- **Pequeno demais (< 64 KiB)**: a região de warm-up (até 470 B na
  Snort) começa a dominar; muitos blocos disputam o mesmo conjunto de
  páginas físicas.
- **Grande demais (> 16 MiB)**: o número de blocos cai abaixo de
  ~50/worker → menos oportunidades de balancear.
- **1 MiB**: por enquanto, o padrão. Resulta em ~1.350 blocos para
  o corpus Enron de 1,36 GiB, ou ~112 blocos por worker em 12 threads
  — folga suficiente para amortizar ruído.

O searcher aceita um override via `cfg->chunk_size` (campo `--chunk`
no CLI), com piso de `max(overlap · 4, 65.536)` para impedir
escolhas patológicas.

## Trade-off de localidade

O custo conhecido do block-cyclic é que blocos consecutivos de um
mesmo worker **não são contíguos**: worker 0 lê bytes
`[0, 1MiB)`, depois pula para `[N·B, (N+1)·B)`, etc. O prefetcher de
hardware perde o stream contínuo.

Em prática, 1 MiB já é suficientemente longo para que o prefetcher
"reaprenda" o padrão dentro de cada bloco e amortize a perda. Mas é
um efeito que precisa ser medido por workload.

## Correção

A regra continua sendo overlap = `L - 1` e ownership por `end_pos`. O
multiset de matches é idêntico ao do `sequential`. A ordem de
inserção na lista local muda — `make test` ordena antes de comparar
e não se importa.

## Resultados em `snort + enron_corpus.txt` (1,36 GiB)

| Searcher              | Threads | min (ms) | mean (ms) | MB/s (mean) |
|-----------------------|--------:|---------:|----------:|------------:|
| pthread_chunked       |   1     | 7.265    | 7.381     | 183,6       |
| pthread_block_cyclic  |   1     | 7.288    | 7.390     | 183,4       |
| pthread_chunked       |   2     | 4.573    | 4.701     | 288,3       |
| pthread_block_cyclic  |   2     | 4.597    | 4.796     | 282,6       |
| pthread_chunked       |  12     | 1.841    | 1.914     | 708,2       |
| **pthread_block_cyclic**| 12    | **1.692**| 1.841     | 736,1       |

Em 12 threads o `min(ms)` é ~8% melhor que o `pthread_chunked`; no
`mean(ms)` o ganho é ~4%. O comportamento se parece com o do
`pthread_dynamic`, mas sem o custo do átomo — coerente com a
hipótese de que parte do balanço dinâmico pode ser conseguido com
distribuição estática se o stride for o suficiente.

Em 1 e 2 threads não há ganho — esperado, pois o efeito depende do
número de blocos serem ≫ número de workers.

## Tunables

| Macro/flag              | Default     | Significado                                       |
|-------------------------|-------------|----------------------------------------------------|
| `BC_BLOCK_BYTES`        | `1 << 20`   | Tamanho do bloco em bytes                          |
| `--chunk N` (CLI)       | `BC_BLOCK_BYTES` | Override pelo usuário; piso = `overlap · 4` / 64 KiB |

## Correção

Validado por `make test` em todas as contagens `{1, 2, 3, 4, 7, 8}`
da suíte padrão. Resultado da última execução: **All correctness
tests PASSED.**

## Leitura relacionada

- [`pthread_chunked.md`](pthread_chunked.md) — particionamento contíguo.
- [`pthread_dynamic.md`](pthread_dynamic.md) — dispatch dinâmico (a
  alternativa via atômico).
