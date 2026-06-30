# Plano de testes — Aho–Corasick paralelo (workstation de núcleos homogêneos)

> **Nota 2026-06-30:** este documento registra a primeira corrida reduzida da
> workstation. O protocolo canônico atual é o default A–G de
> `scripts/run_sweep.sh`, disparado por `RUN_DIR=runs/workstation
> ./scripts/run_all.sh`; ele inclui `pthread_dynamic_flat` nas curvas principais
> e deve substituir estes números quando a nova corrida for versionada.

## 1. Objetivo

Medir, numa CPU multicore de **núcleos homogêneos**, o ganho de desempenho em função de:
- **número de threads** (escalabilidade)
- **tamanho do dicionário** (relação
autômato × cache)
- **estratégia de distribuição de trabalho** (estática vs.
dinâmica)
- **custo de construção** do autômato. 

Toda variante é comparada contra a implementação **sequencial de referência**.

## 2. Ambiente de execução

| Item | Valor |
|---|---|
| Processador | AMD Ryzen 9 9950X (Zen 5), 16 núcleos físicos idênticos / 32 threads (SMT 2×) |
| Cache L3 | 64 MiB **não unificado** — 2 complexos de núcleos (CCD) de 32 MiB cada |
| Memória | DDR5-5600 |
| Sistema | Linux Ubuntu |
| Medição | janela cronometrada com relógio monotônico; corpus carregado em RAM antes da medição |

## 3. Métricas coletadas (por execução)

- **Vazão** (MB/s e Gbps);
- **Tempo** mínimo, médio, mediano e máximo (ms);
- **Coeficiente de variação** (cv %), como medida de estabilidade;
- **Número de casamentos** — verificação de correção contra a sequencial;
- **Speedup** = tempo sequencial ÷ tempo da variante;
- Quando aplicável: **decomposição por thread** (tempo e trabalho de cada
  worker) e **tempo de construção** do autômato.

Cada medição faz iterações de **aquecimento** (pagam page faults e aquecem
caches, descartadas) seguidas de iterações **cronometradas** (reportadas).

## 4. Conjuntos de dados

**Dicionários (padrões → autômato):**

| Dicionário | Origem | Autômato (aprox.) | Regime de cache |
|---|---|---|---|
| 100 regras | subconjunto Snort | ~1 MiB | cabe em cache de núcleo |
| 1.000 regras | subconjunto Snort | ~10–12 MiB | cabe no L3 de um CCD |
| Snort (completo) | assinaturas Snort | ~56 MiB | acima do L3 por CCD (32 MiB), abaixo do total (64 MiB) |
| ET-32 | Emerging Threats | ~515 MiB | muito acima do L3 — dominado por DRAM |

**Corpora (texto varrido):**

| Corpus | Origem | Tamanho |
|---|---|---|
| Enron | Enron Email Dataset | 1,4 GiB |
| Enron ×8 | Enron replicado 8× | 10,6 GiB |

O Enron ×8 representa o regime alvo (varredura em escala de gigabytes, limitada
por banda de memória). O Enron (1×) é usado quando o fator em estudo é o
**dicionário**, não o tamanho do texto.

## 5. Implementações avaliadas

Todas compartilham o mesmo autômato somente-leitura e o mesmo contrato de saída;
diferem em **dois eixos independentes**:

- **Distribuição do trabalho:** *estática* (cada thread recebe uma fatia fixa e
  igual do texto) vs. *dinâmica* (texto cortado em muitas tarefas pequenas; cada
  thread puxa a próxima de uma fila compartilhada — *bag of tasks*).
- **Emissão de casamentos:** *encadeada* (percorre listas de saída por estado)
  vs. *contígua/flat* (lê, por estado, um intervalo linear num vetor
  pré-computado — mais amigável ao prefetcher de hardware).

Para corrigir o estado do autômato na fronteira entre fatias, cada thread relê
os últimos **(L−1)** bytes da fatia anterior como aquecimento (L = maior padrão)
e só reporta casamentos que terminam **dentro da própria fatia**. Sem travas:
cada thread acumula casamentos em lista privada, combinadas ao final.

