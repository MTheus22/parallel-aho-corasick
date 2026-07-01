# Guia de Consulta dos Sweeps

Guia unico para consultar qualquer `sweep.db` gerado por
`scripts/build_sweep_db.py`. Use este arquivo para a workstation canonica, para o
i5 P/E e para novos runs.

## Escolha do Banco

| Banco | Uso correto |
|-------|-------------|
| `runs/workstation_2026-06-30/sweep.db` | Fonte canonica do TCC: Ryzen 9 9950X, fases A B C D E G, commit `c19da78`. |
| `runs/i5/sweep.db` | Evidencia historica para secao P/E no i5-1235U, principalmente `pthread_chunked_v3` e `pthread_chunked_v3_flat`. |

```bash
DB=runs/workstation_2026-06-30/sweep.db
sqlite3 -header -column "$DB" "SELECT ...;"
sqlite3 -header -csv    "$DB" "SELECT ...;"   # para planilha
```

Prefira SQLite ao CSV. O CSV tem uma linha por run e e util para exportacao, mas
o DB ja tipa colunas, cria views e inclui metricas por worker quando existirem.

## Artefatos Esperados

| Arquivo | O que e |
|---------|---------|
| `sweep.db` | SQLite tipado: tabelas `runs` e `worker_metrics` + views. Use isto. |
| `sweep.csv` | Uma linha por execucao/configuracao. |
| `MASTER.log` | Log de orquestracao do sweep. |
| `env/{start,end}.txt` | Ambiente: CPU, governor, memoria, commit. |
| `env/thermal.tsv` | Telemetria termica quando disponivel. |
| `RESULTS.md` | Analise promovida do run, quando houver. |

## Tabela `runs`

Uma linha por execucao/configuracao.

Identificacao: `phase, patterns, corpus, searcher, thr, tag, log_file`.
Tempos: `min_ms, mean_ms, median_ms, max_ms, cv_pct`.
Throughput: `mbps, gbps`.
Volume: `bytes, matches`.
Automato: `pattern_count, automaton_states, automaton_kib`.
Protocolo: `warmup, iters, wrk`.
Build paralelo: `build_ms, build_threads`.

## Tabela `worker_metrics`

Uma linha por worker, preenchida apenas para logs emitidos com `--per-thread`.

Colunas principais: `phase, patterns, corpus, searcher, thr, tag, worker_id,
milliseconds, seconds, bytes_scanned, matches_found, mbps, log_file`.

Use para diagnosticar stragglers, balanceamento e efeitos P/E. A tabela existe
para todos os bancos reconstruidos pelo pipeline novo; pode estar vazia em runs
sem `--per-thread`.

## Views

| View | Para responder |
|------|----------------|
| `v_speedup` | Speedup vs baseline `sequential` na fase A. |
| `v_self_speedup` | Escalabilidade do proprio searcher: `mbps(T)/mbps(T=1)`. |
| `v_footprint` | Fase B: tamanho do automato vs throughput. |
| `v_build` | Fase E: build paralelo vs build sequencial. |
| `v_best` | Melhor searcher por `(patterns, corpus, thr)`. |
| `v_correctness` | Sanidade: match-count unico por `(patterns, corpus)`. |
| `v_worker_balance` | Agregado por-worker: min/avg/max, `spread_pct`, `imbalance_ratio`. |

## Consultas Canonicas

```sql
-- Sanidade: todos os cenarios devem ter distinct_match_counts = 1.
SELECT * FROM v_correctness;

-- Curva de speedup de um searcher.
SELECT thr, speedup_vs_seq, mbps
FROM v_speedup
WHERE patterns='patterns_snort'
  AND corpus='enron_corpus'
  AND searcher='pthread_dynamic_flat'
ORDER BY thr;

-- Melhor variante por cenario e T.
SELECT * FROM v_best
WHERE patterns='patterns_snort'
  AND corpus='enron_corpus'
ORDER BY thr;

-- Footprint/cache cliff.
SELECT patterns, searcher, thr, automaton_mib, mbps
FROM v_footprint
ORDER BY automaton_mib, searcher, thr;

-- Build paralelo.
SELECT patterns, build_threads, build_ms, seq_build_ms, build_speedup
FROM v_build
ORDER BY patterns, build_threads;

-- Balanceamento agregado por worker.
SELECT phase, searcher, thr, tag, workers, min_worker_ms, max_worker_ms,
       spread_pct, imbalance_ratio
FROM v_worker_balance
ORDER BY phase, searcher, tag;

-- Linhas cruas por worker para uma variante.
SELECT worker_id, milliseconds, bytes_scanned, matches_found, mbps
FROM worker_metrics
WHERE phase='D_per_thread'
  AND searcher='pthread_chunked_v3_flat'
ORDER BY worker_id;
```

## Consultas P/E do i5

Use somente com `DB=runs/i5/sweep.db`.

```sql
-- Efeito topology-aware: v3 deve reduzir stragglers vs v2.
SELECT searcher, workers, min_worker_ms, max_worker_ms,
       spread_pct, imbalance_ratio
FROM v_worker_balance
WHERE phase='D_per_thread'
  AND searcher IN ('pthread_chunked_v2',
                   'pthread_chunked_v3',
                   'pthread_chunked_v3_flat')
ORDER BY searcher;

-- Curva principal das variantes de interesse P/E.
SELECT searcher, thr, mbps, speedup_vs_seq
FROM v_speedup
WHERE patterns='patterns_snort'
  AND corpus='enron_corpus'
  AND searcher IN ('pthread_chunked_v2',
                   'pthread_chunked_v3',
                   'pthread_chunked_v3_flat')
ORDER BY searcher, thr;
```

## Recriar Artefatos

```bash
RUN_DIR=runs/workstation_2026-06-30
python3 scripts/extract_sweep_csv.py "$RUN_DIR" -o "$RUN_DIR/sweep.csv" --known-only
python3 scripts/build_sweep_db.py "$RUN_DIR/sweep.csv" -o "$RUN_DIR/sweep.db"
```

`build_sweep_db.py` escreve primeiro em um arquivo temporario e so substitui o
DB final depois de concluir a construcao. Se a reconstrucao falhar, o DB antigo
permanece intacto.
