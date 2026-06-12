# Plano de testes - Aho–Corasick paralelo no Workstation

## 1. Objetivo

Quantificar, em CPU multicore de núcleos homogêneos, o ganho de
desempenho de diferentes estratégias de paralelização e de layout de
memória, em função de:

- **número de threads** (escalabilidade);
- **tamanho do dicionário** (relação entre o autômato e a hierarquia de
  cache);
- **estratégia de distribuição de trabalho** (estática vs. dinâmica);
- **custo de construção** do autômato.

Toda variante é comparada contra a implementação sequencial de
referência.

---

## 2. Ambiente de execução

| Item | Valor |
|---|---|
| Processador | AMD Ryzen 9 9950X (Zen 5), 16 núcleos físicos idênticos / 32 threads (SMT 2×) |
| Cache L3 | 64 MiB **não unificado** — 2 complexos de núcleos (CCD) de 32 MiB cada |
| Memória | 'X' (verificar e preencher) |
| Compilação | C11, binário único, `-O3 -march=native` |
| Governador de frequência | `performance` |
| Medição | janela cronometrada com relógio monotônico; corpus carregado em RAM antes da medição |

**Protocolo por execução:** cada medição faz *iterações de aquecimento*
(que pagam page faults e aquecem caches, descartadas) seguidas de
*iterações cronometradas* (reportadas). As métricas registradas por
execução são:

- **vazão** (MB/s e Gbps);
- **tempo** mínimo, médio e máximo (ms) e mediana;
- **coeficiente de variação** (cv %), como medida de estabilidade;
- **número de casamentos** (verificação de correção);
- **speedup** = tempo médio sequencial ÷ tempo médio da variante;
- opcionalmente, **decomposição por thread** (tempo e trabalho de cada
  worker) e **tempo de construção** do autômato.

---

## 3. Conjuntos de dados

### Dicionários (padrões → autômato)

| Dicionário | Origem | Tamanho aproximado do autômato | Regime de cache |
|---|---|---|---|
| 100 regras | subconjunto de assinaturas Snort | ~1 MiB | cabe em cache de núcleo |
| 1.000 regras | subconjunto de assinaturas Snort | ~10–12 MiB | cabe no L3 de um CCD |
| Snort (completo) | assinaturas Snort | ~56 MiB | acima do L3 por CCD (32 MiB), abaixo do total |
| ET-32 | assinaturas Emerging Threats | ~515 MiB | muito acima do L3 (dominado por DRAM) |

### Corpora (texto varrido)

| Corpus | Origem | Tamanho |
|---|---|---|
| Enron | Enron Email Dataset | 1,4 GiB |
| Enron ×8 | Enron Email Dataset replicado 8× | 10,6 GiB |

O corpus Enron ×8 representa o regime alvo (varredura em escala de
gigabytes, limitada por banda de memória). O corpus Enron (1×) é usado
quando o fator em estudo é o dicionário e não o tamanho do texto.

---

## 4. Implementações avaliadas

Todas compartilham o mesmo autômato somente leitura e o mesmo contrato de
saída; diferem apenas na **estratégia de paralelização** e no **layout de
emissão de casamentos**.

Dois eixos independentes aparecem nas variantes:

- **Distribuição do trabalho:** *estática* (cada thread recebe uma fatia
  fixa e igual do texto) vs. *dinâmica* (o texto é cortado em muitas
  tarefas pequenas e as threads puxam a próxima tarefa de uma fila
  compartilhada conforme terminam).
- **Emissão de casamentos:** *lista encadeada* (percorre ponteiros de
  saída por estado) vs. *tabela contígua* (lê, por estado, um intervalo
  linear de identificadores de padrão num vetor pré-computado).

### 4.1 `sequential` — referência

Varredura single-thread: uma transição por byte; em cada estado, reporta
os padrões percorrendo as listas de saída encadeadas. É o oráculo de
correção e o denominador do speedup.

### 4.2 `sequential_flat` — referência com emissão contígua

Mesma varredura single-thread, mas a emissão lê a **tabela contígua** de
identificadores em vez de percorrer listas encadeadas. Isola o efeito do
layout de memória da emissão, sem nenhuma interferência de paralelismo.

### 4.3 `pthread_chunked_v2` — particionamento estático do texto

Paralelismo de dados sobre o texto. O texto é dividido em N fatias
contíguas de tamanho igual, uma por thread. Para corrigir o estado do
autômato na fronteira, cada thread relê os últimos **(L−1)** bytes da
fatia anterior como aquecimento (L = comprimento do maior padrão) e só
reporta casamentos que terminam **dentro da própria fatia**. O autômato é
somente leitura (sem travas); cada thread acumula casamentos em uma lista
privada, combinadas ao final. Emissão por lista encadeada.

*Estratégia em uma frase:* divide o texto em pedaços fixos e iguais — um
por thread.

### 4.4 `pthread_chunked_flat` — particionamento estático + emissão contígua

Particionamento e tratamento de fronteira idênticos ao
`pthread_chunked_v2`, porém a emissão usa a **tabela contígua**. Combina
o paralelismo sobre o texto com o layout de emissão mais barato.

*Estratégia em uma frase:* pedaços fixos e iguais, com emissão linear de
casamentos.

### 4.5 `pthread_dynamic` — distribuição dinâmica (fila de tarefas)

Mesmo modelo de correção (fatias contíguas com aquecimento (L−1)), mas o
texto é cortado em **muito mais tarefas que threads** (4× o número de
threads). Cada thread obtém a próxima tarefa incrementando um **contador
atômico compartilhado**; uma thread que termina cedo simplesmente pega
mais tarefas. O contador atômico é tocado apenas na fronteira de cada
tarefa, **nunca por byte**. Assim, desequilíbrios em tempo de execução
(contenção de SMT, variação de frequência, ruído do escalonador) são
absorvidos sem suposição de carga uniforme. Emissão por lista encadeada.

