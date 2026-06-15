# Inventário do sweep noturno (`scripts/run_overnight_sweep.sh`)

Relação **exata** de execuções disparadas pelo sweep canônico do TCC.
Fonte: `scripts/run_overnight_sweep.sh`. Cada execução é uma chamada de
`build/aclab` com `--patterns`, `--input`, `--searcher`, `--threads`,
`--warmup` e `--iters`. O total bate com os **265 runs** do
`runs/overnight/sweep.db` (sweep de 2026-05-29, 0 falhas).

> Nota: o cabeçalho-comentário do script (linhas 27-39) está **desatualizado**
> em relação ao corpo. Onde houver divergência, vale o código. Ex.: o comentário
> diz "8 searchers" e "T ∈ {1,2,3,4,6,8,10,12}", mas o código usa **9 searchers
> paralelos** e a curva real em `enron_corpus` é **{1,2,4,6,8,10,12}** (sem 3).

## Parâmetros globais

- `MAX_T = nproc` (na máquina atual, i5-1235U → **12**). Abaixo uso `MAX_T`.
- Searchers sequenciais: `sequential` (chain-walk) e `sequential_flat` (idea 5).
- **Conjunto de 9 searchers paralelos** (reusado em A e D):
  `pthread_chunked`, `pthread_chunked_v2`, `pthread_chunked_v3`,
  `pthread_dynamic`, `pthread_prefetch`, `pthread_chunked_flat` (idea 5),
  `pthread_chunked_v3_flat` (ideas 5+7), `pthread_2d_sharded_chunked` (idea 6),
  `pattern_sharded_prefix` (idea 1).
- Datasets de padrões: `patterns_snort.txt` (~56 MiB de autômato),
  `patterns_et_32.txt` (~515 MiB), e reduzidos `patterns_snort_100.txt`,
  `patterns_snort_1k.txt` (criados via `head` se ausentes).
- Corpora: `enron_corpus.txt` (1,42 GiB), `enron_x8.txt` (10,59 GiB),
  `simplewiki.txt` (1,28 GiB).

---

## Fase A — curvas de speedup (the headline figure) — **206 runs**

`warmup=2 iters=5`. Dois sub-blocos por corpus.

### Curva de threads
- **A1** (`enron_corpus`): `Ts_corpus = {1, 2, 4, 6, 8, 10, MAX_T}` → 7 pontos.
- **A2** (`enron_x8`): `Ts_x8 = {1, 4, 8, MAX_T}` → 4 pontos.

### A1 — `enron_corpus` (1,42 GiB)
Para cada conjunto de padrões em `{snort, et_32}`:
- 2 baselines single-thread: `sequential`, `sequential_flat`.
- 7 pontos de thread × 9 searchers paralelos = 63.
- Subtotal por padrão = 65. **× 2 padrões = 130 runs.**

### A2 — `enron_x8` (10,59 GiB)
Para cada conjunto de padrões em `{snort, et_32}`:
- 2 baselines single-thread.
- 4 pontos de thread × 9 searchers = 36.
- Subtotal por padrão = 38. **× 2 padrões = 76 runs.**

---

## Fase B — footprint do autômato vs. throughput — **24 runs**

`warmup=2 iters=5`, corpus fixo `enron_corpus`. Varre o tamanho do
dicionário para localizar o cross-over de cache.

- Dicts: `snort_100`, `snort_1k`, `snort`, `et_32` (4).
- Por dict:
  - single-thread: `sequential`, `sequential_flat` (2 runs).
  - multi-thread: `pthread_chunked_v3`, `pthread_chunked_flat` × T ∈ {1, MAX_T} (4 runs).
- Subtotal por dict = 6. **× 4 = 24 runs.**

---

## Fase C — sensibilidade ao corpus — **6 runs**

`warmup=2 iters=5`, padrões fixos `snort`.

- Corpora: `simplewiki`, `enron_corpus` (2).
- Por corpus: `sequential` (1, single-thread) + `pthread_chunked_v3` × T ∈ {1, MAX_T} (2).
- Subtotal por corpus = 3. **× 2 = 6 runs.**

---

## Fase D — diagnóstico per-thread em T=MAX_T — **9 runs**

`warmup=1 iters=3 --per-thread`, padrões `snort`, corpus `enron_corpus`,
T = MAX_T fixo. 1 run por searcher do conjunto de 9 paralelos.
Alimenta a tabela de balanceamento (ociosidade de barreira).

---

## Fase E — build paralelo vs. sequencial (idea 4) — **20 runs**

`warmup=0 iters=1` (só interessa o `# build time:` do header), searcher
sempre `sequential`, corpus `enron_corpus`.

