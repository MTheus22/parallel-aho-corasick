# Workstation (Ryzen 9 9950X) — resultados do sweep 2026-06-29

Análise consolidada da corrida A–E na workstation. Consultas token-efficient
via `sqlite3 runs/workstation_2026-06-29/sweep.db` (schema/views idênticos
ao guia em `runs/i5/QUERY_GUIDE.md`).

## Proveniência

| Campo        | Valor                                                              |
|--------------|-------------------------------------------------------------------|
| Data         | 2026-06-29 16:53 → 20:37 (−03), **3h44m**                          |
| Máquina      | hostname `idealab-X870E-AORUS-MASTER` — AMD **Ryzen 9 9950X**, 16C/32T (Zen 5, homogêneo, sem P/E) |
| Cache        | L1d 768 KiB · L2 16 MiB (1 MiB×16) · **L3 64 MiB (2×32 MiB por CCD)** |
| Memória / SO | 123 GiB RAM · Ubuntu 24.04, kernel 6.17 · 1 nó NUMA               |
| Governador   | `performance` (5,4–5,7 GHz nos cores ativos em start e end)        |
| Commit       | `82dc697` (epic-03, harness Fase 1) — estado **de ontem**         |
| Protocolo    | `warmup=2 iters=5`; fases A B C D E; `MAX_T=32`                    |
| Volume       | 464 runs → **ok=444 · skip=0 · fail=3**                            |

> ⚠️ **Não é headline final do TCC ainda.** Este commit **não** inclui
> `pthread_dynamic_flat` na fase A (candidato a campeão em cores homogêneos,
> por CLAUDE.md) **nem** a fase G (granularidade). As mudanças de hoje que
> adicionam o `dynamic_flat` à fase A **não** estão medidas aqui. Além disso,
> a Fase C falhou (ver abaixo), então pelo próprio critério do projeto
> ("promover só sem `.FAIL`") esta corrida ainda não passa limpa — embora
> A/B/D/E estejam íntegras e sem falhas.

## Saúde da corrida

- **Correctness: 100%.** `v_correctness` retorna `distinct_match_counts=1` nos
  4 cenários — os 9 searchers paralelos concordam byte-a-byte com o
  `sequential`. Contagens: snort/enron 17.853.987; et_32/enron 58.367.984
  (e ×8 para o corpus `enron_x8`).
- **3 falhas, todas na Fase C:** corpus `data/simplewiki.txt` ausente
  → `patterns_snort__simplewiki__{sequential,pthread_chunked_v3×T1,×T32}`.
  **A comparação cross-corpus não aconteceu** (só sobrou o braço enron).
- **Sem throttling aparente:** `thermal_zone` indisponível nesta placa, mas as
  frequências continuam altas em `env/end.txt`; nenhuma queda de regime.

## Fase A — curvas de speedup (headline)

Cenários: `{snort 55,2 MiB, et_32 506,8 MiB}` × `{enron 1,32 GiB, enron_x8 10,6 GiB}`.
Baseline `sequential`: **snort 330,7 MB/s · et_32 210,0 MB/s**. Cobertura:
enron = 18 pontos (T=0,1,2…32); enron_x8 = 6 pontos (T=0,1,4,8,16,32).

**Campeão em TODOS os 4 cenários: `pthread_chunked_flat` @ T=32.**

| patterns | corpus  | pico (T=32) | MB/s   | vice (Δ)                       |
|----------|---------|-------------|--------|--------------------------------|
| snort    | enron   | **22,37×**  | 7 399  | chunked_v3_flat 22,35×         |
| snort    | enron_x8| **22,33×**  | 7 330  | chunked_v3_flat 22,17×         |
| et_32    | enron   | **18,95×**  | 3 980  | chunked_v3_flat 18,85×         |
| et_32    | enron_x8| **19,51×**  | 4 177  | chunked_v3_flat 19,30×         |

Ranking de searchers (pico, snort/enron): `chunked_flat 22,37` ≳
`chunked_v3_flat 22,35` > `dynamic 21,86` > `chunked_v3 21,42` ≈
`chunked_v2 21,33` ≈ `prefetch 21,11` ≈ `chunked 20,95` > `2d_sharded 16,63`
≫ `pattern_sharded_prefix 1,87`.

Leituras principais:

1. **A tabela achatada (idea 5) é o diferencial:** as duas variantes `*_flat`
   lideram em todo cenário. Emissão pela flat output table torna o custo por
   match desprezível.
2. **Complexidade topology/freq-aware não paga em cores homogêneos:**
   `chunked_flat` (simples) supera `chunked_v3_flat` (afinidade + pesos por
   `cpufreq`) — em Zen 5 homogêneo, os pesos por frequência viram overhead.
   (Contraste esperado com o i5 híbrido, reservado à discussão P/E.)
3. **Sharding de dicionário é beco sem saída aqui:** `pattern_sharded_prefix`
   satura em ~1,9× (snort) / 2,6× (et_32); `pthread_2d_sharded_chunked` fica
   ~25% atrás do chunking puro. O paralelismo de dados (chunking do texto)
   domina.
