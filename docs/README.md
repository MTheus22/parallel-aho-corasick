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
| [`searchers/`](searchers/)       | Uma página por implementação registrada do contrato `ac_searcher_t` (`sequential`, `pthread_chunked`). |
| `testes-workstation.md` · `workstation-analysis.md` · `workstation.md` · `TODO.md` | Corrida de portabilidade na workstation (Ryzen 9 9950X): plano + resultados (§9), análise i5 × Ryzen, parecer de execução e melhorias pendentes. Fonte de verdade do TCC continua sendo `runs/i5/sweep.db` (i5). |
| `i5-rerun-2026-06-28.md` | Segunda corrida do i5 (headless, máquina fria) em `runs/i5_2026-06-28/` — validação de reprodutibilidade + 1ª execução da fase G. **Não** é canônica; conclui que o pico de speedup no i5 saturado tem ±40–65% de variância entre corridas (invisível ao cv intra-run). |

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