| Implementação | Distribuição | Emissão | Papel no estudo |
|---|---|---|---|
| `sequential` | — (1 thread) | encadeada | referência: oráculo de correção e denominador do speedup |
| `sequential_flat` | — (1 thread) | flat | isola o efeito do layout de emissão (sem paralelismo) |
| `pthread_chunked_v2` | estática | encadeada | baseline paralelo estático |
| `pthread_chunked_flat` | estática | flat | campeão estático (emissão contígua) |
| `pthread_dynamic` | dinâmica | encadeada | balanceamento dinâmico (bag of tasks) |
| `pthread_dynamic_flat` | dinâmica | flat | candidato a melhor variante neste hardware |
| `pattern_sharded_prefix` | por padrão (prefixo) — texto inteiro por thread | encadeada | paralelismo no **dicionário**: K sub-autômatos disjuntos, cada thread varre todo o texto |
| `pthread_2d_sharded_chunked` | 2D: padrão (K) × texto (N) | encadeada | combina sharding de dicionário com particionamento de texto (T = K×N) |

> Variantes *topology-aware* (afinidade + pesos por frequência) ficam **fora de
> escopo** aqui: foram projetadas para balancear P-cores vs. E-cores; em núcleos
> homogêneos colapsam nas variantes estáticas equivalentes.
>
> As duas últimas (`pattern_sharded_prefix`, `pthread_2d_sharded_chunked`)
> exploram um **eixo de paralelismo distinto** — particionar o conjunto de
> padrões em vez do texto — e servem de contraprova ao Teste 5.

## 6. Bateria de testes

### Teste 1 — Escalabilidade: vazão × número de threads
**Pergunta:** como a vazão escala com o número de threads, e quanto cada eixo
(emissão flat; distribuição dinâmica) contribui?

| Parâmetro | Valor |
|---|---|
| Corpus | Enron ×8 (10,6 GiB) |
| Dicionários | Snort (~56 MiB) e ET-32 (~515 MiB) — dois regimes de cache |
| Variantes | `chunked_v2`, `chunked_flat`, `dynamic`, `dynamic_flat` + referências `sequential`, `sequential_flat` |
| Threads | 1, 2, 4, 8, 16, 24, 32 |
| Réplicas | 2 aquecimento + 8 medidas |
| **Execuções** | **60** |

Comparações que o teste isola: **emissão** (`chunked_v2` vs. `chunked_flat`, mesma
partição estática) e **distribuição** (`chunked_v2` vs. `dynamic`, encadeada;
`chunked_flat` vs. `dynamic_flat`, flat).

### Teste 2 — Sensibilidade ao tamanho do dicionário (footprint × cache)
**Pergunta:** em que tamanho de autômato a cache satura e a vazão cai?

| Parâmetro | Valor |
|---|---|
| Corpus | Enron (1,4 GiB) |
| Dicionários | 100, 1.000, Snort, ET-32 (~1 MiB → ~515 MiB) |
| Variantes | `chunked_flat`, `dynamic_flat` + referências `sequential`, `sequential_flat` |
| Threads | 1 e 32 |
| Réplicas | 2 aquecimento + 5 medidas |
| **Execuções** | **24** |

O ponto de saturação é função do **dicionário**, não do texto — daí o corpus
menor. Como o L3 é de 32 MiB **por CCD**, o ponto de virada se desloca em relação
a máquinas de cache pequena (resultado novo para este hardware).

### Teste 3 — Balanceamento de carga por thread
**Pergunta:** quanto cada thread fica ociosa esperando as demais na barreira
final, sob distribuição estática vs. dinâmica?

| Parâmetro | Valor |
|---|---|
| Corpus / dicionário | Enron ×8 / Snort |
| Variantes | `chunked_v2`, `chunked_flat`, `dynamic`, `dynamic_flat` |
| Threads | 32 (decomposição por thread) |
| Réplicas | 1 aquecimento + 3 medidas |
| **Execuções** | **4** |

Mede a qualidade do escalonamento: ociosidade na barreira (estáticas) vs.
dispersão de tarefas consumidas por thread (dinâmicas).

### Teste 4 — Custo de construção do autômato (sequencial vs. paralela)
**Pergunta:** a construção do autômato se beneficia de paralelismo, e a partir de
que tamanho de dicionário?

| Parâmetro | Valor |
|---|---|
| Dicionários | 100, 1.000, Snort, ET-32 |
| Construção | sequencial vs. paralela (busca em largura nível-síncrona) em 2, 4, 8, 16, 32 threads |
| Medição | apenas tempo de construção (a varredura é irrelevante aqui) |
| **Execuções** | **24** |

