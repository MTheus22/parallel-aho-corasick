# Run i5_2026-07-02 — piloto R (curvas replicadas) + H (corpus skew)

> **STUB de instruções.** Este arquivo foi escrito ANTES da coleta. Agente:
> após o sweep terminar, substitua este stub pela análise, preservando a
> seção "Limitações" abaixo.

## O que é este run

Piloto no **i5-1235U** (2 P-cores/4,4 GHz com HT = cpu 0–3; 8 E-cores/3,3 GHz
= cpu 4–11; L3 12 MiB), working tree sobre o commit `68faeb9` (com mudanças
não-commitadas: fases R/H, `cpu=` por worker). Fases:

- **R_replicated** — snort×enron, searchers {seq, v2, v3, chunked_flat,
  v3_flat, dynamic, dynamic_flat}, T∈{1,2,4,8,12}, **5 processos independentes
  por config** (tags `rep01..rep05`, réplicas intercaladas), 1 warmup + 3 iters.
- **H_skew** — {v2, v3, chunked_flat, dynamic, dynamic_flat} × {snort, et_32}
  × {enron_skew_uniform, enron_skew_clustered}, T=12, 5 réplicas, tudo
  `--per-thread` (tabela `workers`, coluna nova `cpu`).

Corpora skew: 4 GiB cada, **paridade exata** (mesmos blocos, ordem diferente):
4 294 967 296 bytes e 94 304 128 matches em ambos.

## Como analisar

Dados: `sweep.db` (gerado pelo próprio `run_all`; se faltar:
`python3 scripts/extract_sweep_csv.py runs/i5_2026-07-02 -o runs/i5_2026-07-02/sweep.csv
&& python3 scripts/build_sweep_db.py runs/i5_2026-07-02/sweep.csv`).
Schema/views: `runs/QUERY_GUIDE.md`. Spec completa da análise:
`specs/epic-04/task-03.md`.

1. **Sanidade:** `SELECT COUNT(*) FROM runs` (esperado: R=155 + H=100, 0 FAIL);
   paridade de `matches_found` entre os dois corpora skew em todos os searchers.
2. **Fase R — dispersão entre processos:** por (searcher, thr), mediana + IQR +
   min–max de `mbps` sobre as 5 reps (`tag LIKE 'rep%'`). Compare a dispersão
   **entre réplicas** com o `cv_pct` **intra-processo** — a tese do run é
   σ²_between ≫ σ²_within no i5 saturado. Curvas de speedup: use a mediana do
   `sequential` como baseline (nunca uma rep isolada).
3. **Fase H — o contraste central:** para cada searcher, uniform vs clustered:
   (a) delta de throughput; (b) spread por worker (tabela `workers`: max−min de
   `seconds` por run, e/ou CV), com classe do core via `cpu` (0–3=P, 4–11=E).
   Perguntas: o estático (v2/chunked_flat) degrada mais no clustered que o
   dinâmico? O v3 (corrige a MÁQUINA, cego ao CONTEÚDO) fica entre os dois?
   O spread do dinâmico permanece baixo nos dois corpora?
4. **Explicar com `cpu=`:** réplicas lentas correlacionam com workers em
   E-core / dois workers no mesmo P-core físico?

## Limitações (manter na análise final)

- **Confundido por P/E**: skew de conteúdo + heterogeneidade de cores agem
  juntos; o contraste válido é *within-machine* (uniform×clustered, confundidor
  ~constante). **Não substitui a coleta canônica na workstation homogênea.**
- **Não-canônico**: nada daqui vira headline do TCC nem entra em
  `partes/*.tex` sem decisão explícita do autor; runs canônicos =
  `runs/workstation_2026-06-30/` e `runs/i5/` (ver `runs/MANIFEST.md`).
- Verificar o governador em `env/start.txt` (o run_all tenta `performance`
  via sudo) e anotar na análise.
- `cpu` é amostrado no FIM do scan de cada worker — migrações no meio do laço
  não aparecem.
