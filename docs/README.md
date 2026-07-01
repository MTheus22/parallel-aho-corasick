# Documentação

Material conceitual completo do laboratório. Para o arranque rápido
(o que é, como builda, como roda), comece pelo
[`../README.md`](../README.md). Para agentes (Claude e afins), use
[`../AGENTS.md`](../AGENTS.md).

Este conjunto de documentos é também a fonte de verdade dos
fundamentos descritos no TCC
*"Acceleration of Pattern Matching Algorithms Using Parallel
Programming"* (texto LaTeX em
[`../../acceleration-of-pattern-matching-algorithms-using-parallel-programming`](../../acceleration-of-pattern-matching-algorithms-using-parallel-programming)).
Em particular, o modelo de paralelismo (chunks com overlap, listas
thread-local, ausência de locks) descrito aqui é o mesmo apresentado
no Capítulo de Proposta da dissertação.

## Mapa

| Pasta                            | Conteúdo                                                          |
|----------------------------------|-------------------------------------------------------------------|
| [`architecture/`](architecture/) | Arquitetura do sistema, autômato, modelo de paralelismo, harness de benchmark, protocolo experimental do TCC e datasets. |
| [`searchers/`](searchers/)       | Uma página por implementação registrada do contrato `ac_searcher_t`. |
| [`../runs/MANIFEST.md`](../runs/MANIFEST.md) | **Política dos runs preservados** (`workstation_2026-06-30/` canônico + `i5/` P/E) e lacunas conhecidas. |
| [`../runs/QUERY_GUIDE.md`](../runs/QUERY_GUIDE.md) | Schema, views e consultas SQLite para os `sweep.db` preservados, incluindo métricas por worker. |
| [`../runs/workstation_2026-06-30/RESULTS.md`](../runs/workstation_2026-06-30/RESULTS.md) | Análise consolidada da coleta canônica; campeão `pthread_dynamic_flat`; mapa de leitura humana por fase. |
| [`sweep-test-inventory.md`](sweep-test-inventory.md) | Inventário das fases do sweep e contagem de runs por `MAX_T`. |
| [`TODO.md`](TODO.md) | Lacunas vivas: réplicas por config, corpus skewed e instrumentação complementar. |

## Ordem sugerida de leitura

1. [`architecture/overview.md`](architecture/overview.md) — visão de
   sistema com diagramas (fluxo end-to-end, módulos, ownership).
2. [`architecture/automaton.md`](architecture/automaton.md) — como o
   autômato é construído e por que ele pode ser compartilhado por
   ponteiro entre threads.
3. [`architecture/parallelism.md`](architecture/parallelism.md) —
   invariantes obrigatórios para qualquer novo searcher paralelo.
4. [`searchers/sequential.md`](searchers/sequential.md) e
   [`searchers/pthread_chunked.md`](searchers/pthread_chunked.md) —
   o baseline e a primeira variante paralela.
5. [`architecture/benchmark-harness.md`](architecture/benchmark-harness.md)
   e [`architecture/benchmark-protocol.md`](architecture/benchmark-protocol.md)
   — como o harness mede e como reportar números defensáveis para a
   dissertação.
6. [`architecture/datasets.md`](architecture/datasets.md) — origem e
   preparação dos datasets (Snort + Enron, Simple Wikipedia).