Avalia a latência de **recarga do conjunto de regras** — métrica operacional
independente da vazão de varredura.

### Teste 5 — Eixo de paralelismo: particionar o texto vs. particionar os padrões
**Pergunta:** dividir o **conjunto de padrões** entre threads (cada uma com seu
sub-autômato) é competitivo com dividir o **texto**?

| Parâmetro | Valor |
|---|---|
| Corpus / dicionário | Enron ×8 / Snort |
| Variantes | `pattern_sharded_prefix` (sharding puro), `pthread_2d_sharded_chunked` (2D) |
| Referência | melhor variante de texto (`dynamic_flat`, Teste 1) |
| Threads | 1 e 32 |
| **Execuções** | **4** |

Sharding de dicionário tem um custo estrutural: cada thread varre o **texto
inteiro** com seu sub-autômato, multiplicando o tráfego de RAM por K. O teste
mede se a melhor localidade de cache por shard compensa esse custo — hipótese
**falsificada** neste hardware (ver resultados). É a contraprova que justifica
a escolha de particionar o texto, não o dicionário.

## 7. Resumo quantitativo

| Teste | Foco | Execuções |
|---|---|---:|
| 1 — Escalabilidade | curva de speedup; eixos emissão e escalonamento | 60 |
| 2 — Dicionário × cache | ponto de saturação da cache | 24 |
| 3 — Balanceamento | ociosidade estática vs. dinâmica | 4 |
| 4 — Construção | build sequencial vs. paralelo | 24 |
| 5 — Eixo de paralelismo | texto vs. padrões (sharding) | 4 |
| **Total** | | **116** |

## 8. Correção e reprodutibilidade

Antes de qualquer medição, todas as implementações são validadas contra a
sequencial em múltiplas contagens de threads ({1, 2, 3, 4, 7, 8}): o multiconjunto
de casamentos produzido deve ser **idêntico** ao da referência, e a ausência de
condições de corrida é confirmada sob analisador dinâmico de threads (o autômato
é acessado só em leitura; cada thread escreve apenas em sua própria saída).

A corrida é automatizada por script único; os resultados ficam em logs brutos +
base SQLite com todas as métricas. Durante a análise, a correção é **re-checada**:
divergência no número de casamentos entre variantes bloqueia o uso dos números.

## 9. Resultados

Fonte: `parallel-aho-corasick/runs/workstation/sweep.db` (corrida de
2026-06-25, Ryzen 9 9950X, 32 threads). Comparativos com i5 vêm de
`runs/i5/sweep.db` (i5-1235U, 12 threads). Correção confirmada:
contagem de casamentos idêntica entre todas as variantes (Snort = 142.831.896;
ET-32 = 466.943.872 sobre Enron ×8).

**Legenda das variantes** (colunas das tabelas abaixo): distribuição do
trabalho × emissão de casamentos.

| Variante | Distribuição | Emissão |
|---|---|---|
| `chunked_v2` | estática | encadeada |
| `chunked_flat` | estática | contígua (flat) |
| `dynamic` | dinâmica (bag of tasks) | encadeada |
| `dynamic_flat` | dinâmica (bag of tasks) | contígua (flat) |

### 9.0 Ambientes

| Item | i5-1235U | Ryzen 9 9950X |
|---|---|---|
| Núcleos físicos / threads | 2 P + 8 E / 12 | 16 / 32 (SMT) |
| Topologia de núcleos | heterogênea (P/E) | homogênea |
| L3 | 12 MiB unificado | 64 MiB (2 CCD × 32 MiB) |
| Threads máx. no sweep | 12 | 32 |

### 9.1 Vazão sequencial de referência (Enron ×8, 1 thread, MB/s)

| Dicionário | i5 `sequential` | i5 `sequential_flat` | Ryzen `sequential` | Ryzen `sequential_flat` | Razão Ryzen/i5 (seq) |
|---|---:|---:|---:|---:|---:|
| Snort (~55 MiB) | 240,28 | 251,82 | 328,71 | 344,39 | 1,37× |
| ET-32 (~507 MiB) | 125,41 | 142,06 | 211,53 | 224,86 | 1,69× |

### 9.2 Teste 1 — Escalabilidade, Snort / Enron ×8 (vazão MB/s)

