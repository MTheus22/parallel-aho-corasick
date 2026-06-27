# Workstation analysis — Ryzen 9 9950X × i5-1235U

Análise consolidada e objetiva da corrida de portabilidade na **AMD Ryzen 9
9950X** (`runs/workstation/sweep.db`, 2026-06-25) confrontada com o sweep
canônico do **Intel i5-1235U** (`runs/i5/sweep.db`, 2026-05-29). Ambos:
governador `performance`, binário nativo (`-O3 -march=native`), corpus Enron ×8
(10,6 GiB), dicionários Snort (55 MiB) e ET-32 (507 MiB), correção validada
(match-count único por `(dicionário, corpus)`: 142.831.896 em Snort,
466.943.872 em ET-32), zero falhas.

O **i5 permanece a fonte de verdade** das tabelas/figuras do corpo da tese; a
workstation entra como **seção de portabilidade/generalização**. Curvas brutas
por variante × threads estão em [`testes-workstation.md`](testes-workstation.md)
§9. Este doc concentra a camada analítica (eficiência, comparação i5×Ryzen,
confronto com a tese).

> Nota de baseline: `MB/s` segue a convenção do harness (bytes ÷ 2²⁰). Onde há
> Gbps, é decimal (ex.: 7.468 MB/s = 62,6 Gbps). Comparações single-thread usam
> `sequential` (T=1) nas duas máquinas.

---

## 1. Hardware lado a lado

| Item | i5-1235U | Ryzen 9 9950X |
|---|---|---|
| Microarquitetura | Alder Lake (híbrida) | Zen 5 (homogênea) |
| Núcleos / threads | 2 P-core HT + 8 E-core = **12 T** | 16 núcleos idênticos = **32 T** (SMT 2×) |
| L3 | **12 MiB unificado** | **64 MiB = 2 × 32 MiB por CCD** (não unificado) |
| Boost | ~4,4 GHz (P-core) | ~5,66 GHz |
| Memória | DDR4/LPDDR5 (laptop) | DDR5, 123 GiB (104 GiB livres na largada) |
| NUMA | 1 nó | 1 nó (2 CCDs, controlador compartilhado) |
| Topologia | **heterogênea** (P ≠ E) | **homogênea** |

---

## 2. Headline (i5 × Ryzen)

Campeão por máquina; Enron ×8; speedup vs. `sequential`.

| Eixo | i5-1235U | Ryzen 9 9950X |
|---|---|---|
| Pico speedup — Snort | 4,52× (T=12) | **22,72×** (T=32) |
| Pico speedup — ET-32 | 2,79× (T=12) | **19,79×** (T=32) |
| Vazão pico — Snort | 1.087 MB/s | **7.468 MB/s** (62,6 Gbps) |
| Vazão pico — ET-32 | 350 MB/s | **4.186 MB/s** (35,1 Gbps) |
| Campeão — Snort | `chunked_v3_flat` (topology-aware) | `dynamic_flat` |
| Campeão — ET-32 | `chunked_flat` | `dynamic_flat` |
| Balanceamento (cv% encadeado) | até **52,9%** | **0,10%** |
| Cache cliff (T=1, seq) | −74% | −67% |

> `dynamic_flat` **não foi medido no i5** (só `pthread_dynamic`); os campeões do
> i5 são as variantes acima. No Ryzen, `v3`/`v3_flat` não foram rodados (colapsam
> em `v2` sem P/E para balancear).

---

## 3. Escalabilidade e eficiência paralela (Snort, Enron ×8)

| T | i5 speedup | Ryzen speedup | Ryzen MB/s | Ryzen efic. |
|---:|---:|---:|---:|---:|
| 1 | 1,05× | 1,05× | 344 | — |
| 4 | 2,37× | 3,99× | 1.310 | — |
| 8 | 3,50× | 7,73× | 2.540 | **96,6%** |
| 12 | 4,47–4,52× (saturado) | — | — | — |
| 16 | — | 14,26× | 4.687 | **89,1%** |
| 24 | — | 18,05× | 5.935 | 75,2% |
| 32 | — | **22,72×** | **7.468** | 71,0% |

