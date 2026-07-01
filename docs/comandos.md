# Compêndio de comandos — `parallel-aho-corasick`

Lista de comandos prontos para copy-paste, cobrindo as combinações de
**searcher × padrão × texto × thread count × variável de ambiente** relevantes
para o TCC. Cada bloco é independente — pinçe o que interessa.

Pré-condições:

```bash
cd ~/projects/idp/tcc/parallel-aho-corasick
make                   # release build (-O3 -march=native)
./build/aclab --list   # lista todos os 11 searchers registrados
```

---

## §1 · Inventário rápido

### Searchers (11)

| Família | Searcher | Característica |
|---|---|---|
| **Baseline** | `sequential` | Single-thread, referência de correção |
| **Idea 5 (single)** | `sequential_flat` | Sequential + emissão flat |
| **Pthreads (chain-walk)** | `pthread_chunked` | v1: chunks iguais |
| | `pthread_chunked_v2` | + split warm-up/owned + cache-pad |
| | `pthread_chunked_v3` | + topology-aware affinity + freq-weighted chunks |
| | `pthread_dynamic` | dispatch atômico (4N tarefas) |
| | `pthread_prefetch` | + `__builtin_prefetch(text + Δ)` |
| **Idea 5 (multi)** | `pthread_chunked_flat` | v2 + flat output table |
| **Idea 7** | `pthread_chunked_v3_flat` ⭐ | v3 + flat (melhor composição) |
| **Idea 1 (sharding)** | `pattern_sharded_prefix` | bucket pelo primeiro byte |
| **Idea 6 (2-D)** | `pthread_2d_sharded_chunked` | K shards × N chunks |

### Padrões (dicionários)

| Arquivo | # padrões | Footprint | Cenário |
|---|---:|---|---|
| `data/patterns_snort_100.txt` | 100 | ~2 MiB | cache-friendly, cabe em L2 |
| `data/patterns_snort_1k.txt` | 1.000 | ~12 MiB | borda de L3 |
| `data/patterns_snort.txt` | 4.188 | ~56 MiB | **default do TCC** |
| `data/patterns_et_32.txt` | 44.678 | ~515 MiB | 42× L3 — DRAM-bound |

### Corpora (texto)

| Arquivo | Tamanho | Cenário |
|---|---:|---|
| `data/simplewiki.txt` | 1,19 GiB | iteração rápida |
| `data/enron_corpus.txt` | 1,4 GiB | **canônico do TCC** |
| `data/enron_x8.txt` | 10,6 GiB | medições de alta estabilidade |

### Variáveis de ambiente

| Env var | Efeito |
|---|---|
| `AC_BUILD_PARALLEL=1` | Usa `ac_automaton_build_par()` (idea 4) |
| `AC_BUILD_THREADS=N` | Threads para o build paralelo. Default: `nproc` |
| `AC_2D_K=K` | Override do K em `pthread_2d_sharded_chunked`. Default: 2 |

---

## §2 · Um comando por searcher

Default: `--warmup 1 --iters 5`, Snort full + Enron, T=12.

```bash
# Baselines single-thread
./build/aclab --patterns data/patterns_snort.txt --input data/enron_corpus.txt \
    --searcher sequential --warmup 1 --iters 5
./build/aclab --patterns data/patterns_snort.txt --input data/enron_corpus.txt \
    --searcher sequential_flat --warmup 1 --iters 5

# Família chain-walk
./build/aclab --patterns data/patterns_snort.txt --input data/enron_corpus.txt \
    --searcher pthread_chunked --threads 12 --warmup 1 --iters 5
./build/aclab --patterns data/patterns_snort.txt --input data/enron_corpus.txt \
    --searcher pthread_chunked_v2 --threads 12 --warmup 1 --iters 5
./build/aclab --patterns data/patterns_snort.txt --input data/enron_corpus.txt \
    --searcher pthread_chunked_v3 --threads 12 --warmup 1 --iters 5
./build/aclab --patterns data/patterns_snort.txt --input data/enron_corpus.txt \
    --searcher pthread_dynamic --threads 12 --warmup 1 --iters 5
./build/aclab --patterns data/patterns_snort.txt --input data/enron_corpus.txt \
    --searcher pthread_prefetch --threads 12 --warmup 1 --iters 5

# Família flat (ideas 5 e 7)
./build/aclab --patterns data/patterns_snort.txt --input data/enron_corpus.txt \
    --searcher pthread_chunked_flat --threads 12 --warmup 1 --iters 5
./build/aclab --patterns data/patterns_snort.txt --input data/enron_corpus.txt \
    --searcher pthread_chunked_v3_flat --threads 12 --warmup 1 --iters 5

# Sharding de dicionário (ideas 1 e 6)
./build/aclab --patterns data/patterns_snort.txt --input data/enron_corpus.txt \
    --searcher pattern_sharded_prefix --threads 8 --warmup 1 --iters 5
./build/aclab --patterns data/patterns_snort.txt --input data/enron_corpus.txt \
    --searcher pthread_2d_sharded_chunked --threads 12 --warmup 1 --iters 5
```

