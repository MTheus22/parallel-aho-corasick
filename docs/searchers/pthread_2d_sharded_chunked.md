# Searcher `pthread_2d_sharded_chunked`

Implementação da **idea 6** do roadmap: paralelismo 2-D que combina
sharding de dicionário (eixo K, idea 1) com chunking de texto (eixo
N, idea 5). O total de workers é `T = K × N`. Cada worker
`(k, n)` executa o sub-autômato `k` (prefix-sharded) sobre o
chunk `n` do texto, com warm-up de `sub_aut[k].max_pattern_len - 1`
bytes na fronteira.

- Fonte: [`src/searchers/pthread_2d_sharded_chunked.c`](../../src/searchers/pthread_2d_sharded_chunked.c)
- Registro: `__attribute__((constructor)) twod_register()`
- Notas do TCC: [`../../../tcc_notes/sections/notes/methodology.md`](../../../tcc_notes/sections/notes/methodology.md) e [`../../../tcc_notes/sections/notes/results.md`](../../../tcc_notes/sections/notes/results.md)
- Sweep: [`scripts/run_2d_sharded_chunked_sweep.sh`](../../scripts/run_2d_sharded_chunked_sweep.sh)

## Por que existir

Os experimentos de idea 1 + idea 5 (ver `pattern_sharded.md` e
`pthread_chunked_flat.md`) deixaram duas constatações que motivaram
a variante 2-D:

- **Sharding standalone** (`pattern_sharded_prefix`) entrega ganho
  pequeno em dicionário grande (≈ 1,21× em Snort full + enron,
  K=8) — porque cada worker re-lê o **texto inteiro**, multiplicando
  o tráfego de RAM por K.
- **Chunking standalone** (`pthread_chunked_flat`) entrega ganho
  grande na busca (3,6–4,5×) — mas **não ataca** o cache blowout
  do autômato unificado em dicionários >> L3.

A hipótese da idea 6 era que combinar os dois eixos transferiria
parte do gargalo de banda de RAM para *cache locality por shard*,
de modo que `K=2 × N=6` (T=12) batesse `pthread_chunked_flat`
(T=12) em dicionários grandes. **A hipótese foi falsificada
empiricamente** nas medições deste laboratório — veja "Headline
benchmark" abaixo.

## Modelo de execução

```mermaid
flowchart TD
    Master[Master thread] --> Build[Build K sub-automatos<br/>(política PREFIX)]
    Build --> S0[(sub_aut[0])]
    Build --> S1[(sub_aut[K-1])]
    Build --> R0[remap[0..K-1]: pid local -> pid global]

    Master --> Chunk[Divide texto em N chunks<br/>chunk n: [n*core, (n+1)*core)]

    Master --> W00[Worker (0,0):<br/>sub_aut[0] / chunk 0]
    Master --> W01[Worker (0,N-1):<br/>sub_aut[0] / chunk N-1]
    Master --> W10[Worker (K-1,0):<br/>sub_aut[K-1] / chunk 0]
    Master --> W11[Worker (K-1,N-1):<br/>sub_aut[K-1] / chunk N-1]

    W00 --> L00[(local matches)]
    W01 --> L01[(local matches)]
    W10 --> L10[(local matches)]
    W11 --> L11[(local matches)]

    Master -->|pthread_join + concat| Out[(ac_match_list_t global)]
```

Características importantes:

- **Sharding policy fixa em PREFIX** (`shard = pat[0] % K`). As
  outras duas políticas registradas pela idea 1 (round-robin, LPT)
  perdem em todos os regimes medidos; não há razão para repetir
  o experimento aqui.
- **Warm-up por shard.** Cada worker usa `sub_aut[k].max_pattern_len - 1`
  bytes de overlap. Esse valor é estritamente menor ou igual ao
  `aut->max_pattern_len` unificado — o shard nunca vê padrões de
  outro shard, então o overlap pode ser apertado por shard.
