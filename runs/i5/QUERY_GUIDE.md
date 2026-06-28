# Sweep do i5 — guia de consulta (token-efficient para LLMs)

Dados do sweep de 2026-05-29 (commit `e3dc7d4`, i5-1235U, 12 threads HW,
governor=performance, 265 runs, 7h29m, 0 falhas). Correctness validada:
match-count único por `(patterns, corpus)` em todos os searchers.

## Forma mais prática de consultar

**Prefira o SQLite, não os CSV.** Rode SELECTs e receba só o que precisa:

```bash
sqlite3 -header -column runs/i5/sweep.db "SELECT ... "
sqlite3 -header -csv    runs/i5/sweep.db "SELECT ... "   # p/ colar em planilha
```

Nunca faça `cat` dos `.log` (são grandes) nem leia o CSV inteiro no contexto —
use as views abaixo, que já agregam o que importa.

## Artefatos

| Arquivo                       | O que é                                            |
|-------------------------------|----------------------------------------------------|
| `sweep.db`                    | SQLite tipado: tabela `runs` + 6 views. **Use isto.** |
| `sweep.csv`                   | Mesmos 265 runs, achatados (1 linha/run).          |
| `csv/<phase>.csv`             | Um CSV por fase (A/B/C/D/E).                        |
| `MASTER.log`                  | Event log do sweep (RUN/OK/SKIP/FAIL timestampado).|
| `env/{start,end}.txt`         | Snapshot de ambiente (governor, lscpu, térmico).   |
| `env/thermal.tsv`             | Térmico + MHz médio por run (correlação throttling).|

## Tabela `runs` (1 linha por execução)

Identificação: `phase, patterns, corpus, searcher, thr, tag`
Tempos (ms): `min_ms, mean_ms, median_ms, max_ms, cv_pct`
Throughput: `mbps, gbps`  •  Volume: `bytes, matches`  •  Workers reais: `wrk`
Autômato: `pattern_count, automaton_states, automaton_kib`  •  Build: `build_ms`
Build paralelo (fase E): `build_threads`  •  Protocolo: `warmup, iters`
Proveniência: `log_file`

Regime: `automaton_kib`/1024 = MiB. L3 = 12 MB. `patterns_et_32` ≈ 507 MiB
(≫ L3, memory-bound) vs `patterns_snort` ≈ 55 MiB.

## Views (já é onde mora a análise)

| View             | Para responder                                              |
|------------------|-------------------------------------------------------------|
| `v_speedup`      | Speedup vs baseline `sequential` (figura headline, fase A). Cols: `speedup_vs_seq, mbps, seq_mbps`. |
| `v_self_speedup` | Escalabilidade: `mbps(T)/mbps(T=1)` do mesmo searcher.       |
| `v_footprint`    | Fase B: `automaton_mib` × `mbps` (throughput vs tamanho).    |
| `v_build`        | Fase E: `build_speedup` paralelo vs sequencial (idea 4).    |
| `v_best`         | Melhor searcher (mbps) por `(patterns,corpus,thr)`.         |
| `v_correctness`  | `distinct_match_counts` por `(patterns,corpus)` (=1 → ok).  |

## Consultas canônicas

```sql
-- Curva de speedup de um searcher (snort × enron_corpus)
SELECT thr, speedup_vs_seq FROM v_speedup
WHERE patterns='patterns_snort' AND corpus='enron_corpus'
  AND searcher='pthread_dynamic' ORDER BY thr;

-- Pico de speedup por cenário (qual searcher vence e em que T)
SELECT patterns, corpus, searcher, thr, speedup_vs_seq FROM v_speedup w
WHERE speedup_vs_seq=(SELECT MAX(speedup_vs_seq) FROM v_speedup x
  WHERE x.patterns=w.patterns AND x.corpus=w.corpus)
ORDER BY patterns, corpus;

-- Footprint vs throughput em T=12 (cache blowout)
SELECT patterns, automaton_mib, searcher, mbps FROM v_footprint
WHERE thr=12 ORDER BY automaton_mib;

-- Speedup do build paralelo (idea 4)
SELECT patterns, build_threads, build_speedup FROM v_build ORDER BY patterns, build_threads;

-- Sensibilidade ao corpus (fase C): seq vs v3 em simplewiki vs enron
SELECT corpus, searcher, thr, round(mbps,2) mbps FROM runs
WHERE phase='C_cross_corpus' ORDER BY corpus, searcher, thr;

-- Balanceamento per-thread (fase D) — searcher escolhido
SELECT searcher, mbps, wrk FROM runs WHERE phase='D_per_thread';
```

## Reproduzir os artefatos

```bash
python3 scripts/extract_sweep_csv.py runs/i5 -o runs/i5/sweep.csv --known-only
python3 scripts/build_sweep_db.py    runs/i5/sweep.csv -o runs/i5/sweep.db
```
