# Épico 04 — Corpus de carga desigual (skew) para justificar a família dinâmica

> Item de origem: `docs/TODO.md` → "P1 — Corpus de carga desigual / skew".
> Fonte de verdade da execução: esta pasta (`specs/epic-04/`).
> Convenção: **este é o primeiro épico do repositório** — define o padrão
> `specs/epic-NN/{context,progress,task-NN}.md`. Idioma: português (segue
> `docs/`), identificadores/código em inglês.

## Objetivo

Construir um corpus **espacialmente desigual** (carga concentrada em poucos
chunks) e uma **fase de benchmark isolada** que, numa CPU **homogênea** (Ryzen
9950X canônico), demonstre o mecanismo que o corpus uniforme quase não estressa:
a divisão estática de texto fica desbalanceada e o **dispatch dinâmico**
(`pthread_dynamic`, `pthread_dynamic_flat`) recupera o makespan. O resultado
transforma um ganho hoje pequeno/ambíguo em um **condicional defensável**
("rende quando a carga é espacialmente desigual"), justificando a família
dinâmica na tese sem depender do confundidor P/E do i5.

## Status atual (auditado em 2026-07-01)

- **Evidência que motiva o épico** (via
  `runs/workstation_2026-06-30/sweep.db`, view `v_speedup`, T=32): em CPU
  homogênea + Enron uniforme, `pthread_dynamic_flat` já é o campeão, mas por
  margem pequena sobre `pthread_chunked_flat` (Snort 22,91× vs. 22,48×; ET-32
  18,96× vs. 18,83×). O `pthread_dynamic` sem flat ainda fica atrás do
  estático-flat (Snort 21,98×; ET-32 17,97×). No i5, ganhos de balanceamento se
  misturam à **heterogeneidade P/E**. Logo, falta um cenário controlado que isole
  carga espacialmente desigual como causa do ganho dinâmico.
- **Corpora atuais** (`data/`, gitignored, recriáveis): `enron_corpus.txt`
  (~1,32 GiB), `enron_x8.txt` (~10,6 GiB, o **mesmo texto ×8** → perfeitamente
  uniforme), `simplewiki.txt`. Padrões versionados: `patterns_snort.txt`
  (4.188 padrões, autômato ~55 MiB), `patterns_et_32.txt` (44.678, ~507 MiB).
- **Gerador de corpus skew: NÃO existe.** `scripts/prepare_data.sh` gera
  `enron_x8` + dicts reduzidos; não há `make_skewed_corpus.sh`.
- **Motor de sweep** (`scripts/run_sweep.sh`): dispatcher de fases limpo
  (`case "$p" in A) phase_A ;; …` na linha ~555) + helper `run_aclab` (linha
  252) que já suporta `--per-thread` e tag por run. Fases hoje: A B C D E G.
  **Adicionar a fase H é isolado:** `PHASES="H" scripts/run_sweep.sh` roda só
  ela — **não** re-roda A–G.
- **Instrumentação per-thread** (`include/ac_searcher.h:16`,
  `ac_thread_metric_t`): hoje expõe `thread_id, seconds, bytes_scanned,
  matches_found`. O `seconds` por worker **já** dá o spread de tempo / ociosidade
  de barreira — a métrica-chave deste épico **não exige mudança em C**. Não há
  campo de CPU físico / classe P-E (irrelevante no Ryzen homogêneo; opcional).
- **Pipeline de resultados**: `extract_sweep_csv.py` **auto-descobre** os
  diretórios de fase (`os.listdir(run_dir)`), então uma pasta `H_skew/` é
  ingerida sem mudança; `build_sweep_db.py` cria as views `v_*`, preenche
  `worker_metrics` a partir das linhas `[tNN] …` dos logs `--per-thread` e expõe
  `v_worker_balance`. A análise de spread deve consultar esse DB, não duplicar o
  parser.
- **Lição da fase G antiga do i5:** 1 amostra por config pode ser dominada pela
  variância **entre corridas**, invisível ao cv intra-run. O run antigo foi
  removido e não deve ser citado como número; mantenha a regra metodológica:
  **réplicas (N≥5 processos independentes) são obrigatórias.**

## Escopo

