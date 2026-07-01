# Plano: Painel Streamlit dos Sweeps

Objetivo: implementar um painel interativo para leitura humana dos resultados de
benchmark, usando os `sweep.db` preservados como fonte primária. O painel deve
permitir explorar curvas, rankings, métricas, balanceamento por worker e
diferenças entre corpora/searchers/threads sem gerar uma imagem estática por
gráfico.

## Escopo

Construir um app Streamlit local, reutilizável e leve, orientado a análise
visual. O app deve ler diretamente os bancos SQLite existentes:

| Run | Banco | Uso no painel |
|-----|-------|---------------|
| Workstation canônica | `runs/workstation_2026-06-30/sweep.db` | Fonte principal: fases A B C D E G, Ryzen 9 9950X, headline do TCC. |
| i5 P/E | `runs/i5/sweep.db` | Comparação seletiva de arquitetura híbrida P/E, principalmente `pthread_chunked_v3` e `pthread_chunked_v3_flat`. |

Referências obrigatórias antes de implementar:

- `runs/QUERY_GUIDE.md` para schema, views e queries canônicas.
- `runs/MANIFEST.md` para política de uso dos runs.
- `runs/workstation_2026-06-30/RESULTS.md` para leitura humana da coleta
  canônica.

Não recriar análise textual extensa dentro do app. O painel deve mostrar dados,
gráficos, tabelas e pequenos avisos contextuais.

## Não Escopo

- Não sobrescrever `sweep.db`, `sweep.csv` ou logs.
- Não gerar PNGs/JPEGs como fluxo principal.
- Não transformar o app em fonte canônica nova. A fonte continua sendo SQLite +
  docs existentes.
- Não misturar i5 com workstation como ranking de máquina. O i5 é apenas
  evidência P/E.
- Não implementar aquisição/reexecução de benchmarks dentro do painel.

## Local Sugerido

Criar uma pasta autocontida:

```text
dashboard/
  app.py
  data.py
  charts.py
  ui.py
  README.md
requirements-dashboard.txt
```

Comando alvo:

```bash
python3 -m pip install -r requirements-dashboard.txt
streamlit run dashboard/app.py
```

Dependências recomendadas:

```text
streamlit
pandas
plotly
```

Opcional: `numpy`, somente se simplificar agregações. Evitar dependências
pesadas ou bibliotecas que exijam build nativo.

## Modelo de Dados no App

Carregar cada banco com uma coluna derivada `run_id`:

| run_id | db_path | label curto |
|--------|---------|-------------|
| `workstation_2026-06-30` | `runs/workstation_2026-06-30/sweep.db` | Workstation 2026-06-30 |
| `i5` | `runs/i5/sweep.db` | i5 P/E |

Funções esperadas em `dashboard/data.py`:

- `discover_databases(root: Path) -> list[RunDb]`
- `load_table(db_path, table_or_view) -> pandas.DataFrame`
- `load_runs(selected_run_ids) -> pandas.DataFrame`
- `load_view(selected_run_ids, view_name) -> pandas.DataFrame`
- `load_worker_metrics(selected_run_ids) -> pandas.DataFrame`
- `load_worker_balance(selected_run_ids) -> pandas.DataFrame`
- `get_filter_options(df) -> dict`

Use `st.cache_data` para queries e `st.cache_resource` apenas se mantiver
conexões SQLite abertas. Para simplicidade e segurança, pode abrir conexão por
query em modo read-only:

```python
sqlite3.connect(f"file:{db_path}?mode=ro", uri=True)
```

Tabelas/views que o app deve suportar:

- `runs`
- `worker_metrics`
- `v_speedup`
- `v_self_speedup`
- `v_footprint`
- `v_build`
- `v_best`
- `v_correctness`
- `v_worker_balance`

O app deve funcionar mesmo quando uma view existe mas retorna zero linhas.

## Filtros Globais

Barra lateral com filtros persistentes:

- `run_id`: multi-select, default `workstation_2026-06-30`.
- `phase`: multi-select, default conforme página.
- `patterns`: multi-select.
- `corpus`: multi-select.
- `searcher`: multi-select com busca.
- `thr`: range slider ou multi-select.
- `metric`: `mbps`, `gbps`, `speedup_vs_seq`, `speedup_vs_t1`, `median_ms`,
  `cv_pct`.
