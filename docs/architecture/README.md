# Arquitetura

Esta pasta concentra o material conceitual: como o sistema é
organizado, quais são os contratos entre componentes e como esses
contratos sustentam o paralelismo sem locks.

## Índice

| Documento                                                      | Conteúdo                                                                 |
|----------------------------------------------------------------|--------------------------------------------------------------------------|
| [`overview.md`](overview.md)                                   | Visão geral, módulos, fluxo do CLI end-to-end.                           |
| [`automaton.md`](automaton.md)                                 | Construção sequencial: trie, função de transição plana, fail, `dict_suffix`. |
| [`parallelism.md`](parallelism.md)                             | Modelo de paralelismo, overlap, ownership, ausência de locks.           |
| [`benchmark-harness.md`](benchmark-harness.md)                 | `bench_now_ns`, `bench_run`, métricas por thread, política de warm-up. |
| [`benchmark-protocol.md`](benchmark-protocol.md)               | Protocolo experimental para o TCC: sweeps, métricas, cuidados.          |
| [`datasets.md`](datasets.md)                                   | Datasets, scripts de aquisição, extração de padrões.                    |

## Como ler

1. Comece pelo [`overview.md`](overview.md) — ele referencia todos os
   outros documentos no contexto correto.
2. Se você vai mexer no autômato ou em qualquer searcher, leia
   [`automaton.md`](automaton.md) antes de qualquer outra coisa.
3. Se você vai escrever um searcher paralelo novo,
   [`parallelism.md`](parallelism.md) explica o que **precisa** ser
   respeitado para a sua variante ser segura e justa nos benchmarks.