### Rodar todos de uma vez

Omita `--searcher` para rodar todos os 11 sequencialmente. Use `min(ms)` para
comparar — o `mean` é inflado por throttling térmico nos searchers posteriores.

```bash
./build/aclab --patterns data/patterns_snort.txt --input data/enron_corpus.txt \
    --threads 12 --warmup 1 --iters 3
```

---

## §3 · Dimensão `patterns` — isolar o efeito do footprint de cache

Fixa corpus e searcher, varia o dicionário para mostrar a curva
cache-friendly → L3 → DRAM-bound.

```bash
for P in patterns_snort_100 patterns_snort_1k patterns_snort patterns_et_32; do
    echo "=== $P ==="
    ./build/aclab --patterns "data/$P.txt" --input data/enron_corpus.txt \
        --searcher pthread_chunked_v3_flat --threads 12 --warmup 1 --iters 3
done
```

Baseline correspondente:

```bash
for P in patterns_snort_100 patterns_snort_1k patterns_snort patterns_et_32; do
    echo "=== seq $P ==="
    ./build/aclab --patterns "data/$P.txt" --input data/enron_corpus.txt \
        --searcher sequential --warmup 1 --iters 3
done
```

---

## §4 · Dimensão `corpus` — sensibilidade ao texto

Snort full como padrão fixo, varia o corpus.

```bash
for C in simplewiki enron_corpus enron_x8; do
    echo "=== corpus $C ==="
    ./build/aclab --patterns data/patterns_snort.txt --input "data/$C.txt" \
        --searcher pthread_chunked_v3_flat --threads 12 --warmup 1 --iters 3
done
```

> **`enron_x8`** (10,6 GiB): cada iteração leva ~25 s. Use `--iters 2` para
> runs rápidas.

---

## §5 · Dimensão `--threads` — curva de escalabilidade

```bash
for T in 1 2 3 4 6 8 10 12; do
    echo "=== T=$T ==="
    ./build/aclab --patterns data/patterns_snort.txt --input data/enron_corpus.txt \
        --searcher pthread_chunked_v3_flat --threads "$T" --warmup 1 --iters 3
done
```

---

## §6 · Build paralelo (idea 4)

`AC_BUILD_PARALLEL=1` ativa `ac_automaton_build_par()`. O autômato resultante
é bit-idêntico ao sequencial — funciona com qualquer searcher.

### 6.1 · Build sequencial vs paralelo isolado

ET-32 (~509k estados) é onde o build paralelo tem ganho; Snort (~55k) tem
overhead > ganho.

```bash
echo "--- ET-32 build sequencial ---"
./build/aclab --patterns data/patterns_et_32.txt --input data/enron_corpus.txt \
    --searcher sequential --warmup 1 --iters 3

echo "--- ET-32 build paralelo T=4 ---"
AC_BUILD_PARALLEL=1 AC_BUILD_THREADS=4 \
./build/aclab --patterns data/patterns_et_32.txt --input data/enron_corpus.txt \
    --searcher sequential --warmup 1 --iters 3

echo "--- ET-32 build paralelo T=12 ---"
AC_BUILD_PARALLEL=1 AC_BUILD_THREADS=12 \
./build/aclab --patterns data/patterns_et_32.txt --input data/enron_corpus.txt \
    --searcher sequential --warmup 1 --iters 3
```

A linha `# build time: NNN.NN ms` no output dá o tempo de construção isolado.