- Toggle `exibir sequential/sequential_flat`.
- Toggle `normalizar por sequential`.
- Toggle `mostrar i5 P/E` com aviso quando ativo.

Os filtros devem ser aplicados consistentemente às tabelas e aos gráficos da
página atual.

## Páginas do Painel

### 1. Visão Geral

Objetivo: responder rapidamente o que existe e se os dados estão saudáveis.

Componentes:

- Cards por run: número de linhas em `runs`, fases disponíveis, searchers,
  corpora, patterns, threads mínimo/máximo.
- Card de sanidade: `v_correctness`, destacando `distinct_match_counts`.
- Tabela de disponibilidade: matriz `phase × run_id` com contagem de linhas.
- Aviso fixo: workstation é canônica; i5 é P/E/histórico.

Gráficos:

- Barras de contagem por fase.
- Heatmap de cobertura `patterns × corpus`.

### 2. Curvas de Speedup

Objetivo: comparar searchers ao variar número de threads.

Fonte principal:

- `v_speedup` para speedup vs `sequential`.
- `v_self_speedup` para escalabilidade própria.
- `runs` para `mbps`, `median_ms`, `cv_pct`.

Gráficos obrigatórios:

- Linha: `thr` no eixo X, `speedup_vs_seq` no Y, cor por `searcher`, facet por
  `patterns/corpus` quando houver múltiplos cenários.
- Linha: `thr` no eixo X, `mbps` no Y, cor por `searcher`.
- Linha ou área: eficiência `speedup_vs_seq / thr`, calculada no app.
- Marcadores verticais úteis: T=16 e T=32 na workstation; T=physical/P+E no i5
  se documentado ou detectado em metadados.

Interações:

- Selecionar um ou mais searchers.
- Comparar `pthread_dynamic_flat`, `pthread_chunked_flat`,
  `pthread_chunked_v3_flat`, `pthread_dynamic`, `pthread_chunked_v2` por
  default na workstation.
- Mostrar tabela filtrada abaixo do gráfico com `thr`, `mbps`,
  `speedup_vs_seq`, `cv_pct`, `matches`.

### 3. Ranking e Vencedores

Objetivo: ver quem vence por cenário e por quantidade de threads.

Fonte:

- `v_best`
- `runs`

Gráficos obrigatórios:

- Heatmap: eixo X `thr`, eixo Y `patterns/corpus`, cor = searcher vencedor.
- Barras agrupadas: top-N searchers por `mbps` para um cenário selecionado.
- Tabela rankeada com colunas `run_id`, `patterns`, `corpus`, `thr`,
  `searcher`, `mbps`, `speedup_vs_seq` quando disponível.

Requisitos:

- Permitir escolher `thr` específico ou comparar todos.
- Permitir ranking por `mbps`, `speedup_vs_seq`, `median_ms` ou `cv_pct`.
- Destacar empates próximos com tolerância configurável, default 1%.

### 4. Footprint e Cache Cliff

Objetivo: visualizar impacto do tamanho do autômato.

Fonte:

- `v_footprint`
- `runs` para baseline sequential e metadados adicionais.

Gráficos obrigatórios:

- Scatter/log-line: `automaton_mib` no X, `mbps` no Y, cor por `searcher`, símbolo
  por `thr`.
- Linha: `automaton_mib` vs speedup calculado contra sequential do mesmo
  `patterns/corpus`.
- Cards: menor/maior autômato, queda do sequential, melhor throughput por
  footprint.

Requisitos:

- Usar escala log no X como opção.
- Mostrar linhas/áreas de referência para L2/L3 somente se os valores forem
  configurados no app; não inferir sem documentação.

### 5. Cross-Corpus

Objetivo: comparar comportamento por corpus.

Fonte:

- `runs` com `phase='C_cross_corpus'` e também fase A quando útil.

Gráficos obrigatórios:

- Barras agrupadas: `mbps` por corpus, cor por searcher, facet por `thr`.
- Linha: `thr` vs `mbps` quando houver múltiplos T por corpus.
- Tabela com deltas percentuais entre corpora para o mesmo
  `patterns/searcher/thr`.

Requisitos:

- Default: `patterns_snort`, `pthread_dynamic_flat`, corpora `enron_corpus` e
  `simplewiki` na workstation.
- Não afirmar causalidade textual; mostrar apenas diferenças observadas.

### 6. Workers e Balanceamento

Objetivo: diagnosticar stragglers, P/E e balanceamento real por worker.

Fonte:

- `worker_metrics`
- `v_worker_balance`

Gráficos obrigatórios:

- Barras por worker: `worker_id` no X, `milliseconds` no Y, cor por searcher.
- Barras por worker: `bytes_scanned` e `matches_found`.
- Linha ou barras: `worker_id` vs `mbps`.
- Scatter: `matches_found` vs `milliseconds`, tamanho por `bytes_scanned`.
- Heatmap: `searcher × thr` com `spread_pct` ou `imbalance_ratio`.

Tabelas:

- Resumo de `v_worker_balance` com `workers`, `min_worker_ms`,
  `avg_worker_ms`, `max_worker_ms`, `spread_pct`, `imbalance_ratio`.
- Dados crus de `worker_metrics` filtrados.

Requisitos:

- Default workstation: fase `D_per_thread`, T=32, snort/enron.
- Default i5: destacar `pthread_chunked_v2`, `pthread_chunked_v3`,
  `pthread_chunked_v3_flat`.
- Mostrar aviso se `worker_metrics` estiver vazio para o filtro atual.

### 7. Build Paralelo

Objetivo: separar custo de construção do autômato do throughput de busca.

Fonte:

- `v_build`
- `runs` com `phase='E_build_par'`

Gráficos obrigatórios:

- Linha: `build_threads` vs `build_speedup`, cor por `patterns`.
- Linha: `build_threads` vs `build_ms`, cor por `patterns`.
- Barras: melhor speedup por dicionário.

Requisitos:

- Mostrar `build_speedup < 1` claramente como regressão.
- Não misturar tempo de build nos gráficos de throughput da busca.

### 8. Granularidade Dinâmica

Objetivo: visualizar efeito de `tokens-per-task` na fase G.

Fonte:

- `runs` com `phase='G_granularity'`
- `worker_metrics` e `v_worker_balance` quando disponíveis para fase G.

Gráficos obrigatórios:

- Linha: `tag` ou tokens-per-task no X, `mbps` no Y.
- Linha: tokens-per-task no X, `cv_pct` no Y.
- Heatmap opcional: tokens-per-task × worker com `milliseconds`, se houver dados
  por worker.

Requisitos:

- Extrair número de tokens-per-task de `tag` quando o padrão for algo como
  `tpt001`, `tpt004`, `tpt016`, `tpt064`, `tpt256`.
- Ordenar numericamente, não lexicograficamente.
- Informar que fase G não substitui corpus skewed.

### 9. Comparação Entre Runs

Objetivo: comparar workstation e i5 sem induzir conclusão errada.

Gráficos obrigatórios:

- Linha: `thr` vs `speedup_vs_seq`, facet por `run_id`, mesmos searchers.
- Barras: melhor `mbps` por run, patterns e corpus.
- Tabela lado a lado para searchers selecionados.

Regras:

- Default desligado para i5.
- Quando i5 estiver ativo, exibir aviso: i5 é evidência P/E, não headline.
- Não mostrar ranking global “melhor máquina” por default.

## Métricas Derivadas

Calcular no app, sem alterar DB:

- `efficiency_vs_seq = speedup_vs_seq / thr`
- `efficiency_vs_t1 = speedup_vs_t1 / thr`
- `delta_mbps_pct = (a_mbps / b_mbps - 1) * 100`
- `near_tie = mbps >= best_mbps * (1 - tolerance_pct / 100)`
- `automaton_mib = automaton_kib / 1024` quando a view não trouxer.
- `tpt = int(regex(tag))` para fase G.