- Dicts: `snort_100`, `snort_1k`, `snort`, `et_32` (4).
- Por dict:
  - build sequencial: 1 run (tag `buildseq`).
  - build paralelo: `AC_BUILD_PARALLEL=1`, `AC_BUILD_THREADS = T` para
    `Ts_build = {2, 4, 8, MAX_T}` → 4 runs (tags `buildpar_T<n>`).
- Subtotal por dict = 5. **× 4 = 20 runs.**

---

## Total

| Fase | Foco | Runs |
|------|------|-----:|
| A | curvas de speedup (escalonamento) | 206 |
| B | footprint / cache blowout | 24 |
| C | sensibilidade ao corpus | 6 |
| D | per-thread / balanceamento | 9 |
| E | build paralelo (idea 4) | 20 |
| **Total** | | **265** |

Resiliência: cada run grava 1 `.log`; rerodar pula o que já completou
(grep pela linha de resultado do searcher). Falhas viram `.FAIL` sem
derrubar o sweep. `flock` impede instâncias paralelas.

---

# Subconjunto recomendado — máquina reservada (AMD Ryzen 9 9950X, 16C/32T)

Alvo: **AMD Ryzen 9 9950X**, Zen 5, **16 núcleos homogêneos / 32 threads**
(SMT 2-way), L3 **64 MiB** (2 CCDs × 32 MiB — **não unificado**),
DDR5-5600. Objetivo escolhido: **validar a portabilidade dos headlines**.
Corpus principal: **`enron_x8`**.

## Premissa (cores homogêneos)

Diferente do i5-1235U híbrido, o 9950X tem **cores idênticos**. Consequências:

- **`pthread_chunked_v3` / `pthread_chunked_v3_flat` saem de escopo.** Toda a
  contribuição deles era afinidade topológica + chunks ponderados por
  `cpufreq` para balancear P-cores vs. E-cores. Em cores homogêneos o peso
  colapsa para uniforme e `v3 ≈ v2`, `v3_flat ≈ chunked_flat`. **Descartados.**
- O balanceamento de carga relevante aqui é **bag of tasks** =
  **`pthread_dynamic`** (dispatch dinâmico via contador atômico, 4N tarefas).
  É o substituto natural do v3: em vez de pesos estáticos por tipo de core,
  ele absorve dinamicamente qualquer assimetria residual (contenção de SMT,
  fronteira de CCD, jitter térmico). **Mantido como o load-balancer do estudo.**
- **Topologia de cache (CCD):** o L3 **não é unificado** — são 32 MiB por CCD.
  Isso reposiciona o cross-over de footprint: `snort` full (~56 MiB) fica
  *entre* o L3 por-CCD (32 MiB) e o total (64 MiB); `snort_1k` (~10-12 MiB)
  cabe num CCD; `et_32` (~515 MiB) segue muito acima. Fase B mede esse
  deslocamento — número novo e citável.
- DDR5-5600 + 2 CCDs entregam muito mais banda agregada que o laptop; o
  regime memory-bound deve escalar melhor até 16 cores físicos.

## O que entra e por quê

| Eixo | Decisão | Motivo |
|------|---------|--------|
| Idea 5 (flat) | **manter, central** | Contribuição portável principal; ganho regime-dependente (+13% single-thread em ET-32, memory-bound). Tem de generalizar fora do laptop. |
| Curva de escalonamento | **manter, enron_x8** | The headline figure; agora 16 cores homogêneos + SMT. |
| Bag of tasks (`pthread_dynamic`, `pthread_dynamic_flat`) | **manter como o load-balancer** | Substitui o papel do v3 em cores homogêneos. `dynamic_flat` (bag of tasks + flat) é o candidato a campeão. |
| Cache blowout (Fase B) | **manter** | L3 por-CCD (32 MiB) move o cross-over — número novo. |
| Per-thread (Fase D) | **manter, T=32** | Diagnóstico de barreira: agora compara chunking estático vs. bag-of-tasks. |
| Build paralelo (idea 4, Fase E) | **manter** | Barato; 16 cores podem melhorar o 1,31× de ET. |
| **Topology-aware (`v3` / `v3_flat`)** | **DESCARTAR** | Cores homogêneos: sem P/E para balancear, vira `v2`/`chunked_flat`. |
| Idea 6 (2-D) / idea 1 (sharding) | **opcional/secundário** | Trabalho futuro da conclusão; fora do objetivo "portabilidade". Só se sobrar máquina. |
| Sensibilidade ao corpus (Fase C) | **cortar** | Já estabelecido (corpus quase não muda throughput); com 1 corpus, é mudo. |
| `pthread_chunked` (v1), `pthread_prefetch` | **cortar** | Dominados; `v2` é o baseline estático e `flat` o campeão. (`prefetch` fica como opcional se quiser medir o efeito do software prefetch sob DDR5.) |
| `simplewiki`, `enron_corpus` na grade A | **cortar** | Grade principal é `enron_x8`. (Fase B usa `enron_corpus` por custo — ver abaixo.) |

