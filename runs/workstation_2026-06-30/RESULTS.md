# Workstation (Ryzen 9 9950X) — resultados do sweep 2026-06-30 (headline)

Análise consolidada da corrida **completa ponta-a-ponta** A–E + C (cross-corpus)
+ G (granularidade) na workstation. Consultas token-efficient via
`sqlite3 runs/workstation_2026-06-30/sweep.db`; schema, views e consultas em
`runs/QUERY_GUIDE.md`.

> **Esta é a coleta canônica do TCC.** Corrida única, num único commit
> (`c19da78`), sem lacunas: 522 runs, **ok=522 · skip=0 · fail=0**, correctness
> 100%. Inclui `pthread_dynamic_flat` em todas as fases, a Fase C recuperada
> (`simplewiki` presente) e a Fase G nova. Substitui como headline o run
> interino de 2026-06-29 (que misturava commits e tinha a Fase C falha).

## Proveniência

| Campo        | Valor                                                              |
|--------------|-------------------------------------------------------------------|
| Data         | 2026-06-30 (−03), **4h10m** ponta-a-ponta                         |
| Máquina      | hostname `idealab-X870E-AORUS-MASTER` — AMD **Ryzen 9 9950X**, 16C/32T (Zen 5, homogêneo, sem P/E) |
| Cache        | L1d 768 KiB · L2 16 MiB (1 MiB×16) · **L3 64 MiB (2×32 MiB por CCD)** |
| Memória / SO | 123 GiB RAM · Ubuntu 24.04, kernel 6.17 · 1 nó NUMA               |
| Governador   | `performance`                                                     |
| Commit       | `c19da78` — commit único da corrida completa                     |
| Protocolo    | `warmup=2 iters=5`; fases A B C D E G; `MAX_T=32`                 |
| Volume       | 562 runs → **ok=522 · skip=0 · fail=0** (562 linhas no CSV)       |

## Saúde da corrida

- **Correctness: 100%.** `v_correctness` retorna `distinct_match_counts=1` nos
  4 cenários — todos os searchers paralelos concordam byte-a-byte com o
  `sequential`.
- **Zero falhas / zero skips.** Corrida limpa e homogênea (um único commit),
  ao contrário do run interino 06-29 (mistura `82dc697` + `334311e`, Fase C
  falha).
- **Sem throttling aparente:** frequências altas e sustentadas em
  `env/{start,end}.txt` sob `performance`; sem queda de regime.

## Mapa de leitura humana

Este documento substitui os antigos relatos de workstation. Use-o como a leitura
humana principal da coleta; use `runs/QUERY_GUIDE.md` quando precisar auditar
números no SQLite. A leitura por fase é:

| Fase | Pergunta que responde | Como interpretar |
|------|-----------------------|------------------|
| A | Qual searcher vence e como escala com threads? | Headline do TCC. Compare sempre contra `sequential` no mesmo cenário; T=16 separa núcleos físicos, T=32 inclui SMT. |
| B | O tamanho do autômato derruba throughput? | Isola o efeito de footprint/cache mantendo corpus fixo. É a evidência principal do regime memory-bound. |
| C | O conteúdo do corpus muda o resultado? | Controle metodológico: Enron e SimpleWiki próximos indicam que o custo vem da travessia do DFA/autômato, não da semântica do texto. |
| D | O trabalho ficou balanceado entre workers? | Diagnóstico por worker. Consulte `worker_metrics`/`v_worker_balance` para separar vazão total de ociosidade de barreira. |
| E | Construir o autômato paralelamente compensa? | Resultado separado da fase de busca; não misture tempo de build nos números de throughput. |
| G | A granularidade da fila dinâmica importa? | Mede overhead de scheduling para `dynamic*`; não substitui um corpus skewed para provar necessidade de dispatch dinâmico. |

Limites de interpretação: a coleta é **single-run por configuração**; portanto,
não use este run para afirmar variância entre corridas. Não há contadores PMU,
logo causas microarquiteturais além de footprint/cache são inferências, não
medidas diretas.

## Fase A — curvas de speedup (headline)

