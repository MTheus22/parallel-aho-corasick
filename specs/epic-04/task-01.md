# Task 01 — Gerador de corpus skew (`make_skewed_corpus.sh`)

## Objetivo

Criar `scripts/make_skewed_corpus.sh` que produz um **par de corpora com bytes
totais idênticos E contagem de matches idêntica**, diferindo apenas na
distribuição espacial da carga: um com os blocos "quentes/caros" **espalhados
uniformemente** e outro com os mesmos blocos **agrupados** no início. O
mecanismo é a **reordenação** de um multiset fixo de blocos — o que garante a
paridade por construção. Suporta um **fator de skew** tunável para gerar uma
família.

## Escopo

- **In scope:** `scripts/make_skewed_corpus.sh`; saídas em `data/` (gitignored);
  reuso de `data/enron_corpus.txt` e `data/patterns_snort.txt` já existentes;
  binário `build/aclab` para medir densidade/matches.
- **Out of scope:** editar searchers; tocar padrões (Snort/ET ficam intactos);
  qualquer pcap/tráfego real; integrar ao `run_sweep.sh` (Task 02).

## Implementação

1. **Blocos.** Fatie `data/enron_corpus.txt` em blocos de tamanho fixo
   `BLOCK=$((8*1024*1024))` (8 MiB). Defina dois tipos:
   - **Frio (barato):** bloco de Enron limpo, **sem** injeção → fica nos estados
     rasos/quentes do DFA (poucos matches, poucas cache misses na `goto_tbl`).
   - **Quente (caro):** bloco de Enron **com prefixos Snort injetados** (passo 2)
     até densidade-alvo → força o DFA a estados profundos/frios + mais emissão.
2. **Injeção de densidade (método FHBM).** Amostre padrões de
   `data/patterns_snort.txt`, tome um **prefixo de ≥80% do comprimento** de cada
   um, e insira em posições aleatórias do bloco quente até a densidade de match
   atingir o alvo `DENSITY=${DENSITY:-0.42}` (≈42%, faixa DEFCON *ugly* 43–45%).
   Meça a densidade rodando `build/aclab --patterns patterns_snort.txt --input
   <bloco> --searcher sequential` e contando matches/bytes; ajuste o número de
   injeções por bissecção simples até ficar dentro de ±2 pontos percentuais.
   Registre a densidade obtida.
3. **Multiset fixo.** Monte `N_HOT` blocos quentes e `N_COLD` blocos frios
   (default: arquivo-alvo ~`SIZE=${SIZE:-4}` GiB → total de blocos
   `SIZE_GiB*1024/8`; `HOT_FRAC=${HOT_FRAC:-0.25}` da carga é quente →
   `N_HOT = round(total*HOT_FRAC)`, `N_COLD = total - N_HOT`). **Gere cada bloco
   quente uma única vez** e reutilize por referência ao concatenar — os mesmos
   bytes aparecem nos dois arquivos de saída.
4. **Duas ordenações (a única diferença):**
   - `data/enron_skew_uniform.txt`: intercale quente/frio **espalhando** os
     blocos quentes por igual (ex.: a cada `round(total/N_HOT)` blocos, um
     quente). Carga ~uniforme por chunk estático.
   - `data/enron_skew_clustered.txt`: **todos** os blocos quentes primeiro
     (concentrados na fração inicial `SKEW=${SKEW:-0.25}` do arquivo), depois
     todos os frios. Poucos chunks estáticos absorvem toda a carga.
   Como o **conjunto de blocos é idêntico**, bytes e matches totais são iguais
   por construção — só a ordem muda.
5. **Fator de skew (família, opcional via `SKEW`):** aceite `SKEW ∈ {1.0, 0.5,
   0.25, 0.1}` — `1.0` = uniforme; menor = mais concentrado no início. Gere
   `data/enron_skew_s<SKEW>.txt` para a curva "spread × skew". `uniform` é o
   caso `SKEW=1.0`; `clustered` o default `0.25`.
6. **Self-check de paridade (obrigatório):** ao final, rode `build/aclab
   --patterns data/patterns_snort.txt --input <arquivo> --searcher sequential`
   nos dois arquivos e verifique: `bytes(uniform) == bytes(clustered)` e
   `matches(uniform) == matches(clustered)`. Aborte com erro se divergirem.
   Imprima um resumo: bytes, matches, densidade global, `N_HOT`, `N_COLD`, skew.
7. **Idempotência/resume:** se o arquivo de saída já existe e passa no
   self-check, pule (padrão dos outros scripts do repo). Sementes de RNG fixas
   (`SEED=${SEED:-1234}`) para reprodutibilidade.

## Validação

```bash
# gera o par default (~4 GiB cada, clustered no 1º quarto)
./scripts/make_skewed_corpus.sh
# paridade (o próprio script já falha se divergir; confirme à mão)
build/aclab --patterns data/patterns_snort.txt --input data/enron_skew_uniform.txt   --searcher sequential | grep -E '^sequential'
build/aclab --patterns data/patterns_snort.txt --input data/enron_skew_clustered.txt --searcher sequential | grep -E '^sequential'
# os dois devem reportar a MESMA contagem de matches e o mesmo nº de bytes
ls -l data/enron_skew_uniform.txt data/enron_skew_clustered.txt   # tamanhos idênticos
```

## Critérios de Aceite

- `scripts/make_skewed_corpus.sh` existe, é executável e roda a partir da raiz do
  repo com `build/aclab` já compilado.
- `data/enron_skew_uniform.txt` e `data/enron_skew_clustered.txt` têm **byte
  count idêntico** (`stat -c%s` iguais).
- `build/aclab … --searcher sequential` reporta **contagem de matches idêntica**
  para os dois arquivos (a paridade de Ródenas).
- A densidade de match dos blocos quentes fica em **40–45%** e é impressa no
  resumo.
- Padrões (`patterns_snort.txt`) **não** foram modificados.
- `SKEW` gera a família de arquivos sem quebrar a paridade byte/match.
- Reexecução pula arquivos já válidos (resume) e usa seed fixa.