### 6.2 · Sweep de `AC_BUILD_THREADS`

```bash
for BT in 1 2 4 8 12; do
    echo "=== build threads=$BT ==="
    AC_BUILD_PARALLEL=1 AC_BUILD_THREADS="$BT" \
    ./build/aclab --patterns data/patterns_et_32.txt --input data/enron_corpus.txt \
        --searcher sequential --warmup 1 --iters 2
done
```

### 6.3 · Stack completo — todas as ideias compostas

Cronometra **build + search end-to-end** sobre `enron_x8` (10,59 GiB).

```bash
# 1) baseline — sequential puro
./build/aclab --patterns data/patterns_snort.txt --input data/enron_x8.txt \
    --searcher sequential --warmup 1 --iters 3

# 2) champion — chunked_flat T=12, build seq
./build/aclab --patterns data/patterns_snort.txt --input data/enron_x8.txt \
    --searcher pthread_chunked_flat --threads 12 --warmup 1 --iters 3

# 3) F1 — v3_flat T=12, build seq
./build/aclab --patterns data/patterns_snort.txt --input data/enron_x8.txt \
    --searcher pthread_chunked_v3_flat --threads 12 --warmup 1 --iters 3

# 4) build par + chunked_flat
AC_BUILD_PARALLEL=1 AC_BUILD_THREADS=12 \
./build/aclab --patterns data/patterns_snort.txt --input data/enron_x8.txt \
    --searcher pthread_chunked_flat --threads 12 --warmup 1 --iters 3

# 5) stack completo: build par + v3_flat
AC_BUILD_PARALLEL=1 AC_BUILD_THREADS=12 \
./build/aclab --patterns data/patterns_snort.txt --input data/enron_x8.txt \
    --searcher pthread_chunked_v3_flat --threads 12 --warmup 1 --iters 3
```

Substitua `patterns_snort.txt` por `patterns_et_32.txt` para o regime DRAM-bound.

**Wall-clock E2E** = `build_ms + search_min_ms` — unidade comparável entre configs.

### 6.4 · Stack reduzido (sem `enron_x8`) — iteração rápida

```bash
# Com build paralelo
AC_BUILD_PARALLEL=1 AC_BUILD_THREADS=12 \
./build/aclab --patterns data/patterns_snort.txt --input data/enron_corpus.txt \
    --searcher pthread_chunked_v3_flat --threads 12 --warmup 1 --iters 5

# Sem build paralelo (comparação)
./build/aclab --patterns data/patterns_snort.txt --input data/enron_corpus.txt \
    --searcher pthread_chunked_v3_flat --threads 12 --warmup 1 --iters 5
```

### 6.5 · Build paralelo × matrix de searchers

```bash
for S in sequential sequential_flat pthread_chunked_flat pthread_chunked_v3_flat \
         pattern_sharded_prefix pthread_2d_sharded_chunked; do
    echo "=== build par + $S ==="
    AC_BUILD_PARALLEL=1 AC_BUILD_THREADS=12 \
    ./build/aclab --patterns data/patterns_et_32.txt --input data/enron_corpus.txt \
        --searcher "$S" --threads 12 --warmup 1 --iters 2
done
```

---

## §7 · `AC_2D_K` — K-sweep do `pthread_2d_sharded_chunked` (idea 6)

`T = K × N`. Padrão K=2; override via `AC_2D_K`.

```bash
# Snort + Enron, T=12 fixo, K varia
for K in 1 2 3 4 6 12; do
    echo "=== K=$K (N=$((12/K))) ==="
    AC_2D_K="$K" \
    ./build/aclab --patterns data/patterns_snort.txt --input data/enron_corpus.txt \
        --searcher pthread_2d_sharded_chunked --threads 12 --warmup 1 --iters 3
done

# ET-32 (onde sharding deveria ajudar mais)
for K in 1 2 3 4 6 12; do
    echo "=== ET-32 K=$K ==="
    AC_2D_K="$K" \
    ./build/aclab --patterns data/patterns_et_32.txt --input data/enron_corpus.txt \
        --searcher pthread_2d_sharded_chunked --threads 12 --warmup 1 --iters 2
done
```

---

## §8 · Comparação chain-walk vs flat — isolar o ganho da idea 5