- **Ownership de matches.** Match com `end_pos = p` é emitido
  pelo worker que possui o chunk contendo `p`, no shard que
  possui o pattern. Pares `(end_pos, pattern_id)` são disjuntos
  entre workers — concatenação simples no merge.
- **Emissão pela tabela achatada** (idea 5). Cada `sub_aut[k]`
  popula `flat_offset/flat_count/flat_pids` automaticamente em
  `ac_automaton_build`; o hot loop do worker faz `flat_pids[off+k]`
  e remapeia para o pid global via `pid_remap[k]`.

## Estratégia de seleção de K

O número de workers `T` vem de `cfg->num_threads`. A escolha de
`K` (e portanto de `N = T / K`) segue este critério:

1. **`T == 1`** → delega para `sequential_flat`. Não há paralelismo
   possível; mantém o flat layout (idea 5) no caminho.
2. **K-preferido = 2** (override via env `AC_2D_K=<n>`). A idea 1
   mostrou que `pattern_sharded_prefix` satura em K=2 — é onde o
   ganho de cache locality "compensa" antes do tráfego de texto
   colapsar.
3. **`K` precisa dividir `T`** e ser `≤ num_patterns`. Se a
   divisão falha (T ímpar, T=3, T=7, etc.) ou `K > num_patterns`,
   o código **delega para `pthread_chunked_flat`** — um sub-autômato
   equivalente ao unificado só duplicaria memória sem ganho.

Resultado para os thread counts de teste:

| T  | K | N  | Caminho exercido                          |
|----|---|----|--------------------------------------------|
| 1  | 1 | 1  | delega → `sequential_flat`                 |
| 2  | 2 | 1  | 2-D: 2 shards × 1 chunk                   |
| 3  | 1 | 3  | delega → `pthread_chunked_flat`           |
| 4  | 2 | 2  | 2-D: 2 shards × 2 chunks                  |
| 7  | 1 | 7  | delega → `pthread_chunked_flat`           |
| 8  | 2 | 4  | 2-D: 2 shards × 4 chunks                  |
| 12 | 2 | 6  | **2-D headline: 2 shards × 6 chunks**     |

Override via env:
- `AC_2D_K=4` força K=4 (em T=12 → N=3).
- `AC_2D_K=12` força K=12 (em T=12 → N=1, equivalente a
  `pattern_sharded_prefix` com K=12).

## Quando usar / quando NÃO usar

### Usar

- **Quando o estudo do TCC pede uma comparação 2-D direta** com os
  searchers 1-D (chunking puro, sharding puro). É o único searcher
  do laboratório que materializa essa composição.
- **Como pivô para sweep de K-vs-N** em hardware com mais cores
  (e/ou L3 maior) onde a relação `K × bytes(text) ≤ memory_bw`
  ainda não saturou. A hipótese original pode reverter em
  máquinas com bandwidth substancialmente maior que o i5-1235U.

### NÃO usar

- **Como searcher de produção neste hardware** — `pthread_chunked_flat`
  é consistentemente mais rápido em todos os dicionários e thread
  counts medidos.
- **Para T ímpar (3, 7)** — o código delega para
  `pthread_chunked_flat` automaticamente, mas o nome do searcher
  vira enganoso ("2D" mas executou 1D). Documentar a delegação no
  benchmark report.

## Invariantes em que o searcher se apoia

1. **Autômato unificado imutável após o build.** Lido apenas no
   build dos sub-autômatos (para extrair `patterns[gid].text` e
   `length`). O hot loop dos workers nunca o toca.
2. **Sub-autômatos imutáveis após `twod_shard_build`.** Cada
   `sub_aut[k]` herda os mesmos invariantes do `ac_automaton_t`
   regular; nenhum write após o build.
3. **`pid_remap[k]` escrito antes do `pthread_create`.** Workers
   leem via `const int32_t *AC_RESTRICT remap`. Happens-before do
   `pthread_create` garante visibilidade.
