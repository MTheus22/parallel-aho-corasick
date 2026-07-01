# Task 04 — (OPCIONAL) Instrumentar `--per-thread` com CPU físico

## Objetivo

Adicionar o **CPU físico** (e, onde barato, a classe P/E) em que cada worker
rodou às métricas per-thread, para **atribuir** a variância — não só medi-la.
**Opcional e não-bloqueante:** no Ryzen 9950X homogêneo o spread de `seconds` já
basta; esta task só compensa se o épico for estendido ao **i5 híbrido** ou se o
P1 "métricas" for absorvido aqui.

## Escopo

- **In scope:** `include/ac_searcher.h` (`ac_thread_metric_t`), o preenchimento
  nos searchers paralelos que já emitem métricas, e o printer em `src/main.c`
  (linha ~380).
- **Out of scope:** frequência efetiva (APERF/MPERF), migrações/ctx-switches
  (ficam no P1 "métricas" amplo); qualquer mudança na assinatura de
  `ac_searcher_t::search` (proibido — quebra todos os searchers).

## Implementação

1. **Struct** (`include/ac_searcher.h:16`): adicione um campo ao final para não
   deslocar os existentes:
   ```c
   typedef struct {
       int    thread_id;
       double seconds;
       size_t bytes_scanned;
       size_t matches_found;
       int    cpu;            /* sched_getcpu() no fim do laço; -1 se indisponível */
   } ac_thread_metric_t;
   ```
2. **Preenchimento:** nos workers que já populam métricas (ex.:
   `pthread_chunked_v2`, `pthread_dynamic*`, os `*_flat`), grave
   `m.cpu = sched_getcpu();` **após** o laço quente (uma amostra é suficiente;
   evite chamar dentro do hot path). Requer `_GNU_SOURCE` (já é padrão do repo).
   Searchers que deixam métricas NULL seguem sem mudança.
3. **Printer** (`src/main.c:380`): acrescente `cpu=%d` à linha per-thread:
   ```c
   printf("    [t%02d] %.3f ms  %zu bytes  %zu matches  %.2f MB/s  cpu=%d\n",
          …, tm[k].cpu);
   ```
   Mantenha o prefixo `[tNN]` e a ordem dos campos anteriores para não quebrar o
   parser da Task 03 (que deve ler `cpu=` como opcional).
4. **Parser (Task 03):** trate `cpu=` como campo opcional — logs antigos sem ele
   continuam válidos.

## Validação

```bash
make && make test          # correção intacta em todos os searchers e T
make tsan                  # sched_getcpu() fora do hot path não introduz race
build/aclab --patterns data/patterns_snort.txt --input data/enron_corpus.txt \
  --searcher pthread_dynamic_flat --threads $(nproc) --warmup 1 --iters 1 --per-thread \
  | grep -E '^\s+\[t'      # linhas agora terminam em cpu=<n>
```

## Critérios de Aceite

- `make test` passa (paridade com `sequential` em todos os T) e `make tsan`
  fica limpo.
- A linha per-thread exibe `cpu=<n>` com valor plausível (0..nproc-1).
- O campo é o **último** da struct e da linha impressa (não desloca campos
  existentes); o parser da Task 03 lê logs com e sem `cpu=`.
- **Se pulada:** registrar em `progress.md` que foi dispensada (Ryzen homogêneo)
  — não é bloqueante para as Tasks 05/06.
