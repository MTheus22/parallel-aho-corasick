# Searchers

Esta pasta documenta cada implementação do contrato `ac_searcher_t`
registrada no laboratório. Todo searcher recebe a mesma entrada
(automato read-only + texto + configuração) e produz a mesma saída
(lista de matches), permitindo comparação 1:1 contra o baseline
sequencial.

## Contrato resumido

```c
int (*search)(const ac_automaton_t *aut,
              const char *text, size_t text_len,
              const ac_searcher_config_t *cfg,
              ac_match_list_t *out_matches,
              ac_thread_metric_t **out_thread_metrics,
              size_t *out_num_thread_metrics);
```

- O autômato (`aut`) é **estritamente read-only** após a construção;
  qualquer número de threads pode lê-lo sem sincronização.
- A lista `out_matches` recebe todos os matches encontrados (qualquer
  ordem; o harness compara após `ac_match_list_sort`).
- Métricas por thread são **opcionais** — searchers sequenciais devem
  retornar `NULL/0`.

## Searchers documentados

| Documento                                | Searcher           |
|------------------------------------------|--------------------|
| [`sequential.md`](sequential.md)         | `sequential`       |
| [`pthread_chunked.md`](pthread_chunked.md) | `pthread_chunked` |

## Comparação rápida

| Aspecto                | `sequential`              | `pthread_chunked`                                |
|------------------------|---------------------------|--------------------------------------------------|
| Paralelismo            | nenhum                    | data-parallel (chunks contíguos)                 |
| Threads                | 1                         | configurável (`--threads N`, default = nproc)    |
| Locks no caminho quente| —                         | nenhum (listas thread-local + autômato read-only)|
| Overhead extra         | nenhum                    | warm-up de `max_pattern_len - 1` bytes por worker|
| Quando degrada         | sempre O(n) — referência  | textos muito curtos: cai para sequencial          |
| Bom para               | correção, baseline        | corpus grande (≥ MB), nº de cores ≥ 2            |