4. **Overlap = `sub_aut[k].max_pattern_len - 1`** por shard.
   Apertar mais (i.e., usar o overlap unificado para todos os
   workers) seria correto mas desperdiçaria warm-up; usar valor
   menor que `sub_aut[k].max_pattern_len - 1` perderia matches em
   fronteira do shard `k`.
5. **Match ownership disjunto em duas dimensões.**
   - Dimensão dicionário: shards têm pids globais disjuntos por
     construção.
   - Dimensão texto: chunks `[core_start, core_end)` são
     disjuntos por construção. Match com `end_pos ∈ [scan_start,
     core_start)` cai no chunk anterior e é silenciosamente
     descartado pelo split-loop (warm-up phase não emite).
6. **Sem locks, mutexes, atomics no hot loop.** Lista de matches
   por worker; merge sequencial no master após `pthread_join`.
   Mesmo padrão de todos os outros pthread searchers.
7. **Cache invalidado por fingerprint multi-campo.** Igual ao
   `pattern_sharded`: `(goto_tbl pointer, num_states,
   num_outputs, num_patterns, K)`.

## Garantias de correção

- **Determinístico**: igual ao `sequential` no multiset de matches.
  `make test` valida 9 casos × 6 thread counts `{1, 2, 3, 4, 7, 8}`,
  incluindo `prefix_bias_h` (testa shards vazios) e `phonetic_16`
  (testa diferentes prefixos por padrão).
- **TSan limpo**: `make tsan` (com `setarch -R` para contornar o
  mapping glitch do TSan) não reporta warnings. O hot loop só lê
  `sub_aut[k].*` e `pid_remap[k][...]`, ambos read-only depois do
  build dos shards.

## Headline benchmark

Ambiente: 12-core x86_64 (Intel i5-1235U, 2 P-cores HT + 8 E-cores),
kernel 6.17, `-O3 -march=native`, GCC. Corpus:
`data/simplewiki.txt` (≈ 1,22 GiB). Sequential é o baseline
single-thread (T=1). `--warmup 1 --iters 3`.
`AC_BUILD_PARALLEL=1` em todos os runs paralelos.

### Snort full (4 188 padrões · 55 479 estados · ~56 MiB)

| Searcher                              | T  | K  | N | MB/s    | Mean (ms) | Speedup vs `sequential` |
|---------------------------------------|----|----|---|---------|-----------|-------------------------|
| `sequential`                          | 1  | —  | — |  89,88  | 13 533,85 | 1,00×                   |
| `pthread_chunked_flat`                | 12 | 1  |12 | **488,28** | **2 491,29** | **5,43×**          |
| `pthread_2d_sharded_chunked`          | 12 | 2  | 6 | 356,60  | 3 411,28  | 3,97×                   |

Match count: 12 245 582 (todos idênticos ao `sequential`). Ratio
2-D / chunked_flat = **0,73×** (27 % mais lento).

### ET-32 (44 678 padrões · 508 896 estados · ~515 MiB — 42× L3)

| Searcher                              | T  | K  | N | MB/s    | Mean (ms) | Speedup vs `sequential` |
|---------------------------------------|----|----|---|---------|-----------|-------------------------|
| `sequential`                          | 1  | —  | — |  48,99  | 24 828,65 | 1,00×                   |
| `pthread_chunked_flat`                | 12 | 1  |12 | **195,23** | **6 230,80** | **3,99×**          |
| `pthread_2d_sharded_chunked`          | 12 | 2  | 6 | 166,80  | 7 292,96  | 3,40×                   |

Match count: 42 284 944. Ratio 2-D / chunked_flat = **0,85×**.

### K-sweep ET-32 + simplewiki (T=12 fixo)

| K  | N  | MB/s   | vs `pthread_chunked_flat` (177,46 MB/s) |
|----|----|--------|------------------------------------------|
| 1  | 12 | 177,46 | 1,00× (referência — delega)              |
| 2  | 6  | 150,49 | 0,85×                                    |
| 3  | 4  | 142,96 | 0,81×                                    |
| 4  | 3  | 135,51 | 0,76×                                    |
| 6  | 2  | 107,92 | 0,61×                                    |
| 12 | 1  |  71,60 | 0,40×                                    |

