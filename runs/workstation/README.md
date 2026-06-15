# Sweep da workstation — contexto completo para o run no AMD Ryzen 9 9950X

> **Para uma IA lendo isto a frio:** este arquivo é auto-contido. Ele explica
> o que é o experimento, como rodá-lo, o formato exato da saída e como
> analisá-la. Se você só precisa dos números, vá direto para
> [Análise](#5-análise-dos-resultados) e consulte `sweep.db` via `sqlite3`.
> NÃO faça `cat` dos `.log` (são muitos) — use as views do SQLite.

---

## 0. O que é isto

Material de apoio de um **TCC (IDP, Eng. de Software)** sobre **paralelização
do algoritmo Aho–Corasick em CPUs multicore de memória compartilhada
(POSIX Threads)**, aplicado a **detecção de intrusão (IDS)**: dicionários de
regras Snort/Emerging-Threats varrendo um corpus de e-mails (Enron) em escala
de gigabytes. Cada variante paralela é comparada contra um baseline
sequencial; métricas em **MB/s, Gbps e speedup vs. nº de threads**.

Este diretório (`runs/workstation/`) guarda **uma corrida única** numa
**máquina alugada** com o `scripts/run_workstation_sweep.sh`. Diferente do
sweep canônico do laptop (`runs/overnight/`, i5-1235U híbrido), o alvo aqui é
um chip de **núcleos homogêneos**, para medir **portabilidade/generalização**
das contribuições.

| Por quê duas máquinas | Resumo |
|---|---|
| `runs/overnight/` (i5-1235U, **híbrido** P/E, L3 12 MiB) | Sweep definitivo de 2026-05-29, **fonte de verdade atual da tese** (265 runs). Inclui os searchers topology-aware (`v3`, `v3_flat`). |
| `runs/workstation/` (9950X, **homogêneo**, L3 2×32 MiB) | Esta corrida. **Descarta** `v3`/`v3_flat` (sem P/E para balancear → viram `v2`/`chunked_flat`) e foca o contraste **estático vs. dinâmico**. |

---

## 1. Hardware alvo

| Item | Valor |
|---|---|
| CPU | **AMD Ryzen 9 9950X** (Zen 5), **16 núcleos físicos idênticos / 32 threads** (SMT 2×) |
| Cache L3 | **64 MiB NÃO unificado** — 2 complexos de núcleos (CCD) de **32 MiB** cada |
| Memória | DDR5-5600 (confirmar valor exato em `env/start.txt`) |
| OS | Linux Ubuntu |
| Build | C11, binário único, `-O3 -march=native` |
| Governador | `performance` (Zen 5 = `amd-pstate`) |

Implicações arquiteturais que a grade explora:
- **8 threads cabem em 1 CCD**; **8→16** cruza a fronteira entre CCDs (L3 não
  compartilhado); **16→32** é o ganho marginal do **SMT**.
- L3 por-CCD = 32 MiB **move o cross-over de cache**: `patterns_snort` (~56 MiB)
  fica *entre* o L3 de um CCD (32 MiB) e o total (64 MiB) — número novo e citável.

Os snapshots reais do chip ficam em `env/start.txt` e `env/end.txt`
(`lscpu` completo, `lscpu -C`, `numactl --hardware`, `lstopo`,
`dmidecode -t memory`, THP, kernel cmdline, governor, térmico, commit do git).

---

## 2. Pré-requisitos / provisionamento de dados (CRÍTICO)

`data/` é **gitignored**: `git clone` numa máquina fresca **não traz os
corpora**. Pior, **dois arquivos não são gerados por nenhum script**:

| Arquivo | Tamanho | Como obter |
|---|---|---|
| `data/enron_corpus.txt` | 1,42 GiB | `scripts/prepare_datasets.sh` (download Enron) **ou** scp do laptop |
| `data/patterns_snort.txt` | ~87 KiB | `scripts/prepare_datasets.sh` (extrai das regras Snort) **ou** scp |
| `data/patterns_et_32.txt` | ~816 KiB | **scp do laptop obrigatório** — sem fonte/script (≈44708 padrões ET) |
| `data/enron_x8.txt` | 10,59 GiB | **gerado** = `enron_corpus.txt` × 8 (o pré-flight faz) |
| `data/patterns_snort_100.txt` / `_1k.txt` | KiB | **gerados** via `head` (o pré-flight faz) |

Use o pré-flight dedicado — ele valida, gera o que dá e **PARA com instrução de
scp** se `patterns_et_32.txt` faltar:

```bash
./scripts/prepare_workstation_data.sh        # só siga se o veredito for "PRONTO"
# (AUTO_DOWNLOAD=1 baixa Snort/Enron se faltarem)
```

Transferência mínima recomendada: leve só `enron_corpus.txt` +
`patterns_*.txt` (~1,42 GiB) e deixe o `enron_x8` ser gerado no host (evita
mover 10,6 GiB). **RAM:** garanta ≳12–16 GiB livres (`enron_x8` é mmap'd com
`MAP_POPULATE` → ~10,6 GiB residentes).

---

## 3. Como rodar

```bash
./scripts/prepare_workstation_data.sh             # 1. dados → "PRONTO"
sudo cpupower frequency-set -g performance         # 2. governor performance
make && make test                                  # 3. correção no chip-alvo
nohup ./scripts/run_workstation_sweep.sh > workstation.out 2>&1 &   # 4. sweep
```

- **Default roda `A B D E`.** A fase opcional `S` (2-D/sharding, trabalho
  futuro) só entra com `PHASES="A B D E S"`.
- **Múltiplas fases ⇒ use a env `PHASES`** (`PHASES="A D" ...`). Passar fases
  como argumentos posicionais (`... A B`) só honra a **primeira** — limitação
  conhecida da orquestração; um único argumento (`... A`) funciona.
- **Resiliência:** cada run grava 1 `.log`; **rerodar PULA o que já completou**
  (resume-from-crash via `grep` da linha de resultado). Falhas viram `.FAIL`
  sem derrubar o sweep. `flock` impede instâncias paralelas. SIGINT/SIGTERM
  tratados.
- **Pontos de thread adaptam-se ao `nproc`** (ou `MAX_THREADS=…`): em 32
  threads a curva da fase A é `{1,2,4,8,16,24,32}`.
- **Override de dataset para teste:** `DATA_DIR=/caminho ...` aponta para outro
  diretório de dados (usado nos smoke tests).
- **Ao terminar**, o script roda os extratores automaticamente →
  `sweep.csv` + `sweep.db` prontos neste diretório.

```bash
git add runs/workstation && git commit -m "resultados: sweep workstation 9950X"
```

`runs/workstation/` tem **exceção no `.gitignore`** (o resto de `runs/` segue
ignorado), então estes resultados são versionáveis.

---

## 4. A grade de testes (o que cada fase mede)

Searchers avaliados (núcleo = 6). Dois eixos independentes:
**distribuição** (estática = fatia fixa por thread; dinâmica = fila de tarefas,
4N tarefas, contador atômico) × **emissão** (lista encadeada vs. tabela
contígua/*flat*).

| Searcher | Distribuição | Emissão | Papel |
|---|---|---|---|
| `sequential` | — | encadeada | baseline / oráculo de correção / denominador do speedup |
| `sequential_flat` | — | flat | isola o efeito do layout flat sem paralelismo |
| `pthread_chunked_v2` | estática | encadeada | baseline de chunking estático |
| `pthread_chunked_flat` | estática | flat | campeão portável estático (idea 5) |
| `pthread_dynamic` | dinâmica | encadeada | bag of tasks (substitui o papel do `v3` em cores homogêneos) |
| `pthread_dynamic_flat` | dinâmica | flat | **candidato a campeão homogêneo** |

> **Descartados de propósito:** `pthread_chunked_v3` / `v3_flat` (topology-aware
> + pesos por `cpufreq`) — sem P/E cores para balancear, colapsam em
> `v2`/`chunked_flat`. `pthread_chunked` (v1) e `pthread_prefetch` são
> dominados.

| Fase (dir) | Pergunta | Parâmetros | Runs @32T |
|---|---|---|---:|
| **A** `A_speedup_curves` | Como a vazão escala com threads? Quanto cada eixo contribui? | `enron_x8`; `{snort, et_32}`; T∈{1,2,4,8,16,24,32}; **warmup 2, iters 8** | **60** |
| **B** `B_footprint` | Em que tamanho de autômato a cache satura? | `enron_corpus`; dicts {snort_100, snort_1k, snort, et_32}; `{seq, seq_flat}` + `{chunked_flat, dynamic_flat}`×T∈{1,32}; warmup 2, iters 5 | **24** |
| **D** `D_per_thread` | Quanta ociosidade/desbalanceamento por thread? | `enron_x8`, `snort`, T=32, `--per-thread`; 4 paralelos; warmup 1, iters 3 | **4** |
| **E** `E_build_par` | A construção do autômato escala? | `enron_corpus`; 4 dicts; `buildseq` + `buildpar` T∈{2,4,8,16,32}; warmup 0, iters 1 | **24** |
| **S** (opc.) `S_2d_sharding` | 2-D/sharding (trabalho futuro) | `enron_x8`, `snort`; `{2d_sharded_chunked, pattern_sharded_prefix}`×T∈{1,32} | +4 |
| | | **Núcleo** | **112** |

**Comparações limpas que a grade isola (fase A):**
- **emissão** encadeada vs. flat (mesma partição estática): `chunked_v2` vs. `chunked_flat`;
- **distribuição** estática vs. dinâmica (mesma emissão): `chunked_v2` vs. `dynamic` (encadeada) e `chunked_flat` vs. `dynamic_flat` (flat).

Justificativa detalhada da grade: `docs/sweep-test-inventory.md` e
`docs/testes-workstation.md`.

---

## 5. Análise dos resultados

### 5.1 Estrutura de saída

```
runs/workstation/
  README.md                 # este arquivo
  MASTER.log                # event log timestampado (RUN/OK/SKIP/FAIL)
  env/start.txt, end.txt    # snapshot rico de hardware/ambiente (início e fim)
  env/thermal.tsv           # térmico + MHz médio por run (correlação de throttling)
  A_speedup_curves/<run>.log
  B_footprint/<run>.log
  D_per_thread/<run>.log
  E_build_par/<run>.log
  sweep.csv                 # 1 linha por run (gerado no fim)
  sweep.db                  # SQLite tipado + 6 views — USE ISTO
```

Nome dos `.log`: `<patterns>__<corpus>__<searcher>__T<n>[_<tag>].log`.
Cada log tem um header `#` (`# patterns:`, `# automaton: N states, X MiB`,
`# build time:`, `# warmup= iters= threads=`) e a tabela de resultado. As
linhas `#   iter_ms:` e `[tNN] ...` (per-thread, fase D) também estão no log
mas **não** entram no CSV/DB — leia o `.log` para o detalhe por thread.

### 5.2 Consultar (SQLite, token-efficient)

```bash
sqlite3 -header -column runs/workstation/sweep.db "SELECT ..."
```

Tabela `runs` (1 linha/run): `phase, patterns, corpus, searcher, thr, tag` ·
tempos ms `min_ms, mean_ms, median_ms, max_ms, cv_pct` · `mbps, gbps` ·
`bytes, matches, wrk` · autômato `pattern_count, automaton_states,
automaton_kib` · build `build_ms, build_threads` · `warmup, iters, log_file`.

Views (onde mora a análise):

| View | Para responder |
|---|---|
| `v_speedup` | Speedup vs. baseline `sequential` (figura headline, fase A). |
| `v_self_speedup` | Escalabilidade: `mbps(T)/mbps(T=1)` do mesmo searcher. |
| `v_footprint` | Fase B: `automaton_mib` × `mbps` (cross-over de cache). |
| `v_build` | Fase E: `build_speedup` paralelo vs. sequencial. |
| `v_best` | Melhor searcher (mbps) por `(patterns, corpus, thr)`. |
| `v_correctness` | `distinct_match_counts` por `(patterns, corpus)` — **deve ser 1**. |

```sql
-- 0. SANIDADE primeiro: correção (1 = todos os searchers concordam)
SELECT * FROM v_correctness;

-- 1. Curva de speedup de um searcher (snort × enron_x8)
SELECT thr, speedup_vs_seq FROM v_speedup
WHERE patterns='patterns_snort' AND corpus='enron_x8'
  AND searcher='pthread_dynamic_flat' ORDER BY thr;

-- 2. Quem vence e em que T (pico por cenário)
SELECT patterns, corpus, searcher, thr, speedup_vs_seq FROM v_speedup w
WHERE speedup_vs_seq=(SELECT MAX(speedup_vs_seq) FROM v_speedup x
  WHERE x.patterns=w.patterns AND x.corpus=w.corpus) ORDER BY patterns, corpus;

-- 3. Cross-over de cache em T=32 (fase B)
SELECT patterns, automaton_mib, searcher, mbps FROM v_footprint
WHERE thr=32 ORDER BY automaton_mib;

-- 4. Build paralelo (fase E)
SELECT patterns, build_threads, build_speedup FROM v_build ORDER BY patterns, build_threads;

-- 5. Balanceamento per-thread (fase D) — detalhe [tNN] está no .log
SELECT searcher, mbps, wrk FROM runs WHERE phase='D_per_thread';
```

### 5.3 Reproduzir os artefatos a partir dos `.log`

```bash
python3 scripts/extract_sweep_csv.py runs/workstation -o runs/workstation/sweep.csv --known-only
python3 scripts/build_sweep_db.py    runs/workstation/sweep.csv -o runs/workstation/sweep.db
```

---

## 6. Relação com a tese e fontes canônicas

- **Antes de citar qualquer número da workstation**, confronte `sweep.db` (aqui)
  com o sweep do laptop (`runs/overnight/sweep.db`). Os números do i5 são a
  fonte de verdade atual da tese; **estes resultados entram como seção de
  portabilidade/generalização** (homogêneo + SMT + L3 por-CCD), não substituem
  os do i5.
- Números **pré-2026-05-29** (ex.: 7,49×) foram **descartados** (baseline antigo
  lento) — não os reintroduza.
- Síntese da tese: `docs/tcc-synthesis.html`. Consolidação por seção:
  `../tcc_notes/sections/notes/{methodology,results,conclusion}.md`.

---

## 7. Troubleshooting

| Sintoma | Causa provável | Ação |
|---|---|---|
| Muitos `.FAIL` em A/D | `enron_x8.txt` ausente | rode `prepare_workstation_data.sh` |
| `.FAIL` só em `et_32` | `patterns_et_32.txt` ausente | scp do laptop (não regenerável) |
| `v_correctness` ≠ 1 | searchers discordam no nº de matches | **bug real** — investigar antes de usar números |
| `dynamic_flat` some do db | `--known-only` antigo sem ele | já corrigido em `extract_sweep_csv.py` |
| cv% alto / MHz caindo em `thermal.tsv` | throttling térmico | confirme governor `performance`, refrigeração |
| "outra instância já rodando" | `flock` em `.lock` | só um sweep por vez; remova `.lock` se órfão |

---

## 8. Ponteiros

- Script do sweep: `scripts/run_workstation_sweep.sh`
- Pré-flight de dados: `scripts/prepare_workstation_data.sh`
- Extratores: `scripts/extract_sweep_csv.py`, `scripts/build_sweep_db.py`
- Plano/justificativa: `docs/sweep-test-inventory.md`, `docs/testes-workstation.md`
- Searchers (técnico): `docs/searchers/<nome>.md`
- Guia de consulta do sweep do laptop (views idênticas): `runs/overnight/QUERY_GUIDE.md`
- Visão geral do projeto: `CLAUDE.md` (raiz do repo) e `../CLAUDE.md` (workspace)