## Pontos de thread

`Ts = {1, 2, 4, 8, 16, 24, 32}` — captura: single, região dentro de **um
CCD** (≤8), travessia da **fronteira entre CCDs** (8→16), todos os 16 cores
físicos (16) e a faixa com **SMT** (24, 32). O joelho da curva em 16→32
isola o ganho marginal do SMT no regime memory-bound.

## Conjunto de searchers (núcleo = 6)

- Baselines: `sequential`, `sequential_flat` (idea 5 single-thread).
- Baseline de chunking estático: `pthread_chunked_v2`.
- Campeão portável estático (idea 5 multi-thread): `pthread_chunked_flat`.
- **Bag of tasks** chain-walk (controle de escalonamento): `pthread_dynamic`.
- **Bag of tasks + flat** — candidato a campeão homogêneo:
  `pthread_dynamic_flat`.

> **`pthread_dynamic_flat` foi criado e validado** (2026-06-05): dispatch
> dinâmico do `pthread_dynamic` + emissão flat da idea 5. Passa `make test`
> em `{1,2,3,4,7,8}` (54 OK / 0 FAIL) e `make tsan` limpo. Bench de sanidade
> no i5 híbrido (Snort + 600 MB Enron, T=12, mesma contagem de matches):
>
> | Searcher | MB/s | cv% |
> |---|---:|---:|
> | `pthread_dynamic` | 432,6 | 8,41 |
> | `pthread_chunked_flat` | 467,7 | 4,99 |
> | **`pthread_dynamic_flat`** | **472,3** | **1,64** |
>
> Já empata/supera o `chunked_flat` e tem a menor variância mesmo no laptop;
> espera-se vantagem mais clara em cores homogêneos. As duas comparações
> limpas que a grade isola: (a) **escalonamento** — `pthread_dynamic` vs
> `pthread_chunked_v2` (ambos chain-walk); (b) **estático vs dinâmico com
> flat fixo** — `pthread_chunked_flat` vs `pthread_dynamic_flat`.

## Grade final

### A' — escalonamento (`enron_x8`, warmup=2, **iters=8**)
Réplicas maiores (iters 5→8) p/ apertar a variância, já que o objetivo é
robustez/portabilidade.
- Padrões: `snort`, `et_32`.
- Por padrão: 2 baselines single-thread + 7 threads × 4 paralelos
  (`v2`, `flat`, `dynamic`, `dynamic_flat`) = 30.
- **× 2 = 60 runs.**

### B' — footprint / cache blowout (`enron_corpus`, warmup=2, iters=5)
Em `enron_corpus` (1,4 GiB) de propósito: footprint é função do
**dicionário**, não do corpus (Fase C provou), e rodar 4 dicts em
single-thread sobre 10,6 GiB seria caro demais sem ganho.
- Dicts: `snort_100`, `snort_1k`, `snort`, `et_32`.
- Por dict: `sequential`, `sequential_flat` (single) + `flat`,
  `dynamic_flat` × T∈{1,32} (campeão estático vs. dinâmico, ambos flat).
- 4 × 6 = **24 runs.**

### D' — per-thread (`enron_x8`, `snort`, T=32, --per-thread, warmup=1, iters=3)
- 4 paralelos núcleo (`v2`, `flat`, `dynamic`, `dynamic_flat`) = **4 runs.**
- Lê aqui o contraste central: ociosidade de barreira do chunking estático
  (`v2`/`flat`) vs. do bag-of-tasks (`dynamic`/`dynamic_flat`) em cores
  homogêneos. O spread de `tasks_done` por worker quantifica o
  balanceamento dinâmico.

### E' — build paralelo (`enron_corpus`, warmup=0, iters=1)
- Dicts: `snort_100`, `snort_1k`, `snort`, `et_32`.
- Por dict: 1 `buildseq` + `buildpar` em T∈{2,4,8,16,32} = 6.
- 4 × 6 = **24 runs.**

### (Opcional) S' — 2-D / sharding (`enron_x8`, `snort`, warmup=2, iters=5)
Só se sobrar janela; alimenta o trabalho-futuro da conclusão.
- `pthread_2d_sharded_chunked`, `pattern_sharded_prefix` × T∈{1,32} = **4 runs.**

| Bloco | Runs |
|-------|-----:|
| A' escalonamento | 60 |
| B' footprint | 24 |
| D' per-thread | 4 |
| E' build | 24 |
| **Núcleo** | **112** |
| S' opcional | +4 |