A vazão decresce monotonicamente conforme K cresce — o que é
o sintoma direto do **gargalo de banda de RAM dominando**: a
cada incremento de K, o tráfego total de texto cresce
proporcionalmente (cada worker re-lê o seu chunk pelo seu
sub_aut, mas existem `K × N = T` workers totais e cada chunk é
lido `K` vezes em paralelo).

### Snort full + enron_corpus (T=12, 1,32 GiB)

| Searcher                              | T  | K  | N | MB/s    | Mean (ms) | Speedup vs `sequential` |
|---------------------------------------|----|----|---|---------|-----------|-------------------------|
| `sequential`                          | 1  | —  | — |  88,48  | 15 317,31 | 1,00×                   |
| `pthread_chunked_flat`                | 12 | 1  |12 | **367,07** | **3 692,35** | **4,15×**          |
| `pthread_2d_sharded_chunked`          | 12 | 2  | 6 | 309,96  | 4 372,64  | 3,50×                   |
| `pattern_sharded_prefix`              | 8  | —  | — | 116,91  | 11 592,68 | 1,32×                   |

### Speedup vs `pthread_chunked_flat` em vários T (ET-32 + simplewiki)

| T  | `pthread_chunked_flat` (MB/s) | `pthread_2d_sharded_chunked` K=2 (MB/s) | Ratio |
|----|-------------------------------|--------------------------------------------|-------|
| 4  |  98,30                        |  88,13                                     | 0,90× |
| 8  | 145,43                        | 118,93                                     | 0,82× |
| 12 | 177,46                        | 155,22                                     | 0,87× |

## Por que a hipótese caiu (interpretação)

A motivação original assume que o gargalo de
`pthread_chunked_flat` em dicionários grandes seja o **cache blowout
do autômato unificado**: cada byte gera uma transição num
`goto_tbl[state*256+c]` de 515 MiB (ET-32), 42× a L3 do
i5-1235U. Sharding deveria mitigar isso porque cada sub-autômato é
K vezes menor; em K=2, ~258 MiB — ainda >> L3, mas com working set
menor por thread.

A teoria falha porque:

1. **A redução de footprint do shard não desce o suficiente.** Para
   sair do regime DRAM-bound, o sub-autômato precisaria caber em
   L2 (1,25 MiB no i5-1235U) ou pelo menos em L3 (12 MiB). K=2
   produz sub-autômatos de ~258 MiB; K=12 de ~43 MiB — ainda não
   cabe. A latência por transição continua dominada por leitura
   de DRAM.
2. **O custo de banda de texto cresce linearmente em K.** Para
   K=2 N=6 sobre `simplewiki.txt` (1,22 GiB), o tráfego de texto
   passa de ~1,22 GiB (chunked_flat, K=1) para ~2,44 GiB (K=2).
   Em hardware com banda saturada, esse é um custo direto que
   não é amortizado.
3. **A redução de latência por miss é parcial.** Mesmo que cada
   miss custe menos por estar num sub-autômato mais "denso", o
   número de misses por byte permanece similar — porque a
   ramificação típica do hot path do AC carrega um número
   parecido de linhas de cache distintas independentemente do K.

Resultado consolidado: no regime IDS real (Snort ≥ 56 MiB,
ET-32 ≥ 515 MiB), **dividir a dicionário em K shards não compensa
o custo adicional de re-leitura do texto** em hardware desktop com
~25 GB/s de banda DRAM. A composição 2-D só recupera o nível do
chunking puro quando K=1 (e nesse caso é literalmente o chunked
flat). Para hardware servidor com banda de >100 GB/s ou L3 de >32
MiB, a relação pode reverter — o experimento permanece útil como
ponto comparativo.

## O que sobrou de valor