| Threads | `chunked_v2` | `chunked_flat` | `dynamic` | `dynamic_flat` |
|---:|---:|---:|---:|---:|
| 1 | 329,85 | 343,95 | 328,23 | 343,82 |
| 2 | 644,66 | 669,79 | 645,16 | 670,04 |
| 4 | 1263,91 | 1314,95 | 1259,25 | 1310,13 |
| 8 | 2440,35 | 2545,08 | 2427,00 | 2539,77 |
| 16 | 4499,03 | 4686,68 | 4500,64 | 4687,13 |
| 24 | 5625,83 | 5941,08 | 5644,77 | 5934,94 |
| 32 | 7017,10 | 7345,21 | 7134,12 | **7467,87** |

### 9.3 Teste 1 — Escalabilidade, ET-32 / Enron ×8 (vazão MB/s)

| Threads | `chunked_v2` | `chunked_flat` | `dynamic` | `dynamic_flat` |
|---:|---:|---:|---:|---:|
| 1 | 210,26 | 223,90 | 210,23 | 225,31 |
| 2 | 406,47 | 434,03 | 405,71 | 433,17 |
| 4 | 762,84 | 812,77 | 768,21 | 815,35 |
| 8 | 1451,87 | 1542,88 | 1440,62 | 1537,04 |
| 16 | 2618,13 | 2757,98 | 2618,60 | 2760,90 |
| 24 | 3252,55 | 3453,46 | 3177,58 | 3426,40 |
| 32 | 3921,04 | 4176,83 | 3925,22 | **4186,47** |

### 9.4 Teste 1 — Speedup vs. sequencial a 32 threads (Ryzen, Enron ×8)

Speedup = vazão da variante ÷ vazão da `sequential` (mesmo dicionário e corpus).

| Variante | Snort | ET-32 |
|---|---:|---:|
| `pthread_chunked_v2` | 21,35× | 18,54× |
| `pthread_chunked_flat` | 22,35× | 19,75× |
| `pthread_dynamic` | 21,70× | 18,56× |
| `pthread_dynamic_flat` | **22,72×** | **19,79×** |

### 9.4b Teste 1 — Speedup vs. sequencial por variante × threads (Ryzen)

Snort / Enron ×8:

| Threads | `chunked_v2` | `chunked_flat` | `dynamic` | `dynamic_flat` |
|---:|---:|---:|---:|---:|
| 1 | 1,00× | 1,05× | 1,00× | 1,05× |
| 2 | 1,96× | 2,04× | 1,96× | 2,04× |
| 4 | 3,85× | 4,00× | 3,83× | 3,99× |
| 8 | 7,42× | 7,74× | 7,38× | 7,73× |
| 16 | 13,69× | 14,26× | 13,69× | 14,26× |
| 24 | 17,12× | 18,07× | 17,17× | 18,06× |
| 32 | 21,35× | 22,35× | 21,70× | **22,72×** |

ET-32 / Enron ×8:

| Threads | `chunked_v2` | `chunked_flat` | `dynamic` | `dynamic_flat` |
|---:|---:|---:|---:|---:|
| 1 | 0,99× | 1,06× | 0,99× | 1,07× |
| 2 | 1,92× | 2,05× | 1,92× | 2,05× |
| 4 | 3,61× | 3,84× | 3,63× | 3,86× |
| 8 | 6,86× | 7,29× | 6,81× | 7,27× |
| 16 | 12,38× | 13,04× | 12,38× | 13,05× |
| 24 | 15,38× | 16,33× | 15,02× | 16,20× |
| 32 | 18,54× | 19,75× | 18,56× | **19,79×** |

> Nota: `chunked_v2` e `dynamic` têm a mesma emissão (encadeada) e, em núcleos
> homogêneos com corpus uniforme, o balanceamento dinâmico quase não rende —
> por isso as duas colunas ficam tão próximas que coincidem ao arredondar
> (a 2 casas). Os valores brutos diferem: ver vazão em MB/s nas tabelas 9.2/9.3
> (ex.: Snort @32 = 7017,10 vs. 7134,12 MB/s).

### 9.5 Comparação i5 × Ryzen — `pthread_chunked_flat`, Enron ×8

Snort (~55 MiB):

| Threads | i5 MB/s | i5 speedup | Ryzen MB/s | Ryzen speedup |
|---:|---:|---:|---:|---:|
| 1 | 251,62 | 1,05× | 343,95 | 1,05× |
| 4 | 570,44 | 2,37× | 1314,95 | 4,00× |
| 8 | 841,07 | 3,50× | 2545,08 | 7,74× |
| 12 (i5 máx.) | 1073,32 | 4,47× | — | — |
| 32 (Ryzen máx.) | — | — | 7345,21 | 22,35× |