Joelhos físicos da curva do Ryzen:
- **≤ 8 threads:** cabem dentro de **1 CCD** (8 núcleos / 32 MiB L3) → quase ideal.
- **8 → 16:** atravessa os 2 CCDs (16 núcleos físicos) → 89,1% de eficiência.
- **16 → 32:** **SMT**; dobrar threads lógicos rende **1,59×** (4.687 → 7.468) —
  sob carga memory-bound, os 2 threads lógicos disputam a mesma porta de memória.

i5: satura em T=12 (só 2 P-cores; 8 E-cores rendem pouco; L3 de 12 MiB esgota cedo).

---

## 4. Eixos do estudo isolados (Ryzen, Snort, T=32)

| Variante | Distribuição | Emissão | Speedup |
|---|---|---|---:|
| `pthread_dynamic_flat` | dinâmica | flat | **22,72×** |
| `pthread_chunked_flat` | estática | flat | 22,35× |
| `pthread_dynamic` | dinâmica | encadeada | 21,70× |
| `pthread_chunked_v2` | estática | encadeada | 21,35× |

- **Emissão flat > encadeada:** +4,7% (dinâmica) / +4,9% (estática); presente já em T=1.
- **Distribuição dinâmica > estática:** +1,6% (flat) — sistemática, mas pequena.
- Estabilidade: cv% ≤ 0,32% (Snort) / ≤ 0,56% (ET-32) em toda a fase A.

---

## 5. Cache cliff (single-thread, T=1, `sequential`)

| Dicionário | Autômato | i5 MB/s | Ryzen MB/s |
|---|---:|---:|---:|
| Snort-100 | 1,9 MiB | 483 | 644 |
| Snort-1k | 11,9 MiB | 337 | 451 |
| Snort | 55,2 MiB | 241 | 329 |
| ET-32 | 506,8 MiB | 126 | 214 |
| **Queda** | | **−74%** | **−67%** |

Degradação estrutural e universal (~70% nas duas máquinas) quando o autômato
estoura a cache. Diferenças:
- **Piso absoluto:** Ryzen retém 214 MB/s no pior caso vs. 126 do i5 (DDR5).
- **Cross-over:** L3 de **32 MiB por CCD** põe o Snort (55 MiB) num regime
  intermediário (entre L3/CCD e os 64 MiB totais) que o i5 (12 MiB unificado) não
  exibe. O regime paralelo (T=32) mostra cliff de −75% (16.149 → 4.012 MB/s).

---

## 6. Balanceamento por thread (Snort, Enron ×8, cv%)

| Variante | i5 cv% | Ryzen cv% |
|---|---:|---:|
| encadeada (`chunked`/`chunked_v2`) | **39,9–52,9%** | **0,10%** |
| flat (`chunked_flat`) | 0,43% | 0,38% |
| campeão (`*_flat`) | 4,28% (`v3_flat`) | 0,23% (`dynamic_flat`) |

No i5, a estática encadeada gera variância enorme (E-cores viram *stragglers*;
a barreira espera o mais lento) — origem das variantes topology-aware. No Ryzen,
com núcleos idênticos, a estática já equilibra (cv 0,10%) e o `dynamic_flat`
simples vence sem afinidade/pesos.

---

## 7. Construção paralela do autômato (build speedup vs. sequencial)

| Dicionário | i5 (pico) | Ryzen (pico) |
|---|---|---|
| ET-32 | 1,55× (T=8) | 1,57× (T=8) |
| Snort | 1,19× (T=4) | 1,10× (T=4) |
| Snort-1k / 100 | < 1× | < 1× |

Praticamente idêntico nas duas máquinas: BFS nível-síncrona é Amdahl-limitada,
satura em ~1,5× só no dicionário grande, e regride nos pequenos. Build é < 1% do
tempo total → irrelevante para o speedup de pipeline.

---

## 8. Footprint × cache — saturação (Ryzen, Enron 1×, `dynamic_flat`, T=32)