```bash
# Single-thread
./build/aclab --patterns data/patterns_snort.txt --input data/enron_corpus.txt \
    --searcher sequential --warmup 1 --iters 5
./build/aclab --patterns data/patterns_snort.txt --input data/enron_corpus.txt \
    --searcher sequential_flat --warmup 1 --iters 5

# Multi-thread: mesmo chunking v2, só difere na emissão
./build/aclab --patterns data/patterns_snort.txt --input data/enron_corpus.txt \
    --searcher pthread_chunked_v2 --threads 12 --warmup 1 --iters 5
./build/aclab --patterns data/patterns_snort.txt --input data/enron_corpus.txt \
    --searcher pthread_chunked_flat --threads 12 --warmup 1 --iters 5
```

A diferença `mean(ms)` entre cada par é o delta atribuível exclusivamente à idea 5.

---

## §9 · Diagnóstico per-thread (balanceamento P/E-core)

```bash
# Chunks iguais — workers em E-cores chegam atrasados
./build/aclab --patterns data/patterns_snort.txt --input data/enron_corpus.txt \
    --searcher pthread_chunked --threads 12 --warmup 1 --iters 1 --per-thread

# Chunks ponderados por frequência — convergência ideal
./build/aclab --patterns data/patterns_snort.txt --input data/enron_corpus.txt \
    --searcher pthread_chunked_v3 --threads 12 --warmup 1 --iters 1 --per-thread

# Composição: chunks ponderados + flat
./build/aclab --patterns data/patterns_snort.txt --input data/enron_corpus.txt \
    --searcher pthread_chunked_v3_flat --threads 12 --warmup 1 --iters 1 --per-thread
```

---

## §10 · Validar correção (`--print-matches`)

```bash
# Referência canônica
./build/aclab --patterns data/patterns_snort_100.txt --input data/simplewiki.txt \
    --searcher sequential --warmup 0 --iters 1 --print-matches > /tmp/ref.txt

# Paralelo a comparar
./build/aclab --patterns data/patterns_snort_100.txt --input data/simplewiki.txt \
    --searcher pthread_chunked_v3_flat --threads 8 --warmup 0 --iters 1 \
    --print-matches > /tmp/v3flat.txt

diff -q /tmp/ref.txt /tmp/v3flat.txt && echo "OK: outputs idênticos"
```

`make test` automatiza isso para todos os 11 searchers.

---

## §11 · Modos de build e teste

```bash
make              # release (-O3 -march=native)
make debug        # -O0 -g3 + asserts
make asan         # AddressSanitizer + UBSan
make tsan         # ThreadSanitizer (data races)
make test         # suite de correção (todos os searchers vs sequential)
make clean
```

---

## §12 · `perf stat` — diagnóstico microarquitetural

```bash
perf stat -e cycles,instructions,cache-references,cache-misses,\
branches,branch-misses,LLC-loads,LLC-load-misses \
./build/aclab --patterns data/patterns_snort.txt --input data/enron_corpus.txt \
    --searcher pthread_chunked_v3_flat --threads 12 --warmup 1 --iters 3
```

Para comparar chain-walk vs flat em nível de LLC:

```bash
for S in pthread_chunked_v2 pthread_chunked_flat pthread_chunked_v3_flat; do
    echo "=== $S ==="
    perf stat -e LLC-loads,LLC-load-misses \
    ./build/aclab --patterns data/patterns_et_32.txt --input data/enron_corpus.txt \
        --searcher "$S" --threads 12 --warmup 1 --iters 2
done
```

---

## §13 · `taskset` — pinning manual do processo

```bash
# Apenas P-cores (CPUs 0-3 no i5-1235U)
taskset -c 0,1,2,3 ./build/aclab --patterns data/patterns_snort.txt \
    --input data/enron_corpus.txt --searcher pthread_chunked_v3_flat \
    --threads 4 --warmup 1 --iters 3

# Apenas E-cores (CPUs 4-11)
taskset -c 4,5,6,7,8,9,10,11 ./build/aclab --patterns data/patterns_snort.txt \
    --input data/enron_corpus.txt --searcher pthread_chunked_v3_flat \
    --threads 8 --warmup 1 --iters 3

# 1 P-core + 1 E-core (isolar heterogeneidade com T=2)
taskset -c 0,4 ./build/aclab --patterns data/patterns_snort.txt \
    --input data/enron_corpus.txt --searcher pthread_chunked_v3 \
    --threads 2 --warmup 1 --iters 3
```

