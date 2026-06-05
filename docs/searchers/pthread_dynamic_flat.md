# Searcher `pthread_dynamic_flat`

Composição do **dispatch dinâmico (bag of tasks)** do
[`pthread_dynamic`](pthread_dynamic.md) com a **tabela achatada de
pattern_ids** da idea 5 (a mesma de [`sequential_flat`](sequential_flat.md)
e [`pthread_chunked_flat`](pthread_chunked_flat.md)). Junta os dois eixos
de ganho ortogonais: **balanceamento de carga dinâmico** (workers puxam
tarefas de uma fila atômica) **e** **emissão linear** de matches por
estado.

- Fonte: [`src/searchers/pthread_dynamic_flat.c`](../../src/searchers/pthread_dynamic_flat.c)
- Registro: `__attribute__((constructor)) dyn_flat_register()`
- Descrição: *Pthreads dynamic dispatch (bag of tasks) + flat output table (idea 5)*
- Layout transformado: [`../architecture/flat-outputs.md`](../architecture/flat-outputs.md)
- Notas do TCC: [`../../../tcc_notes/sections/notes/methodology.md`](../../../tcc_notes/sections/notes/methodology.md)

## Motivação

Em CPUs **híbridas** (Alder/Raptor Lake P+E), a forma certa de balancear
é peso estático ciente de topologia — é o que faz `pthread_chunked_v3`.
Em CPUs **homogêneas** (ex.: AMD Zen 5, 16 núcleos idênticos) esse peso
estático colapsa para uniforme e perde o sentido; o load-balancer
correto passa a ser **dinâmico**: fatiar o texto em `K = 4·N` tarefas e
deixar workers consumirem da fila conforme terminam. `pthread_dynamic`
já faz isso, mas emite matches caminhando a cadeia
`(own_out_head, dict_suffix, outputs)` — o mesmo custo por match que a
idea 5 elimina.

`pthread_dynamic_flat` é para o `pthread_dynamic` o que o
`pthread_chunked_flat` é para o `pthread_chunked_v2`: **fluxo de
controle idêntico, só a emissão muda**. É o candidato a campeão
portável em cores homogêneos, onde `chunked_flat` (chunks estáticos)
pode sofrer com assimetria de SMT/CCD/jitter que o dispatch dinâmico
absorve de graça.

## Quando usar

- **Cores homogêneos de muitos núcleos** (Zen 5, EPYC, Xeon não-híbrido):
  o dispatch dinâmico cobre desbalanceamento residual (contenção de SMT,
  fronteira de CCD, throttling) sem o overhead de sondar `cpufreq`.
- Sempre que quiser o ganho da idea 5 **sem** assumir que cada slice
  estático faz o mesmo trabalho.

## Quando NÃO usar

- Em corpus muito pequenos: faz fallback automático para
  `sequential_flat` (e, se não registrado, para `sequential`). A
  heurística é a mesma do `pthread_dynamic`/`chunked_flat`:

  ```text
  nthreads == 1
  || text_len <= 2 * (max_pattern_len - 1)
  || text_len <  nthreads * 64
  ```

- Em CPU híbrida, comparar contra `pthread_chunked_v3_flat`: lá o peso
  estático por tipo de core pode ganhar do dispatch dinâmico.

## Algoritmo, em uma frase

Idêntico ao [`pthread_dynamic`](pthread_dynamic.md) — `K = 4·N` tarefas
`[core_start, core_end)` com warm-up `L-1`, puxadas via
`atomic_fetch_add` —, exceto que o loop owned lê
`(flat_offset[state], flat_count[state], flat_pids[])` em vez de
caminhar `(own_out_head, dict_suffix, outputs)`.

## Estruturas consumidas

Da `ac_automaton_t` (read-only):

- `goto_tbl[state * 256 + byte]` — função de transição.
- `flat_offset[state]`, `flat_count[state]`, `flat_pids[]` — arenas da
  idea 5.
- `max_pattern_len` — para derivar `overlap = max_pattern_len - 1`.

## Invariantes preservados

1. **Overlap = `max_pattern_len - 1`** por tarefa (warm-up `[scan_start,
   core_start)` é state-only, sem emissão).
