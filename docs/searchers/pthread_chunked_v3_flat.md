# Searcher `pthread_chunked_v3_flat`

Composição direta de dois ganhos já validados isoladamente:

1. **`pthread_chunked_v3`** — afinidade ciente de topologia + chunks
   ponderados por `cpuinfo_max_freq` (ver
   [`pthread_chunked_v3.md`](pthread_chunked_v3.md)).
2. **`pthread_chunked_flat`** — emissão pelo flat output table da
   idea 5 (ver [`pthread_chunked_flat.md`](pthread_chunked_flat.md)).

A hipótese é que os dois ataques são **ortogonais**: v3 redistribui os
bytes de texto entre P-cores e E-cores; o flat layout elimina o
chain-walk `(own_out_head, dict_suffix, outputs)` durante a emissão de
matches. Logo o ganho deve ser **multiplicativo**.

- Fonte: [`src/searchers/pthread_chunked_v3_flat.c`](../../src/searchers/pthread_chunked_v3_flat.c)
- Registro: `__attribute__((constructor)) v3f_register()`
- Descrição: *Pthreads chunks; topology-aware affinity + freq-weighted chunks + flat output table*
- Notas do TCC: [`../../../tcc_notes/sections/notes/methodology.md`](../../../tcc_notes/sections/notes/methodology.md), [`../../../tcc_notes/sections/notes/results.md`](../../../tcc_notes/sections/notes/results.md) e [`../../../tcc_notes/sections/notes/conclusion.md`](../../../tcc_notes/sections/notes/conclusion.md)

## Quando usar

- **CPU híbrida P+E (Alder Lake-class) com dicionário grande** — o
  caso de uso central do TCC. A heterogeneidade dos cores beneficia v3;
  o tamanho do dicionário (Snort ~55k estados, ET-32 ~5M estados)
  beneficia o flat.
- Substituto natural do `pthread_chunked_v3` para qualquer regime —
  o flat layout nunca regride e o overhead em build é desprezível
  (< 1 ms para Snort).

## Quando NÃO usar

- Inputs minúsculos: o searcher faz fallback automático para
  `sequential_flat` (e, na ausência dele, para o `sequential`). A
  heurística é a mesma de v2/v3:

  ```text
  nthreads == 1
  || text_len <= 2 * (max_pattern_len - 1)
  || text_len <  nthreads * 64
  ```

- Hardware homogêneo (todos cores em mesma frequência): a parte v3 do
  searcher degenera para chunks iguais e o ganho colapsa para o de
  `pthread_chunked_flat`. Continua correto; só não há ganho extra.

## Algoritmo, em uma frase

Idêntico ao `pthread_chunked_v3` — descobre topologia em
`/sys/devices/system/cpu`, ordena CPUs (líderes SMT por frequência
desc.), pina workers nessa ordem e dimensiona chunks proporcionalmente
ao `cpuinfo_max_freq` — exceto que o loop owned lê
`(flat_offset[state], flat_count[state])` em vez de caminhar
`(own_out_head, dict_suffix, outputs)`.

## Hot loop (fase owned)

```c
for (size_t i = core_start; i < core_end; i++) {
    uint8_t c = (uint8_t)text[i];
    state = goto_tbl[(size_t)state * AC_ALPHABET_SIZE + c];

    int32_t cnt = flat_count[state];
    if (AC_UNLIKELY(cnt > 0)) {
        int32_t off = flat_offset[state];
        for (int32_t k = 0; k < cnt; k++) {
            ac_match_t m = {
                .end_pos    = (int64_t)i,
                .pattern_id = flat_pids[off + k],
            };
            ac_match_list_push(&w->local, m);
        }
    }
}
```

A única diferença textual em relação ao hot loop de
`pthread_chunked_flat` é a presença prévia da chamada a `v3f_try_pin`
em `worker_main` e o cálculo de `core_start/core_end` proporcionais ao
peso de cada CPU.

## Invariantes preservados

1. **Overlap = `max_pattern_len - 1`** entre chunks adjacentes
   (herdado de v2/v3).
2. **Ownership disjunto** — matches com `end_pos ∈ [core_start[i],
   core_end[i])` pertencem ao worker `i`.
3. **Matches thread-local** — merge sequencial via
   `ac_match_list_extend_consume` após `pthread_join`.
4. **Sem atomics/locks no hot path** — sysfs lido uma vez por
   chamada de `search()`, fora da janela timed.
5. **Topology discovery best-effort** — sysfs indisponível degrada
   para pesos = 1 (chunks iguais, ordem identidade), comportamento
   idêntico ao `pthread_chunked_flat`.
6. **Multiset bit-equivalente ao `sequential`** após `ac_match_list_sort`
   (validado em `tests/test_correctness.c` para `{1,2,3,4,7,8}` threads).

## Estruturas consumidas

Da `ac_automaton_t` (read-only):

- `goto_tbl[state * 256 + byte]` — função de transição.
- `flat_offset[state]`, `flat_count[state]`, `flat_pids[]` —
  arenas da idea 5.
- `max_pattern_len` — para derivar overlap.

Do sysfs (read-only, fora da janela timed):