Cenários: `{snort 55,2 MiB, et_32 506,8 MiB}` × `{enron 1,32 GiB, enron_x8 10,6 GiB}`.
Baseline `sequential`: **snort 329,2 MB/s · et_32 209,8 MB/s**.

**Campeão: `pthread_dynamic_flat` @ T=32** (bag-of-tasks dinâmico + emissão pela
tabela achatada). Vence em 3 dos 4 cenários; no 4º (et_32/enron_x8) empata com
`pthread_chunked_flat` dentro de <0,3%.

| patterns | corpus  | pico (T=32)     | MB/s   | campeão               | vice (Δ)                  |
|----------|---------|-----------------|--------|-----------------------|---------------------------|
| snort    | enron   | **22,91×**      | 7 542  | dynamic_flat          | chunked_flat 22,48×       |
| snort    | enron_x8| **22,64×**      | 7 459  | dynamic_flat          | chunked_flat ~22,3×       |
| et_32    | enron   | **18,96×**      | 3 979  | dynamic_flat          | chunked_flat 18,83×       |
| et_32    | enron_x8| **19,83×**      | 4 183  | chunked_flat (empate) | dynamic_flat 19,79×       |

Ranking @T=32 (snort/enron): `dynamic_flat 22,91` > `chunked_flat 22,48` >
`chunked_v3_flat 22,18` > `dynamic 21,98` > `chunked_v2 21,57` ≈ `chunked_v3
21,46` ≈ `prefetch 21,34` > `chunked 21,02` > `2d_sharded 16,75` ≫
`pattern_sharded_prefix 1,78`.

Leituras principais:

1. **Emissão flat + escalonamento dinâmico é a combinação vencedora.** Os quatro
   primeiros lugares são todos `*_flat` ou `dynamic*`; a tabela achatada (idea 5)
   torna o custo por match desprezível e o dispatch dinâmico (bag of tasks)
   suaviza o desbalanceamento residual. Em cores homogêneos, `dynamic_flat`
   supera consistentemente o `chunked_flat` estático.
2. **Complexidade topology/freq-aware não paga em cores homogêneos:**
   `chunked_v3_flat` (afinidade + pesos por `cpufreq`) fica atrás do
   `chunked_flat` simples — em Zen 5 homogêneo os pesos por frequência viram
   overhead. (Contraste reservado à discussão P/E do i5 híbrido.)
3. **Sharding de dicionário é beco sem saída aqui:** `pattern_sharded_prefix`
   satura em ~1,8×; `pthread_2d_sharded_chunked` fica ~25% atrás. O paralelismo
   de dados (chunking do texto) domina.
4. **Escala limpa e monotônica** até o máximo — **todos os picos em T=32**, sem
   platô nem colapso.
5. **Cores físicos vs SMT** (dynamic_flat): a T=16 (1 thread/core) → **13,9×
   (snort) / 12,7× (et_32)**, eficiência 0,87 / 0,79. O SMT (16→32) **soma 1,65×
   (snort) / 1,49× (et_32)** — hyperthreading esconde latência de DRAM, mais no
   snort. Eficiência final a T=32: 0,72 / 0,59.
6. **Regime de cache manda no teto:** snort (55 MiB, ~L3) escala melhor (~23×)
   que et_32 (507 MiB ≫ L3, memory-bound) (~19×). Penalidade do cache blowout,
   confirmada.
7. **Robusto à escala do corpus:** `enron_x8` (10,6 GiB) reproduz a curva do
   enron (1,32 GiB), logo o resultado não é artefato de corpus pequeno.

## Fase B — footprint (tamanho do autômato × throughput)

`sequential` vs melhor searcher @ T=32, dicionários crescentes:

| dict        | autômato          | seq MB/s | T=32 MB/s | speedup |
|-------------|-------------------|----------|-----------|---------|
| snort_100   | 1,93 MiB (L2)     | 648      | 16 182    | ~25×    |
| snort       | 55,2 MiB (~L3)    | 329      | 7 594     | ~23×    |
| et_32       | 506,8 MiB (DRAM)  | 210      | 4 046     | ~19×    |