4. **Escala limpa e monotônica** até o máximo — **todos os picos em T=32**, sem
   platô nem colapso (contraste forte com o i5, que saturava/oscilava em T=12).
5. **Cores físicos vs SMT:** a T=16 (1 thread/core) → **12,5× (et_32) /
   13,7× (snort)**, eficiência 0,78/0,85. O SMT (16→32) **soma 1,51× (et_32) /
   1,64× (snort)** — hyperthreading esconde latência de DRAM, mais no snort.
   Eficiência final a T=32: 0,59 / 0,70.
6. **Regime de cache manda no teto:** snort (55 MiB, ~L3) escala melhor
   (22×) que et_32 (507 MiB ≫ L3, memory-bound) (19×). Penalidade do cache
   blowout, confirmada.
7. **Resultado robusto à escala:** `enron_x8` (10,6 GiB) reproduz a curva do
   enron (1,32 GiB) — 19,5×/22,3× — logo não é artefato de corpus pequeno.

## Fase B — footprint (tamanho do autômato × throughput)

`sequential` vs `pthread_chunked_flat` @ T=32, dicionários crescentes:

| dict        | autômato   | seq MB/s | T=32 MB/s | speedup |
|-------------|------------|----------|-----------|---------|
| snort_100   | 1,93 MiB (L2) | 646   | 16 126    | ~25×    |
| snort_1k    | 11,9 MiB   | 451      | 9 916     | ~22×    |
| snort       | 55,2 MiB   | 327      | 7 445     | ~22,7×  |
| et_32       | 506,8 MiB (DRAM) | 210 | 3 972     | ~18,9×  |

O throughput sequencial **desaba ~3×** (646→210 MB/s) conforme o autômato
transborda a hierarquia de cache, e a escala paralela degrada de ~25× para
~19×. Gradiente claro do memory wall — exatamente o regime-alvo da tese.

## Fase C — cross-corpus ❌ INCOMPLETA

`simplewiki.txt` ausente derrubou os 3 runs do braço wiki. Sobrou só
`snort/enron`: seq 330 → `chunked_v3` T=32 = 7 140 MB/s (21,6×), redundante com
a Fase A. **Sem comparação de sensibilidade a corpus nesta corrida** —
recuperar rodando `prepare_data.sh` antes de reexecutar.

## Fase D — balanceamento por thread (T=32, snort/enron)

Balanceamento **excelente**: por-worker 145–160 ms (cv < 1,3%) apesar da
densidade de matches variar ~4× entre threads (414 k–1 052 k). O custo é
dominado pela travessia do DFA; a emissão flat é barata → **tempo de parede
independente da densidade de matches**. Ranking agregado corrobora a Fase A
(`chunked_flat`/`v3_flat` ~7 405 › `dynamic` 7 228 › v2/v3/prefetch ~7 000–7 119
› `chunked` 6 958 › `2d` 5 496 › `pattern_sharded` 589).

## Fase E — build paralelo (idea 4)

Speedup da **construção** do autômato vs build sequencial:

| dict      | melhor speedup | em T | a T=32 |
|-----------|----------------|------|--------|
| et_32     | **1,54×**      | 16   | 1,43×  |
| snort     | 1,14×          | 4    | 0,60×  |
| snort_1k  | 0,98×          | 4    | 0,32×  |
| snort_100 | —              | —    | 0,26×  |

Só compensa em **dicionário grande** (et_32) e **até ~16 threads**; para dicts
pequenos o overhead domina e regride. Idea 4 tem valor limitado e específico.

## O que falta / próximos passos

- [ ] **Reexecutar com o commit de hoje** (inclui `pthread_dynamic_flat` na
      fase A + fase G) antes de fixar headline — este run é do estado anterior.
- [ ] **Recuperar a Fase C**: gerar `data/simplewiki.txt` (`prepare_data.sh`)
      e refazer o cross-corpus.
- [ ] Confirmar se `chunked_flat` continua campeão quando `dynamic_flat`
      entra na disputa (CLAUDE.md aposta no `dynamic_flat` em cores homogêneos).
- [ ] i5-1235U fica reservado à discussão **P/E** (não é mais canônico).

## Consultas rápidas

```bash
DB=runs/workstation_2026-06-29/sweep.db
sqlite3 -header -column $DB "SELECT * FROM v_correctness;"                 # sanidade
sqlite3 -header -column $DB "SELECT * FROM v_best;"                         # melhor searcher por (patterns,corpus,T)
sqlite3 -header -column $DB "SELECT thr,speedup_vs_seq,mbps FROM v_speedup \
  WHERE searcher='pthread_chunked_flat' AND corpus='enron_corpus' \
  AND patterns='patterns_snort' ORDER BY thr;"                             # curva do campeão
sqlite3 -header -column $DB "SELECT * FROM v_footprint;"                    # fase B
sqlite3 -header -column $DB "SELECT * FROM v_build;"                        # fase E
```
