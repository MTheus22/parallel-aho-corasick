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
| **`K = 4·nthreads`** | Valor de partida da literatura de work-stealing (TBB, Cilk Plus) |
| `K = 16·nthreads` | Tarefas mais finas reduzem o desbalanço de cauda |

O valor 4 vem da literatura de work-stealing (TBB, Cilk Plus) como
ponto de partida razoável. Não é tunado por workload neste laboratório.

> **Correção empírica (sweep de 2026-05-28).** Uma versão anterior
> desta tabela afirmava que `K = 16·nthreads` era "excessivo" porque o
> warm-up roubaria mais tempo do que economiza. **Isso está errado neste
> regime.** O custo de warm-up é `num_tasks × (L-1)` bytes *distribuído*
> por todas as threads — escala de KB perante 1,36 GiB, desprezível
> (ver §"Comparação justa vs `pthread_chunked_v3`" abaixo). O que
> realmente limita o ganho de K alto **não é warm-up**: é um platô de
> desbalanço de cauda + variância (medido abaixo).

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

## Comparação justa vs `pthread_chunked_v3`

A tabela acima compara contra o `pthread_chunked` **plano** (chunks
iguais, sem afinidade). Essa não é a comparação que decide o veredito:
o concorrente real do dispatch dinâmico é o
[`pthread_chunked_v3`](pthread_chunked_v3.md), que resolve a
heterogeneidade P/E **estaticamente** — chunks dimensionados por
`cpuinfo_max_freq` + pin topológico determinístico. Os dois atacam o
mesmo problema (cores heterogêneos) por caminhos opostos: v3 *prevê* a
assimetria na partição; dynamic *reage* a ela em runtime.

Medições de **2026-05-28**, Enron 1,36 GiB, T=12, i5-1235U,
`--warmup 1`.

**Regime ET (44k regras, autômato ~515 MiB ≫ L3) — o regime headline
do projeto (memory-bound, cache blowout):**

| Searcher              | mean (ms) | max (ms) | MB/s |
|-----------------------|----------:|---------:|-----:|
| `pthread_chunked_v3`  | **3276**  | 3464     | **414** |
| `pthread_dynamic`     | 5306      | 6813     | 255  |

→ v3 é ~**38% mais rápido** e muito mais estável (max 3464 vs 6813).

**Regime Snort (4188 regras):**

| Searcher              | mean (ms) | max (ms) | MB/s |
|-----------------------|----------:|---------:|-----:|
| `pthread_chunked_v3`  | **1297**  | 1382     | **1045** |
| `pthread_dynamic`     | 1641      | 1799     | 826  |

→ v3 ~21% melhor na média. (Em *min* isolados o dynamic às vezes ganha
— é o efeito de alta variância já descrito acima, não um ganho de
regime.)

### Duas melhorias "óbvias" testadas — ambas falham

Para checar se o gap era corrigível, prototipou-se um
`pthread_dynamic_pinned` (dynamic + o pinning topológico do v3, com `K`
configurável). Resultados no regime ET, T=12:

1. **Adicionar afinidade topológica.** ET mean **5306 → 5312**: nenhuma
   melhora. (Snort melhorou marginalmente: 1641 → 1545.) **A lentidão
   do dynamic no regime pesado não vem de falta de pinning.**

2. **Tarefas mais finas (sweep de K).**

   | `K_PER_THREAD` | mean (ms) | max (ms) |
   |---------------:|----------:|---------:|
   | 4              | 5907      | 6663     |
   | 16             | 5391      | 6433     |
   | 32             | 4544      | 6122     |
   | 64             | 4420      | 6295     |

   K maior ajuda (confirma que a causa é desbalanço de cauda das
   tarefas grossas e iguais), mas **estaciona ~35% acima do v3** (3276)
   e a variância persiste. O *melhor caso* (min ~3500) chega perto do
   v3; a *média*, não.

### Causa-raiz (por que o dynamic perde neste regime)

1. **O v3 já resolve a heterogeneidade P/E de graça e na origem.** Ele
   dimensiona cada chunk à frequência do core; o ratio de frequência
   (4400/3300 ≈ 1,33) bate com o throughput por-thread medido, então
   todos os workers terminam quase juntos — sem cauda, sem overhead de
   runtime.
2. **O dynamic com tarefas de tamanho igual reintroduz exatamente o
   desbalanço que o v3 elimina.** Uma tarefa de ~29 MB num E-core leva
   ~1,5× a de um P-core; perto do fim, se um E-core pega uma das últimas
   tarefas, ele *gateia* o `pthread_join` enquanto cores rápidos ficam
   ociosos — e *qual* core pega a última tarefa é aleatório → daí a
   variância gigante.
3. **O dispatch dinâmico só compensaria contra variância
   *imprevisível*** (ruído de scheduler, thermal, page faults
   irregulares). O protocolo do TCC (`--warmup ≥ 1`, máquina quieta)
   minimiza isso. Logo o dynamic paga todos os custos da adaptatividade
   sem colher o benefício.

**Conclusão:** bag-of-tasks é forte para *cores homogêneos + variância
externa alta*; este projeto é *cores heterogêneos + velocidade
previsível + variância baixa* — o regime oposto. Aceita-se que
`pthread_dynamic` é inferior aqui; o valor dele para o TCC é como
**contraponto controlado** que isola por que o particionamento
freq-weighted estático (v3) vence.

### Caminho real para superar o v3 (trabalho futuro)

Se o objetivo for salvar a ideia de dispatch dinâmico, o caminho que
*poderia* vencer é um **híbrido work-stealing**: base = partição
freq-weighted do v3 (cauda zero no caso comum, 1 warm-up por thread,
prefetch contíguo perfeito), e só *roubar* sub-chunks finos da região
do worker mais lento quando uma thread termina cedo. Isso absorveria
stragglers imprevisíveis sem pagar o imposto das tarefas iguais que
afunda o dynamic atual.

## Tunables

| Tunable                              | Default | Significado                                  |
|--------------------------------------|--------:|----------------------------------------------|
| tasks/thread (`--tasks-per-thread N` / `AC_DYN_TASKS_PER_THREAD`) | `4` | Multiplicador `num_tasks = N · nthreads` |

`num_tasks` é configurável **em runtime, sem recompilar** — exatamente
para automatizar o sweep de granularidade. Ordem de resolução:

1. CLI `--tasks-per-thread N` (passa via `cfg->tasks_per_thread`);
2. env `AC_DYN_TASKS_PER_THREAD=N`;
3. fallback `K_PER_THREAD_DEFAULT = 4`.

O valor efetivo é ecoado no header do run
(`# warmup=… tasks_per_thread=N`), em texto e CSV. Valores razoáveis:
2, 4, 8, 16, 32, 64 (ver §"Tarefas mais finas" acima). É a alavanca da
**fase G** (`phase_G` em
[`scripts/run_sweep.sh`](../../scripts/run_sweep.sh)), o sweep de
granularidade do P1 ([`../TODO.md`](../TODO.md)).

> Histórico: antes era o `#define K_PER_THREAD 4`, que exigia uma
> recompilação por ponto de sweep.

## Correção

Validado por `make test` em todas as contagens `{1, 2, 3, 4, 7, 8}` da
suíte padrão. Resultado da última execução: **All correctness tests
PASSED.**

## Leitura relacionada

- [`pthread_chunked.md`](pthread_chunked.md) — versão estática de
  particionamento.
- [`pthread_chunked_v3.md`](pthread_chunked_v3.md) — particionamento
  estático freq-weighted + pin topológico; vence o dynamic no regime
  ET (ver §"Comparação justa vs `pthread_chunked_v3`" acima).