O throughput sequencial **desaba ~3×** (648→210 MB/s) conforme o autômato
transborda a hierarquia de cache, e a escala paralela cai de ~25× para ~19× (a
T=32 o throughput absoluto despenca ~4×, de 16 182 para ~4 000 MB/s). Gradiente
claro do memory wall — exatamente o regime-alvo da tese.

## Fase C — cross-corpus (sensibilidade ao corpus) ✅

`pthread_dynamic_flat`, snort, dois corpora de conteúdo distinto:

| corpus     | seq MB/s | T=1 MB/s | T=32 MB/s |
|------------|----------|----------|-----------|
| enron      | 329      | 343      | 7 595     |
| simplewiki | 342      | 356      | 7 976     |

O throughput é **insensível ao conteúdo do corpus** — `simplewiki` é até um
pouco mais rápido que `enron`, tanto sequencial quanto a T=32. O custo é
dominado pela travessia do DFA e pela hierarquia de memória do autômato, não
pela distribuição estatística do texto. (No run interino 06-29 esta fase falhava
por ausência de `simplewiki.txt`; aqui está recuperada.)

## Fase D — balanceamento por thread (T=32, snort/enron)

Balanceamento **excelente**: `pthread_dynamic_flat` entrega 7 577 MB/s com
tempos por-worker 177,8–180,9 ms (**cv 0,96%**). O dispatch dinâmico mantém os
32 workers dentro de ~2% um do outro apesar da densidade de matches variar entre
threads; a emissão flat é barata → **tempo de parede independente da densidade
de matches**. Corrobora a Fase A (`dynamic_flat` 7 577 › `chunked_flat` 7 383 ›
`dynamic` 7 233).

## Fase E — build paralelo (idea 4)

Speedup da **construção** do autômato vs build sequencial:

| dict      | melhor speedup | em T | a T=32 |
|-----------|----------------|------|--------|
| et_32     | **1,59×**      | 16   | 1,47×  |
| snort     | 1,17×          | 4    | 0,57×  |
| snort_1k  | 0,95×          | 4    | 0,33×  |
| snort_100 | —              | —    | 0,21×  |

Só compensa em **dicionário grande** (et_32) e **até ~16 threads**; para dicts
pequenos o overhead domina e regride abaixo de 1×. Idea 4 tem valor limitado e
específico ao regime de autômato grande.

## Fase G — granularidade (tokens por tarefa) ✅ NOVA

`pthread_dynamic_flat`, snort/enron, T=32, varredura de `tokens-per-task`:

| tpt    | T=32 MB/s | cv   |
|--------|-----------|------|
| 001    | 7 414     | 0,3  |
| 004    | 7 601     | 0,2  |
| 016    | 7 664     | 0,1  |
| 064    | 7 756     | 0,1  |
| 256    | **7 804** | 0,0  |

Granularidade **mais grossa é melhor**: o throughput cresce monotonicamente de
tpt001→tpt256 (~5%). Tarefas maiores amortizam a contenção no contador atômico
do dispatch dinâmico e reduzem overhead de escalonamento, sem prejudicar o
balanceamento (cv permanece <0,3%). Não há evidência de over-subscription ruim
até tpt256.

## Consultas rápidas

```bash
DB=runs/workstation_2026-06-30/sweep.db
sqlite3 -header -column $DB "SELECT * FROM v_correctness;"                  # sanidade
sqlite3 -header -column $DB "SELECT * FROM v_best;"                          # melhor searcher por (patterns,corpus,T)
sqlite3 -header -column $DB "SELECT thr,speedup_vs_seq,mbps FROM v_speedup \
  WHERE searcher='pthread_dynamic_flat' AND corpus='enron_corpus' \
  AND patterns='patterns_snort' ORDER BY thr;"                              # curva do campeão
sqlite3 -header -column $DB "SELECT * FROM v_footprint;"                     # fase B
sqlite3 -header -column $DB "SELECT * FROM v_build;"                         # fase E
sqlite3 -header -column $DB "SELECT tag,mbps,cv_pct FROM runs \
  WHERE phase='G_granularity' AND searcher='pthread_dynamic_flat' \
  AND threads_label NOT LIKE '%perthread%' ORDER BY tag;"                   # fase G
```