- `/sys/devices/system/cpu/cpuN/cpufreq/cpuinfo_max_freq`
- `/sys/devices/system/cpu/cpuN/topology/thread_siblings_list`

## Garantias

- **Determinístico após sort**: igual ao `sequential` para qualquer T.
- **TSan-clean**: zero warnings sob `make tsan` (testado com
  `setarch $(uname -m) -R` para contornar ASLR layout em kernels
  recentes).
- **Build limpo**: zero warnings sob o conjunto `-Wall -Wextra
  -Wpedantic -Wshadow -Wstrict-prototypes -Wmissing-prototypes
  -Wpointer-arith -Wcast-align`.

## Como o harness chama

```text
v3f_search(aut, text, text_len, cfg,
           out_matches,
           out_thread_metrics → array de tamanho spawned (ou NULL),
           out_num_thread_metrics → spawned (ou 0))
```

Reporta `ac_thread_metric_t` por worker: `thread_id`, `seconds`,
`bytes_scanned = core_end - scan_start`, `matches_found = local.count`.

## Headline benchmark — sweep multi-regime (2026-05-22)

Ambiente: i5-1235U (2P+8E, 12 lógicas), Linux 6.17, `-O3 -march=native`,
`--warmup 1 --iters 3`. Quatro combinações (patterns × corpus) cobrindo o eixo
emission-bound ↔ DRAM-bound:

| Regime | `pthread_chunked_flat` MB/s | `pthread_chunked_v3_flat` MB/s | Δ |
|---|---:|---:|---:|
| **R1** Snort + SimpleWiki (low density) | **551,11** | 547,11 | −0,7 % (empata) |
| **R2** Snort + Enron (DRAM-bound) | 510,37 | **523,61** | **+2,6 %** ⭐ |
| **R3** Snort-100 + SimpleWiki (cache-friendly) | **1.292,08** | 951,98 | −26,3 % |
| **R4** ET-32 + SimpleWiki (autômato >> L3) | **165,73** | 150,03 | −9,5 % |

**Conclusão honesta:** `pthread_chunked_flat` continua sendo o melhor searcher
em 3 dos 4 regimes. `pthread_chunked_v3_flat` ganha apenas em R2 (Snort + Enron),
e o ganho é modesto (+2,6 %, não os +15 % reportados no primeiro sweep — que
era ruído térmico em uma janela fria de `chunked_flat`).

**Quando usar este searcher:** dicionário grande o suficiente para gerar
desbalanceamento entre P-cores e E-cores (Snort full ~56 MiB, ainda maior que
L3 mas com hot path coerente) **e** corpus de alta densidade de matches
(Enron-class) que mantém o DFA caminhando por estados densos. Fora desse nicho
o overhead de leitura de sysfs (`cpufreq`, `thread_siblings_list`) custa mais
do que entrega.

> **Histórico:** o primeiro sweep (apenas R2,
> [`scripts/run_flat_compositions_sweep.sh`](../../scripts/run_flat_compositions_sweep.sh))
> reportou +15,6 % sobre `chunked_flat`. O sweep multi-regime corrigiu esse viés
> via medições com cache quente reproduzíveis. O ganho real e estável é +2,6 %
> em R2; demais regimes empatam ou regridem.

Validação `make test` e `setarch -R make tsan` em `{1,2,3,4,7,8}` threads
contra o baseline `sequential` — passa em todos. Sweep multi-regime:
[`scripts/run_multiregime_flat_compositions.sh`](../../scripts/run_multiregime_flat_compositions.sh).

## Trade-offs e limitações

- **Mesmo HT_FACTOR de v3** (`100/100` = sem derate). Calibrado para
  o i5-1235U; outras famílias podem exigir 0.65–0.85.
- **Pesos por freq subestimam IPC** — mesma observação de v3: P-core
  tem IPC maior que E-core além da diferença de freq.
- **Apenas Linux**. `pthread_setaffinity_np` + sysfs em
  `/sys/devices/system/cpu` são Linux-específicos. Em outros POSIX, o
  searcher cai para a heurística degenerada (chunks iguais), o que é
  equivalente a `pthread_chunked_flat`.

## Tunables

| Macro                       | Default | Significado                                                                                   |
|-----------------------------|--------:|-----------------------------------------------------------------------------------------------|
| `V3F_CACHE_LINE`            | `64`    | Alinhamento de `worker_t` e arredondamento de chunks.                                         |
| `V3F_MAX_CPUS`              | `256`   | Capacidade dos vetores estáticos de topologia.                                                |
| `V3F_HT_FACTOR_NUM/DEN`     | `100/100` | Fator de peso para threads em par HT. 100/100 = sem derate.                                |

## Leitura relacionada

- [`pthread_chunked_v3.md`](pthread_chunked_v3.md) — base do v3 sem
  flat layout (mesmo chunking, emissão chain-walk).
- [`pthread_chunked_flat.md`](pthread_chunked_flat.md) — base do flat
  sem topology-awareness (mesmo emission, chunks iguais).
- [`../../../tcc_notes/sections/notes/conclusion.md`](../../../tcc_notes/sections/notes/conclusion.md) — veredito consolidado da composição F1.
- [`../architecture/parallelism.md`](../architecture/parallelism.md) —
  invariantes herdados (overlap, ownership, read-only automaton).