---

## §14 · Sweep canônico UNIFICADO (`run_sweep.sh`)

Motor de sweep **unificado e env-agnóstico** (substitui `run_i5_sweep.sh` e
`run_workstation_sweep.sh`, agora legados). Roda a **grade completa** A–G em
qualquer host, derivando `MAX_T=nproc` e o `RUN_DIR` (slug do modelo de CPU, ex.
`runs/amd_ryzen_9_9950x`). Em `MAX_T=32`, o default atual dispara 562 runs.
Resume automático — pula runs já concluídas. Para cortar uma fase, passe
explicitamente a lista desejada em `PHASES`. Falhas individuais viram `.FAIL`;
o sweep continua para as demais configs, mas retorna rc=1 se alguma falha
permanecer ao fim.

Use `run_sweep.sh` quando o ambiente já estiver preparado e você quiser apenas
o motor de benchmark. Para corrida real/noturna, prefira `run_all.sh`: ele
executa pré-flight de dados, build/test, governador, desacoplamento de sessão e
upload/notificação best-effort antes/depois de chamar `run_sweep.sh`.

```bash
# Tudo, RUN_DIR auto pelo modelo de CPU
nohup scripts/run_sweep.sh > sweep.out 2>&1 &

# Forçar o RUN_DIR (ex.: workstation canônica do epic-03)
RUN_DIR=runs/workstation nohup scripts/run_sweep.sh > sweep.out 2>&1 &

# Fase A só; ou fases específicas
PHASES="A" scripts/run_sweep.sh
PHASES="B C D" scripts/run_sweep.sh

# Pontos de curva custom (filtrados a 1..MAX_T)
THREAD_POINTS="1 2 4 8 16 24 32" scripts/run_sweep.sh

# Fase G isolada: granularidade da fila dinâmica — varre tasks-per-thread
# {1,4,16,64,256} para dynamic/dynamic_flat (P1). Saída em <RUN_DIR>/G_granularity/.
PHASES="G" nohup scripts/run_sweep.sh > sweep.out 2>&1 &

# Acompanhar progresso (ajuste o RUN_DIR)
tail -f runs/<run_dir>/MASTER.log
```

### §14b · Corrida "um comando" desacoplada (`run_all.sh`)

Wrapper **env-agnóstico** (substitui `i5_all.sh`/`workstation_all.sh`). Faz pull
opcional (`AC_GIT_PULL=1`), pré-flight de dados (`prepare_data.sh`), governador
`performance` (agnóstico: `amd-pstate`/`intel_pstate`), build + `make test`, e
então **desacopla** o sweep (imune a logout/suspensão), imprimindo os comandos de
log. Ao terminar, faz upload/notificação best-effort.

```bash
# i5 (TTY texto pós-reboot, protocolo de máquina quieta), sem upload
RUN_DIR=runs/i5 ./scripts/run_all.sh                 # default A B C D E G
RUN_DIR=runs/i5_granularidade PHASES="G" ./scripts/run_all.sh   # só granularidade

# Workstation Ryzen 9 9950X: grade completa + push da runs/ + notificação no celular
RUN_DIR=runs/workstation_YYYY-MM-DD \
AC_GIT_PULL=1 AC_GIT_PUSH=1 AC_GH_PAT=github_pat_xxx \
AC_NOTIFY='curl -d "TCC sweep pronto (rc=$AC_RC): $AC_RESULTS" ntfy.sh/SEU-TOPICO' \
  ./scripts/run_all.sh
```

> `AC_GIT_PUSH`+`AC_GH_PAT` (PAT só por ambiente; push em `HEAD:main`),
> `AC_UPLOAD_CMD` (`$AC_RESULTS`=.tgz) e `AC_NOTIFY` (`$AC_RC`/`$AC_RESULTS`) são
> independentes e best-effort — falha em qualquer um não derruba o sweep.

Pós-sweep — gerar artefatos de análise e consultar (ver `runs/QUERY_GUIDE.md`):

