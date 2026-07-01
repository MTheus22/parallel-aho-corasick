# Task 03 — Ingestão + análise do spread por worker (mediana + IQR)

## Objetivo

Transformar os logs de `H_skew/` numa conclusão quantitativa: **spread de tempo
por worker / ociosidade de barreira**, com **mediana + IQR sobre as réplicas**,
comparando estático × dinâmico nos corpora uniforme × clustered. Produzir a
tabela que valida (ou falsifica) o mecanismo.

## Escopo

- **In scope:** `scripts/analyze_skew.py` (novo); opcional view `v_skew` em
  `scripts/build_sweep_db.py`; usa o `sweep.db` já gerado por
  `extract_sweep_csv.py`/`build_sweep_db.py` (que **auto-descobrem** `H_skew/`).
- **Out of scope:** gerar corpus (Task 01), rodar sweep (Task 02), escrever o
  texto (Tasks 05/06).

## Implementação

1. **Vazão agregada (via `sweep.db`):** confirme que `H_skew/` foi ingerido
   (`extract_sweep_csv.py` faz `os.listdir(run_dir)` → pega qualquer pasta de
   fase). Opcional: adicione ao `build_sweep_db.py` uma view
   ```sql
   CREATE VIEW v_skew AS
     SELECT patterns, corpus, searcher, thr,
            round(median(mbps),2) AS mbps_med   -- ou agregue no Python
     FROM runs WHERE phase = 'H_skew'
     GROUP BY patterns, corpus, searcher, thr;
   ```
   (SQLite não tem `median` nativo → se preferir, faça a agregação no Python e
   deixe o `sweep.db` só com as linhas cruas.)
2. **Spread por worker (`scripts/analyze_skew.py`):** as linhas per-thread
   `[tNN]   <ms> ms  <bytes> bytes  <matches> matches  <mbps> MB/s`
   **não** entram no CSV — parseie-as direto dos `.log` de `H_skew/`. Para cada
   log (uma réplica de uma config):
   - Colete `seconds[t]` de cada worker (o `ms` da linha `[tNN]`).
   - Calcule: `t_max`, `t_min`, `t_mean`; **spread** = `(t_max - t_min)/t_mean`;
     **ociosidade de barreira** = `(t_max - t_mean)/t_max` (fração do makespan
     perdida esperando o straggler); **cv** = `stdev/t_mean`.
3. **Agregação sobre réplicas:** agrupe por `(patterns, corpus, searcher)` e
   reporte **mediana + IQR (p25–p75)** de spread e de ociosidade de barreira
   sobre as REPS réplicas — nunca a média das iterations intra-processo.
4. **Tabela de saída** (stdout + `runs/<dir>/H_skew/skew_analysis.md`): colunas
   `patterns | corpus | searcher | mbps_med | spread_med [IQR] | barrier_idle_med
   [IQR]`. Ordene para deixar o contraste óbvio (uniform vs clustered lado a
   lado por searcher).
5. **Paridade (sanidade de Ródenas):** cheque e imprima que
   `matches(uniform) == matches(clustered)` e bytes idênticos (dos headers/linha
   de resultado) — se não baterem, marque o resultado como **inválido**.
6. **Veredito automático:** imprima a leitura esperada e se ela se sustenta:
   - no **clustered**, `spread(estático) ≫ spread(dinâmico)`;
   - no **uniform**, spreads ~iguais;
   - `mbps_med(dinâmico) > mbps_med(estático)` no clustered.
   Se **não** se sustentar (efeito fraco por flat barato), diga isso claramente —
   é resultado negativo válido, não erro.

## Validação

```bash
python3 scripts/extract_sweep_csv.py runs/workstation_skew   # gera sweep.csv (inclui H_skew)
python3 scripts/build_sweep_db.py    runs/workstation_skew   # gera sweep.db
python3 scripts/analyze_skew.py      runs/workstation_skew   # tabela spread/barrier idle
sqlite3 -header -column runs/workstation_skew/sweep.db \
  "SELECT * FROM runs WHERE phase='H_skew' LIMIT 5;"
```

## Critérios de Aceite

- `scripts/analyze_skew.py` roda sobre `runs/<dir>` e emite a tabela `patterns |
  corpus | searcher | mbps_med | spread_med [IQR] | barrier_idle_med [IQR]`.
- Métricas agregadas por **mediana + IQR sobre ≥5 réplicas** (não média de
  iters).
- A checagem de paridade byte/match uniform×clustered é reportada; divergência
  ⇒ resultado marcado inválido.
- O script imprime um **veredito explícito** (mecanismo confirmado / não
  confirmado), sem forçar narrativa.
- `runs/<dir>/H_skew/skew_analysis.md` fica salvo para as Tasks 05/06 citarem.