ET-32 (~507 MiB):

| Threads | i5 MB/s | i5 speedup | Ryzen MB/s | Ryzen speedup |
|---:|---:|---:|---:|---:|
| 1 | 142,49 | 1,14× | 223,90 | 1,06× |
| 4 | 248,88 | 1,99× | 812,77 | 3,84× |
| 8 | 331,29 | 2,64× | 1542,88 | 7,29× |
| 12 (i5 máx.) | 350,01 | 2,79× | — | — |
| 32 (Ryzen máx.) | — | — | 4176,83 | 19,75× |

### 9.6 Vazão de pico por máquina (melhor variante disponível, Enron ×8)

`@thr` = nº de threads no pico (máx. da máquina: i5 = 12, Ryzen = 32). Cada
célula usa a **melhor variante medida naquele hardware** — e os conjuntos de
variantes diferem **de propósito**, casados com a topologia:

- **i5 (P/E heterogêneo):** inclui as variantes *topology-aware* `v3`/`v3_flat`
  (afinidade + chunks ponderados por frequência); **não** inclui `dynamic_flat`.
  O campeão é `v3_flat` (Snort) porque ele ataca diretamente o desbalanceamento
  P/E dando fatias maiores aos P-cores.
- **Ryzen (homogêneo):** inclui `dynamic_flat`; **não** roda `v3`/`v3_flat`, que
  colapsariam em `v2` (não há P/E para balancear). O campeão é `dynamic_flat`.

Por isso o "melhor searcher" não é o mesmo nos dois — `dynamic_flat` nem foi
testado no i5 (vacilo meu, se der rodo na segunda), e `v3_flat` não faz sentido no Ryzen.

| Dicionário | i5 pico (MB/s @thr, variante) | Ryzen pico (MB/s @thr, variante) | Razão Ryzen/i5 |
|---|---|---|---:|
| Snort | 1087,13 @12 (`chunked_v3_flat`) | 7467,87 @32 (`dynamic_flat`) | 6,87× |
| ET-32 | 350,01 @12 (`chunked_flat`) | 4186,47 @32 (`dynamic_flat`) | 11,96× |

### 9.7 Teste 2 — Dicionário × cache (Enron 1×, `pthread_chunked_flat`)

| Dicionário | Autômato (MiB) | Ryzen 1 thr | Ryzen 32 thr | Ryzen 1→32 | i5 1 thr | i5 12 thr | i5 1→12 |
|---|---:|---:|---:|---:|---:|---:|---:|
| Snort-100 | 1,93 | 647,3 | 16149,5 | 24,95× | 488,6 | 1430,4 | 2,93× |
| Snort-1k | 11,92 | 471,1 | 9952,4 | 21,13× | 350,4 | 1108,7 | 3,16× |
| Snort | 55,23 | 341,9 | 7438,9 | 21,76× | 251,7 | 1228,3 | 4,88× |
| ET-32 | 506,78 | 227,2 | 4012,3 | 17,66× | 142,0 | 358,8 | 2,53× |

### 9.8 Teste 3 — Vazão e estabilidade das variantes (Snort / Enron ×8, 32 threads, Ryzen)

| Variante | MB/s | cv % | min (ms) | mean (ms) | max (ms) |
|---|---:|---:|---:|---:|---:|
| `pthread_chunked_v2` | 7016,62 | 0,10 | 1544,04 | 1545,30 | 1547,03 |
| `pthread_chunked_flat` | 7357,63 | 0,38 | 1468,95 | 1473,68 | 1479,85 |
| `pthread_dynamic` | 7177,75 | 0,10 | 1509,09 | 1510,61 | 1512,14 |
| `pthread_dynamic_flat` | 7510,14 | 0,23 | 1440,43 | 1443,75 | 1447,12 |

Em 9.8 min/mean/max e cv% medem a **corrida inteira** (3 iterações
cronometradas). Em 9.8b a granularidade desce ao **worker individual**.

### 9.8b Teste 3 — Balanceamento por thread: decomposição por worker (Snort / Enron ×8, 32 workers, Ryzen)

Tempo e trabalho de cada um dos 32 workers dentro da mesma corrida. `Spread` =
(máx − mín) ÷ mín dos tempos por worker.