| Dicionário | Autômato | Regime de cache | MB/s (T=32) | MB/s (T=1, seq) |
|---|---:|---|---:|---:|
| Snort-100 | 1,9 MiB | cabe em L2 | 16.223 | 644 |
| Snort-1k | 11,9 MiB | L3 de 1 CCD | 10.058 | 451 |
| Snort | 55,2 MiB | entre L3/CCD (32) e total (64) | 7.576 | 329 |
| ET-32 | 506,8 MiB | 15× o L3 total — DRAM | 4.071 | 214 |

Queda de −75% do menor ao maior autômato (T=32); piso absoluto alto (4.071 MB/s).

---

## 9. Eixo de paralelismo — texto × padrões (Ryzen, Snort, T=32)

| Variante | MB/s | vs. campeão |
|---|---:|---:|
| `pthread_dynamic_flat` (texto) | 7.468 | 1,00× |
| `pthread_2d_sharded_chunked` (2D) | 5.502 | 0,74× |
| `pattern_sharded_prefix` (padrões) | 585 | 0,08× |

Particionar o **texto** vence particionar o **conjunto de padrões** também em
núcleos homogêneos. O sharding puro colapsa (cada thread relê o corpus inteiro →
tráfego de memória ×K, sobre um gargalo que já é de banda).

---

## 10. Confronto com a tese

Tese escrita só com o i5. Verdicto = efeito dos dados da workstation.
Referências: `acceleration-of-.../partes/{results,conclusion}.tex`.

| Afirmação da tese | i5 | Ryzen | Veredito |
|---|---|---|---|
| "Larger uniform machine delays the bottleneck, not eliminates it" (`conclusion.tex:74-81`) | previsão | cliff persiste −75%; efic. ET-32 cai 82%@16→62%@32 | **CONFIRMA** |
| Split de L3 limita a thread única | — | 1 thread vê só 32 MiB; vazão cai 451→329 ao cruzar 11,9→55 MiB | **CONFIRMA** |
| Texto > padrões; flat > encadeada; dinâmica ≥ estática | hierarquia | mesma hierarquia | **CONFIRMA** |
| Build paralelo é teto algorítmico (~1,5×), não de hardware | 1,55×@8 | 1,57×@8 (16 vs 2 núcleos) | **CONFIRMA** |
| "more threads counterproductive" (regressão pós-6T) (`conclusion.tex:18-21`) | regride após 6T | monotônico até 22,72× | **CONTESTA** — artefato de hardware bandwidth-starved (1 canal DRAM + 2 P-cores) |
| Fração serial inerente ~15% (`results.tex:146`) | 37,7% efic. → ~15% | 89,1% efic.@16 → ~0,8% | **CONTESTA** — os "15%" eram a assimetria P/E, não código serial |
| flat "+13%, cresce com a pressão" (`results.tex`) | +1%→+4,7%→+13% | ~+4% constante (−0,3/+4,0/+4,1/+4,7%) | **CONTESTA** — pico +13% era a DRAM fraca do i5 |
| "Load balancing on heterogeneous cores" / topology-aware (`results.tex:208-267`) | cv 39,9–52,9% encadeado | cv 0,10% | **REVELA** — variância é específica de hardware híbrido; some em núcleos idênticos |
| "No single strategy is best" (`results.tex:461`) | campeão alterna | `dynamic_flat` vence nos dois regimes | **REVELA** — hardware uniforme colapsa o espaço de projeto num vencedor |

---

## 11. Não resolvido pela workstation

- **Fase C (cross-corpus) não rodada** → a afirmação "conteúdo do texto não
  importa / cv 66%" (`results.tex:155-165`) não foi re-testada. O cv ínfimo aqui
  (≤ 0,56%) sugere que os 66% do i5 eram ruído térmico/scheduler — não é prova.
- **Sem contadores de hardware (PMU)** → banda de memória continua *inferida* das
  curvas, não medida (Future Work, `conclusion.tex:86-91`).
- **DDR5: velocidade exata não capturada** (`dmidecode` sem root) → preencher a
  tabela de hardware do TCC manualmente.