Contra 265 do sweep completo: ~**42%** das runs, concentradas no que é
portável + no contraste chunking-estático vs. bag-of-tasks (`flat` vs.
`dynamic_flat`).

## Pré-flight (antes do sweep grande)

0. **Provisionamento de dados (CRÍTICO — corrida única).** `data/` é
   gitignored: um `git clone` numa máquina alugada **não** traz os corpora.
   Pior, **`enron_x8.txt` e `patterns_et_32.txt` não são gerados por nenhum
   script** (o `prepare_datasets.sh` só cobre Snort + Enron 1×). Sem eles, as
   fases A/D/E **falham em massa**. Use o pré-flight dedicado:

   ```bash
   ./scripts/prepare_workstation_data.sh
   ```

   Ele: (a) gera `enron_x8.txt` = `enron_corpus.txt` × 8 (checa disco e
   tamanho exato); (b) gera `patterns_snort_100/1k.txt` via `head`; (c) baixa
   Snort/Enron se faltarem (`AUTO_DOWNLOAD=1`); (d) **PARA** se
   `patterns_et_32.txt` faltar — esse arquivo **não é regenerável**, copie do
   laptop (`scp laptop:.../data/patterns_et_32.txt data/`). Só siga adiante
   quando o veredito for **PRONTO**. Transferência mínima recomendada:
   `enron_corpus.txt` + `patterns_*.txt` (~1,42 GiB) e gerar `enron_x8` no
   host (evita mover 10,6 GiB).
1. `lscpu` / `lstopo` — confirmar 16C/32T e o layout **2 CCDs × 32 MiB L3**
   (relevante para interpretar o cross-over de footprint). O sweep já captura
   `lscpu` completo, `numactl --hardware`, `lstopo`, `dmidecode -t memory`,
   THP e cmdline em `runs/workstation/env/` (alimenta a tabela de hardware).
2. `make && make test` — correção em `{1,2,4,8,16,24,32}` neste chip.
3. Governor de performance: no Zen 5 é `amd-pstate`; use
   `sudo cpupower frequency-set -g performance` (ou `performance` via
   `amd_pstate=active`). Fechar IDE/browser. RAM: garanta ≳12–16 GiB livres
   (`enron_x8` é mmap'd com `MAP_POPULATE` → 10,6 GiB residentes).
4. `pthread_dynamic_flat` já registrado e validado (test + tsan em
   2026-06-05); reconfirmar `make test` no chip-alvo basta.

## Como rodar (driver enxuto)

Script dedicado: **`scripts/run_workstation_sweep.sh`** (irmão do overnight,
herda lock/resume/env-snapshot/thermal). Implementa exatamente esta grade
e descarta `v3`/`v3_flat`.

```bash
./scripts/prepare_workstation_data.sh            # PRÉ-FLIGHT DE DADOS (deve dar PRONTO)
sudo cpupower frequency-set -g performance       # Zen 5 = amd-pstate
make && make test                                # correção no chip-alvo
nohup ./scripts/run_workstation_sweep.sh > workstation.out 2>&1 &
```

- Default roda `A B D E` (a fase opcional `S` de 2-D/sharding só entra com
  `PHASES="A B D E S"`).
- Os pontos de thread se adaptam ao `nproc` (ou a `MAX_THREADS=…`): em 32
  threads a curva é `{1,2,4,8,16,24,32}`.
- Saída em `runs/workstation/<fase>/`; rerodar pula o que já completou.
- **Versionável:** `runs/workstation/` tem exceção no `.gitignore` (o resto de
  `runs/` segue ignorado). Ao terminar, o script roda os extratores e deixa
  `runs/workstation/sweep.{csv,db}` prontos; basta
  `git add runs/workstation && git commit`.
- **Análise:** consulte via `sqlite3 runs/workstation/sweep.db` com as mesmas
  views do overnight (`v_speedup`, `v_self_speedup`, `v_footprint`, `v_build`,
  `v_best`, `v_correctness`). A correção é auto-checada: `v_correctness` deve
  dar `distinct_match_counts = 1` por `(patterns, corpus)`.

## Estimativa de custo (ordem de grandeza)

Dominado pelas 4 baselines sequenciais de A' sobre `enron_x8` (10,6 GiB):
`snort` seq ~6 min/run, `et_32` seq ~12 min/run (× iters=8 + warmup). Runs
paralelas em T alto são muito mais rápidas; o 9950X (Zen 5, DDR5, 2 CCDs)
deve reduzir tudo bastante vs. o laptop. Estimativa grosseira do núcleo:
**poucas horas**, B'/E' somam ~1h, D' minutos.