```bash
RUN_DIR=runs/workstation_YYYY-MM-DD
python3 scripts/extract_sweep_csv.py "$RUN_DIR" -o "$RUN_DIR/sweep.csv" --known-only
python3 scripts/build_sweep_db.py    "$RUN_DIR/sweep.csv" -o "$RUN_DIR/sweep.db"

# Token-efficient: rode SELECTs nas views (v_speedup, v_footprint, v_build, …)
sqlite3 -header -column "$RUN_DIR/sweep.db" \
    "SELECT thr, searcher, speedup_vs_seq FROM v_speedup
     WHERE patterns='patterns_snort' AND corpus='enron_corpus'
     ORDER BY thr, speedup_vs_seq DESC;"
```

---

## §15 · Matriz canônica do TCC

Quatro regimes × searchers principais × T={1,12}:

```bash
REGIMES=(
    "data/patterns_snort.txt|data/simplewiki.txt|R1-snort-wiki"
    "data/patterns_snort.txt|data/enron_corpus.txt|R2-snort-enron"
    "data/patterns_snort_100.txt|data/enron_corpus.txt|R3-snort100-enron"
    "data/patterns_et_32.txt|data/enron_corpus.txt|R4-et32-enron"
)
SEARCHERS=(sequential pthread_chunked_v3 pthread_chunked_flat pthread_chunked_v3_flat)

for r in "${REGIMES[@]}"; do
    IFS='|' read -r PATS TEXT LABEL <<< "$r"
    for S in "${SEARCHERS[@]}"; do
        for T in 1 12; do
            [[ "$S" == "sequential" && "$T" == "12" ]] && continue
            echo "=== $LABEL · $S · T=$T ==="
            ./build/aclab --patterns "$PATS" --input "$TEXT" \
                --searcher "$S" --threads "$T" --warmup 1 --iters 3
        done
    done
done
```

---

## §16 · Comandos compostos comuns

### Validação rápida pós-mudança

```bash
make && make test && echo "✓ build + correção"
```

### A/B rápido entre dois searchers

```bash
./build/aclab --patterns data/patterns_snort.txt --input data/enron_corpus.txt \
    --searcher pthread_chunked_flat --threads 12 --warmup 1 --iters 5 \
    | tee /tmp/before.txt

./build/aclab --patterns data/patterns_snort.txt --input data/enron_corpus.txt \
    --searcher pthread_chunked_v3_flat --threads 12 --warmup 1 --iters 5 \
    | tee /tmp/after.txt

diff -u /tmp/before.txt /tmp/after.txt
```

### Stack maximalista (todas as ideias)

```bash
AC_BUILD_PARALLEL=1 AC_BUILD_THREADS=12 \
./build/aclab --patterns data/patterns_snort.txt --input data/enron_corpus.txt \
    --searcher pthread_chunked_v3_flat --threads 12 --warmup 1 --iters 5
# Ideas: 4 (build par) + 5 (flat) + 7 (v3 + flat)
```

### Watch ao vivo (recompila e roda a cada mudança em src/)

```bash
ls src/**/*.c src/*.c | entr -c sh -c \
    'make && ./build/aclab --patterns data/patterns_snort_100.txt \
        --input data/simplewiki.txt --searcher pthread_chunked_v3_flat \
        --threads 4 --warmup 0 --iters 1'
```

---

## Tabela-índice

| Eu quero… | Vá para |
|---|---|
| Inventário de searchers/patterns/corpus | §1 |
| Rodar um searcher | §2 |
| Rodar todos de uma vez | §2 |
| Variar dicionário (footprint) | §3 |
| Variar corpus | §4 |
| Variar T (curva de scaling) | §5 |
| Build paralelo + qualquer search | §6 |
| Stack end-to-end (F3) | §6.3 |
| K-sweep do 2-D sharded | §7 |
| Ganho isolado do flat (idea 5) | §8 |
| Ver per-thread (P/E balance) | §9 |
| Validar correção (multiset) | §10 |
| Sanitizers (asan/tsan) | §11 |
| Cache miss rate via perf | §12 |
| Pinning manual P/E-cores | §13 |
| Sweep automatizado (i5) | §14 |
| Analisar resultados (CSV/SQLite) | §14 |
| Matriz canônica do TCC | §15 |
| Stack maximalista | §16 |