- **In scope:**
  - `scripts/make_skewed_corpus.sh` — gerador do par uniforme×skewed (invariante
    de controle: mesmos bytes E mesmos matches) + família por fator de skew.
  - `scripts/run_sweep.sh` — **fase H isolada** (skew), com réplicas e
    `--per-thread`.
  - `scripts/analyze_skew.py` — consultas em `worker_metrics`/`v_worker_balance`
    → spread/ociosidade de barreira, mediana+IQR sobre réplicas.
  - Documentação: `runs/<run>/RESULTS.md`,
    `../tcc_notes/sections/notes/{methodology,results}.md`.
  - LaTeX: `acceleration-.../partes/{results,conclusion}.tex` +
    `referencias.bib` (só conferir/usar keys existentes).
- **Out of scope:**
  - **Scans reais / pcap / tráfego real de IDS** (DEFCON17, CIC-IDS, DARPA,
    ToN-IoT). Decisão registrada — estudo de throughput, não de acurácia; ver
    memória `real-scans-out-of-scope` e `docs/TODO.md`. Nada de encanamento
    pcap→bytes neste épico.
  - **Novos conjuntos de padrões** (ClamAV etc.): o eixo Snort-100→ET-32 já
    cobre o regime de cache; padrões ficam **intactos** (mexer nos dois lados =
    benchmark circular).
  - Alterar searchers ou o contrato `ac_searcher_t::search`.
  - Re-rodar as fases A–G (a fase H é aditiva e isolada).
  - Instrumentar CPU físico / P-E / freq efetiva no `--per-thread` — só como
    **Task 04 opcional** (sinergia com o P1 "métricas"), desnecessária no Ryzen
    homogêneo.

## Dependências

- **Bloqueante:** nenhuma dura. O binário atual (`build/aclab`, com
  `pthread_dynamic_flat` registrado e `--per-thread`) basta. Exige a coleta
  rodar na **workstation Ryzen 9950X** (homogênea) para o argumento valer.
- **Sinergia (não bloqueia):**
  - P1 "Métricas: replicar invocações + instrumentar `--per-thread`" — a Task 02
    já embute as réplicas mínimas; a instrumentação de CPU/P-E/freq (Task 04
    opcional) enriquece a atribuição, mas não é necessária no homogêneo.
  - P1 "Sweep de granularidade (fase G)" — o corpus skew é o alvo ideal para
    re-rodar a fase G depois (carga desigual dá à fila dinâmica o que corrigir).

## Forma do épico

**Slice vertical por camada** (gerador → harness → análise → docs → LaTeX),
com uma Task opcional de instrumentação em C. Acoplamento é sequencial: cada
task consome a saída da anterior (corpus → logs → tabela → texto). Ver
`progress.md` para a ordem de execução e as dependências duras.

## Instruções para a IA de Tarefas

- **Máquina:** rode a coleta (Task 02) na **workstation Ryzen 9950X**
  (`RUN_DIR=runs/workstation_skew PHASES="H" ./scripts/run_all.sh` ou
  `PHASES="H" scripts/run_sweep.sh` com `MAX_T` derivado). Governador
  `performance`. **Não** re-rode A–G.
- **Invariante científico inegociável (Ródenas):** o par uniforme×skewed deve
  ter **bytes totais idênticos E contagem de matches idêntica**; varia só a
  distribuição espacial. Se não bater, o resultado não é atribuível a
  balanceamento. A Task 01 deve **provar** isso (mesmos blocos, só reordenados)
  e a Task 03 deve reportar a paridade.
- **Disciplina:** manipular só o corpus; padrões Snort/ET intactos.
- **Alavanca correta:** heterogeneidade de conteúdo (blocos que forçam o DFA a
  estados frios) é a alavanca primária; densidade de match (FHBM, alvo ~40%
  DEFCON *ugly*) é reforço — com flat a emissão é barata.
- **Réplicas obrigatórias:** N≥5 processos independentes por config; reportar
  **mediana + IQR** (não média de iterações intra-processo). Sem isso o achado
  repete o inconclusivo da fase G.
- **Citações:** ao chegar no LaTeX, **conferir as keys** em
  `acceleration-.../referencias.bib` (não inventar). ⚠️ O resumo do "Ródenas
  Picó" é a tese de *graph matching* dele, não o paper "Parallel AC/NFA/OpenMP";
  enquadrar como moldura de balanceamento transferível, não precedente de AC.
- **Decisão a devolver ao usuário, não inventar:** se a Task 03 mostrar que
  **nem** o corpus skewed faz o dinâmico vencer (efeito real fraco por causa do
  flat barato), **não** force a narrativa — reporte como resultado negativo
  robusto e pergunte se a família dinâmica deve ser aposentada do texto.