*Estratégia em uma frase:* muitas tarefas pequenas numa fila; threads
ociosas puxam mais trabalho.

### 4.6 `pthread_dynamic_flat` — fila de tarefas + emissão contígua

Combina a distribuição dinâmica do `pthread_dynamic` com a emissão pela
**tabela contígua**. Une balanceamento dinâmico de carga e emissão
barata.

*Estratégia em uma frase:* fila de tarefas dinâmica com emissão linear de
casamentos.

---

## 5. Bateria de testes

Cada teste responde a uma pergunta experimental específica. As contagens
de execuções assumem 32 threads de hardware.

### 5.1 Escalabilidade: vazão vs. número de threads

**Pergunta:** como a vazão escala com o número de threads, e quanto cada
eixo (emissão contígua; distribuição dinâmica) contribui?

| Parâmetro | Valor |
|---|---|
| Corpus | Enron ×8 (10,6 GiB) |
| Dicionários | Snort (~56 MiB) e ET-32 (~515 MiB) — dois regimes de cache |
| Referências (1 thread) | `sequential`, `sequential_flat` |
| Variantes | `pthread_chunked_v2`, `pthread_chunked_flat`, `pthread_dynamic`, `pthread_dynamic_flat` |
| Threads | 1, 2, 4, 8, 16, 24, 32 |
| Iterações | 2 aquecimento + 8 cronometradas |
| Execuções | 2 dicionários × (2 referências + 7 threads × 4 variantes) = **60** |

**Comparações que o teste isola:**
- emissão *lista encadeada* vs. *contígua*, a partição fixa:
  `pthread_chunked_v2` vs. `pthread_chunked_flat`;
- distribuição *estática* vs. *dinâmica*, mesma emissão:
  `pthread_chunked_v2` vs. `pthread_dynamic` (encadeada) e
  `pthread_chunked_flat` vs. `pthread_dynamic_flat` (contígua).

Os pontos de thread atravessam as fronteiras arquiteturais do
processador: até 8 threads dentro de um único CCD; 8→16 cruzando a
fronteira entre os dois CCDs (L3 não compartilhado entre eles); 16→32
medindo o ganho marginal do SMT.

### 5.2 Sensibilidade ao tamanho do dicionário (footprint vs. cache)

**Pergunta:** em que ponto o crescimento do autômato satura a cache e
derruba a vazão?

| Parâmetro | Valor |
|---|---|
| Corpus | Enron (1,4 GiB) |
| Dicionários | 100 regras, 1.000 regras, Snort completo, ET-32 (~1 MiB → ~515 MiB) |
| Referências (1 thread) | `sequential`, `sequential_flat` |
| Variantes | `pthread_chunked_flat`, `pthread_dynamic_flat` |
| Threads | 1 e 32 |
| Iterações | 2 aquecimento + 5 cronometradas |
| Execuções | 4 dicionários × (2 referências + 2 variantes × 2 threads) = **24** |

Usa o corpus de 1,4 GiB de propósito: o ponto de saturação é função do
**dicionário** (tamanho do autômato), não do conteúdo do texto. Como o L3
deste processador é de 32 MiB por CCD, os dicionários cruzam o limite de
cache em pontos diferentes do esperado em máquinas de cache pequena — daí
o interesse em remedir o ponto de virada.

### 5.3 Balanceamento de carga por thread

**Pergunta:** quanto tempo cada thread fica ociosa esperando as demais
na barreira final, sob distribuição estática vs. dinâmica?

| Parâmetro | Valor |
|---|---|
| Corpus | Enron ×8 (10,6 GiB) |
| Dicionário | Snort completo |
| Variantes | `pthread_chunked_v2`, `pthread_chunked_flat`, `pthread_dynamic`, `pthread_dynamic_flat` |
| Threads | 32 |
| Medição | decomposição por thread (tempo e trabalho de cada worker) |
| Iterações | 1 aquecimento + 3 cronometradas |
| Execuções | **4** |

Mede diretamente a qualidade do escalonamento: nas variantes estáticas, a
ociosidade na barreira; nas dinâmicas, a dispersão do número de tarefas
consumidas por thread. É a evidência por telemetria do efeito observado
na curva de escalabilidade.

### 5.4 Custo de construção do autômato (sequencial vs. paralela)

**Pergunta:** a construção do autômato se beneficia de paralelismo, e a
partir de que tamanho de dicionário?

| Parâmetro | Valor |
|---|---|
| Dicionários | 100 regras, 1.000 regras, Snort completo, ET-32 |
| Construção | sequencial vs. paralela (busca em largura nível-síncrona) |
| Threads (construção paralela) | 2, 4, 8, 16, 32 |
| Medição | apenas tempo de construção (a varredura é irrelevante aqui) |
| Execuções | 4 dicionários × (1 sequencial + 5 paralelas) = **24** |

Avalia a latência de **recarga do conjunto de regras** — métrica
operacional independente da vazão de varredura.

---

## 6. Resumo quantitativo

| Bloco de teste | Pergunta | Execuções |
|---|---|---:|
| 5.1 Escalabilidade vs. threads | curva de speedup; emissão e escalonamento | 60 |
| 5.2 Sensibilidade ao dicionário | ponto de saturação de cache | 24 |
| 5.3 Balanceamento por thread | ociosidade estática vs. dinâmica | 4 |
| 5.4 Construção do autômato | tempo de build sequencial vs. paralelo | 24 |
| **Total (conjunto principal)** | | **112** |