Tratar `thr=0` do `sequential` como caso especial. Para gráficos de speedup,
usar o baseline `sequential` como referência, não como ponto de thread.

## Consultas Base

Exemplos que o agente pode converter para pandas:

```sql
SELECT * FROM runs;
SELECT * FROM v_speedup;
SELECT * FROM v_self_speedup;
SELECT * FROM v_best;
SELECT * FROM v_footprint;
SELECT * FROM v_build;
SELECT * FROM v_correctness;
SELECT * FROM v_worker_balance;
SELECT * FROM worker_metrics;
```

Para dados unidos por run, carregar cada banco separadamente e concatenar em
pandas com `run_id`. Não tentar `ATTACH` múltiplos bancos no primeiro desenho;
isso deixa filtros e cache mais simples.

## Qualidade Visual

O painel é para leitura humana, então evitar tabelas cruas como interface
principal. Usar Plotly para hover, legenda clicável, zoom e facets.

Direção visual:

- Layout wide: `st.set_page_config(layout="wide")`.
- Sidebar para filtros globais.
- Abas ou `st.navigation` para páginas.
- Cards compactos no topo de cada página.
- Gráficos com títulos objetivos e unidades explícitas.
- Tabelas abaixo dos gráficos, não acima.
- Paleta estável por `searcher`, para a mesma variante manter a mesma cor em
  páginas diferentes.

Hover mínimo recomendado:

- `run_id`
- `phase`
- `patterns`
- `corpus`
- `searcher`
- `thr`
- `mbps`
- `speedup_vs_seq` quando existir
- `cv_pct`
- `matches`
- `log_file`

## Segurança dos Dados

- Abrir SQLite em modo read-only.
- Nunca chamar `build_sweep_db.py` automaticamente.
- Nunca escrever em `runs/`.
- Se implementar exportação, exportar apenas para download via Streamlit
  (`st.download_button`) ou para `dashboard/exports/` se o usuário pedir depois.
- O app deve falhar de forma clara se um DB estiver ausente, mantendo os demais
  disponíveis.

## Critérios de Aceite

O primeiro PR/entrega deve satisfazer:

- `streamlit run dashboard/app.py` abre sem erro na raiz do repositório.
- O app lista `workstation_2026-06-30` e `i5` quando os bancos existem.
- A página de visão geral mostra contagens por fase e `v_correctness`.
- A página de speedup permite comparar pelo menos 5 searchers em
  `patterns_snort/enron_corpus` na workstation.
- A página de workers mostra dados de `worker_metrics` para fase
  `D_per_thread`.
- A fase G ordena `tpt001`, `tpt004`, `tpt016`, `tpt064`, `tpt256`
  numericamente.
- O app não modifica nenhum arquivo em `runs/`.
- O README do dashboard explica instalação, execução e limitações.

Validação mínima:

```bash
python3 -m compileall dashboard
streamlit run dashboard/app.py
```

Se `streamlit run` não puder ser validado em ambiente headless, pelo menos
validar import/compile dos módulos e registrar isso no resultado.

## Ordem de Implementação Recomendada

1. Criar `requirements-dashboard.txt` e estrutura `dashboard/`.
2. Implementar loader SQLite read-only com concatenação por `run_id`.
3. Criar filtros globais e página de visão geral.
4. Implementar curvas de speedup e ranking.
5. Implementar workers/balanceamento.
6. Implementar footprint, cross-corpus, build e granularidade.
7. Adicionar comparação entre runs com avisos sobre i5.
8. Escrever `dashboard/README.md`.
9. Rodar validação mínima.

## Decisões em Aberto

- Se o app deve ganhar comando no `Makefile`, por exemplo `make dashboard`.
- Se deve haver exportação de gráficos HTML via Plotly.
- Se vale adicionar um arquivo de metadados manual por run com CPU/cache para
  marcar T=16/T=32 e P/E sem inferência frágil.
- Se futuros runs devem ser descobertos automaticamente ou cadastrados
  explicitamente em uma lista allowlist.

Para a primeira versão, preferir allowlist explícita dos dois bancos preservados.
Isso evita ressuscitar runs antigos ou bases que não devem mais orientar a tese.