| Variante | Bytes/worker (mín–máx) | Tempo/worker mín (ms) | máx (ms) | Spread |
|---|---|---:|---:|---:|
| `pthread_chunked_v2` | 355.295.934 – 355.296.403 | 1282,5 | 1384,5 | **8,0%** |
| `pthread_chunked_flat` | 355.295.934 – 355.296.403 | 1247,5 | 1307,7 | 4,8% |
| `pthread_dynamic` | 355.295.936 (igual) | 1280,1 | 1357,1 | 6,0% |
| `pthread_dynamic_flat` | 355.295.936 (igual) | 1221,9 | 1284,7 | 5,1% |

> Semântica da coluna de bytes: nas **estáticas**, bytes/worker incluem o
> warm-up de overlap `(L−1) = 469` B; o worker 0 não tem trecho anterior para
> reler, daí os 469 B a menos.
> Nas **dinâmicas**, bytes/worker = `tarefas consumidas × tamanho da tarefa`
> (≈ 88,8 MB cada), então a métrica só varia em passos de uma tarefa inteira:
> os 128 tarefas (4 por thread) couberam exatamente 4-a-4 entre os 32 workers,
> resultando em valores idênticos.

Acho que faria sentido testar com tarefas menores, 128 para 32 threads não revelou muita coisa.

**Exemplo da medição bruta por worker**

`pthread_chunked_flat` (estática — cada worker varre uma fatia contígua fixa):

| Worker | Tempo (ms) | Bytes | Matches | MB/s |
|---|---:|---:|---:|---:|
| 1 (t00) | 1284,4 | 355.295.934 | 4.892.714 | 263,8 |
| 2 (t01) | 1248,9 | 355.296.403 | 4.338.749 | 271,3 |
| 3 (t02) | 1288,4 | 355.296.403 | 3.945.684 | 263,0 |
| 32 (t31) | 1248,1 | 355.296.403 | 4.676.840 | 271,5 |

`pthread_dynamic_flat` (dinâmica — cada worker junta 4 tarefas espalhadas):

| Worker | Tempo (ms) | Bytes | Matches | MB/s |
|---|---:|---:|---:|---:|
| 1 (t00) | 1271,5 | 355.295.936 | 3.894.627 | 266,5 |
| 2 (t01) | 1238,2 | 355.295.936 | 4.936.083 | 273,7 |
| 3 (t02) | 1280,7 | 355.295.936 | 5.563.129 | 264,6 |
| 32 (t31) | 1234,5 | 355.295.936 | 4.827.761 | 274,5 |

### 9.9 Teste 4 — Construção do autômato, ET-32 (tempo ms, speedup vs. sequencial)

| Build threads | i5 ms | i5 speedup | Ryzen ms | Ryzen speedup |
|---:|---:|---:|---:|---:|
| 1 | 485,18 | 1,00× | 310,21 | 1,00× |
| 2 | 412,38 | 1,18× | 269,98 | 1,15× |
| 4 | 338,59 | 1,43× | 227,28 | 1,37× |
| 8 | 312,66 | **1,55×** | 198,15 | **1,57×** |
| 12 (i5 máx.) | 345,08 | 1,41× | — | — |
| 16 | — | — | 203,16 | 1,53× |
| 32 | — | — | 224,41 | 1,38× |

Dicionários menores não se beneficiam da construção paralela (speedup ≤ ~1,1
e degradando acima de 8 threads): melhor caso Snort = 1,10× @4 (Ryzen) /
1,19× @4 (i5); Snort-1k e Snort-100 ficam sempre < 1,0×.

### 9.10 Teste 5 — Particionar o texto vs. os padrões (Snort / Enron ×8, Ryzen)

Speedup 1→32 = vazão a 32 threads ÷ vazão a 1 thread (mesma variante). Última
linha: melhor variante de **texto** (Teste 1) como referência.

| Searcher | Eixo | 1 thr (MB/s) | 32 thr (MB/s) | Speedup 1→32 |
|---|---|---:|---:|---:|
| `pattern_sharded_prefix` | padrões | 342,69 | 584,69 | 1,71× |
| `pthread_2d_sharded_chunked` | padrões × texto | 343,46 | 5501,73 | 16,02× |
| `pthread_dynamic_flat` (ref.) | texto | 343,82 | 7467,87 | 21,72× |