Mesmo com a hipótese falsificada, o searcher é **defensável** no
TCC por três motivos:

1. **Negativo é resultado.** A literatura paralela de AC raramente
   reporta tentativas falhas; a dissertação ganha em honestidade
   experimental ao mostrar o ponto onde a combinação para de
   compor.
2. **Cobertura do espaço.** Junto com `pattern_sharded` (1-D
   sharding) e `pthread_chunked_flat` (1-D chunking), o searcher
   2-D fecha a grade: o leitor tem todos os pontos para inferir
   por que paralelismo de texto domina paralelismo de
   dicionário neste hardware.
3. **Plataforma para experimentos futuros.** Trocar a política
   de sharding (LPT, hash de prefixo de 2 bytes, etc.), trocar
   a distribuição de chunks por uma freq-weighted como
   `pthread_chunked_v3`, ou mover o build para
   `ac_automaton_build_par`, ficam todos atrás de uma única
   recompilação.

## Custo do build

Para `K = AC_2D_K = 2`, o searcher constrói **2 sub-autômatos
sequenciais** via `ac_automaton_build` (o mesmo do
`pattern_sharded`). Custo total típico:

- Snort full (4 188 padrões): ~120 ms por sub-automato (vs
  ~50 ms para o unificado sequencial; ~30 ms paralelo T=12).
- ET-32 (44 678 padrões): ~970 ms total para K=2 (vs ~820 ms
  unificado paralelo). Pequena penalidade de build, paga uma
  vez por `(aut, K)` graças ao cache estático.

O ponto a observar para o TCC: o build de `K` sub-autômatos não
escala como `K × build(unified)` porque cada um cobre `~1/K`
dos padrões, mas a parte fixa (alocação, BFS por nível, flat
output table) é re-paga.

## Como o harness chama

```text
twod_search(aut, text, text_len,
            cfg /* num_threads = T */,
            out_matches,
            out_thread_metrics → tm[T], out_num_thread_metrics → T)
```

Per-thread metrics:
- `tm[i].thread_id`: `i = k * N + n` (ordem shard-major).
- `tm[i].bytes_scanned`: `core_end - scan_start` (inclui warm-up
  do shard `k`).
- `tm[i].matches_found`: contagem da partição `(k, n)`.

## Trabalho futuro

- **Política de sharding sensível a footprint**: medir
  `ac_automaton_memory_bytes` por shard e re-balancear até que
  todos caibam em L3. Equivalente a um bin-packing por estado,
  não por bytes-de-padrão.
- **Chunk size adaptativo**: usar o sweep de `pthread_chunked_v3`
  (freq-weighted) para o eixo N. Ortogonal ao K.
- **Build paralelo dos sub-autômatos**: chamar
  `ac_automaton_build_par` em vez de `ac_automaton_build` para
  cada shard. Reduz o tempo de cache priming em ET full.
- **Reproduzir em hardware servidor**: i5-1235U tem ~25 GB/s de
  banda DRAM e 12 MiB de L3. Repetir o experimento em Xeon ou
  EPYC com banda > 200 GB/s e L3 > 128 MiB pode reverter a
  conclusão.

## Leituras relacionadas

- Consolidação metodológica e números: [`../../../tcc_notes/sections/notes/methodology.md`](../../../tcc_notes/sections/notes/methodology.md) e [`../../../tcc_notes/sections/notes/results.md`](../../../tcc_notes/sections/notes/results.md).
- Eixo K standalone: [`pattern_sharded.md`](pattern_sharded.md).
- Eixo N standalone: [`pthread_chunked_flat.md`](pthread_chunked_flat.md).
- Análise de banda de RAM e cache blowout:
  [`../../../tcc_notes/sections/notes/results.md`](../../../tcc_notes/sections/notes/results.md).
- Sweep automatizado:
  [`../../scripts/run_2d_sharded_chunked_sweep.sh`](../../scripts/run_2d_sharded_chunked_sweep.sh).