2. **Disjoint ownership por tarefa** — cada tarefa emite só matches com
   `end_pos ∈ [core_start, core_end)`. As tarefas cobrem `[0, text_len)`
   sem buracos nem sobreposição na visão de emissão.
3. **Atomic só nas fronteiras de tarefa** — `atomic_fetch_add_explicit(..,
   memory_order_relaxed)` é tocado uma vez por tarefa, **nunca por byte**.
   O hot loop não tem locks/atomics.
4. **Matches thread-local** — cada worker escreve só em sua
   `ac_match_list_t`; o master concatena via `ac_match_list_extend_consume`
   após `pthread_join`.
5. **Multiset preservado** — `flat_pids[s]` é construído na mesma ordem
   do chain walk; após `ac_match_list_sort` a saída é bit-equivalente ao
   `sequential`. A **atribuição** de cada match a uma thread é
   não-determinística (depende de quem pegou a tarefa), mas o **conjunto**
   é invariante.

## Garantias

- **Determinístico após sort**: igual ao `sequential`. Validado por
  `tests/test_correctness.c` em `{1,2,3,4,7,8}` threads e em todos os 6
  casos (54 verificações OK, 0 FAIL).
- **TSan-clean**: zero warnings sob `make tsan` — leitura read-only do
  autômato + escrita só na lista local; a única mutação compartilhada é
  o contador atômico.

## Métricas por thread

Reporta `ac_thread_metric_t` por worker (mesmo formato do
`pthread_dynamic`): `thread_id`, `seconds`,
`bytes_scanned = tasks_done * task_size` (aproximado — ignora overlap,
contribuição pequena), `matches_found = local.count`. Como o dispatch é
dinâmico, `tasks_done` varia entre workers; é exatamente esse spread que
documenta o balanceamento.

## Benchmark de sanidade (i5-1235U, híbrido, T=12)

Snort + 600 MB de `enron_corpus`, `--warmup 1 --iters 3`,
`-O3 -march=native`, build sequencial. Mesma contagem de matches
(8.147.760) em todos:

| Searcher                | MB/s   | mean(ms) | cv%  |
|-------------------------|-------:|---------:|-----:|
| `sequential_flat`       | 108,50 | 5.273,9  | 2,48 |
| `pthread_dynamic`       | 432,62 | 1.322,7  | 8,41 |
| `pthread_chunked_flat`  | 467,67 | 1.223,5  | 4,99 |
| `pthread_dynamic_flat`  | **472,31** | **1.211,5** | **1,64** |

Mesmo no laptop híbrido (onde o teto é P/E balancing, não emissão),
`pthread_dynamic_flat` já empata/supera o `chunked_flat` e tem a **menor
variância** — o dispatch dinâmico estabiliza o tempo de parede. O regime
onde ele deve abrir vantagem clara é em **cores homogêneos de muitos
núcleos** (Zen 5/EPYC), onde os chunks estáticos do `chunked_flat` ficam
mais expostos a jitter. Esse é o número que o sweep da máquina reservada
deve confirmar — ver [`../sweep-test-inventory.md`](../sweep-test-inventory.md).

## Complexidade

- **Tempo da busca**: `O(|texto|/T + |saídas|/T)` no caso bem-balanceado;
  o dispatch dinâmico aproxima esse ideal melhor que chunks estáticos sob
  assimetria.
- **Coordenação**: `O(T)` para create/join + `O(K)` atomics nas
  fronteiras de tarefa + merge sequencial das listas locais.
- **Memória extra**: `K = 4·T` structs `task_t` + listas locais (4096
  entradas iniciais por worker).

## Leituras relacionadas

- Pai do dispatch: [`pthread_dynamic.md`](pthread_dynamic.md).
- Pai do layout flat: [`pthread_chunked_flat.md`](pthread_chunked_flat.md).
- Layout do flat output table: [`../architecture/flat-outputs.md`](../architecture/flat-outputs.md).
- Modelo de chunking/correção em fronteira: [`../architecture/parallelism.md`](../architecture/parallelism.md).
- Plano do sweep na máquina reservada: [`../sweep-test-inventory.md`](../sweep-test-inventory.md).
