# Datasets

Os benchmarks usam **regras reais de IDS** como dicionário e **corpus
textuais grandes** como alvo de varredura. A escolha é deliberada:
reproduz a literatura sobre Aho–Corasick em contexto de Intrusion
Detection Systems (IDS) e produz autômatos cujo tamanho extrapola
caches comuns, expondo de fato o impacto do paralelismo.

## Pipeline de aquisição

```mermaid
flowchart LR
    subgraph Padrões
        direction TB
        SR[Snort 3 Community<br/>.rules] --> ESN[extract_snort_patterns.py<br/>parse 'content:' + |hex|]
        YR[YARA rules] --> EY[extract_yara_patterns.py]
        ESN --> PSN[(data/patterns_snort.txt)]
        EY  --> PYR[(data/patterns_yara.txt)]
        EX[Manuais<br/>(EN, code, exploits)] --> PEX[(data/patterns_*.txt)]
    end

    subgraph Corpus
        direction TB
        WK[Simple Wikipedia<br/>XML dump] --> ACQ[acquire_corpus.sh<br/>+ stripper Python]
        EN[Enron Email Dataset<br/>~1.7 GB tar] --> PRP[prepare_datasets.sh<br/>concat de todos os emails]
        ACQ --> WTX[(data/simplewiki.txt)]
        PRP --> ETX[(data/enron_corpus.txt)]
    end

    PSN --> Bench[build/aclab --patterns ... --input ...]
    WTX --> Bench
    ETX --> Bench
```

## Padrões

### `patterns_snort.txt`

- **Fonte**: [Snort 3 Community Rules](https://www.snort.org/downloads/#rule-downloads).
- **Como é gerado**: `scripts/prepare_datasets.sh` baixa o tarball
  oficial, extrai, e roda `scripts/extract_snort_patterns.py` que
  parseia os campos `content:"..."` (e `uricontent:"..."`), incluindo
  segmentos hexadecimais `|DE AD BE EF|`. Regexes (`pcre:`) e
  modificadores dinâmicos são descartados.
- **Por que importa**: gera um autômato representativo de um IDS real,
  com milhares de padrões de tamanhos heterogêneos. O footprint
  resultante (em geral centenas de KiB a alguns MiB) coloca pressão
  sobre a hierarquia de cache, evidenciando o que importa em um
  benchmark de pattern matching paralelo.

### Outras coleções

- `patterns_en.txt`, `patterns_code.txt` — listas hand-crafted para
  reprodutibilidade dos testes iniciais. Geradas por
  `scripts/acquire_corpus.sh`.
- `patterns_et*.txt`, `patterns_exploit.txt` — extrações
  intermediárias (Emerging Threats etc.) usadas durante a fase
  exploratória do TCC.

### Princípios de extração

Documentado em `scripts/extract_snort_patterns.py` e
`scripts/extract_yara_patterns.py`:

1. **Extraia apenas strings literais**. Regexes precisam de outro
   engine.
2. **Aceite segmentos hex** (`|FF SMB 73|`) e converta para os bytes
   originais.
3. **Respeite `nocase`** (lower-case quando o modificador aparece).
4. **Descarte padrões muito curtos** (geralmente < 4 bytes) — eles
   geram matches densos sem valor científico para um benchmark de
   throughput.
5. **Deduplique** — múltiplas regras podem reusar o mesmo `content:`.

## Corpora

### `enron_corpus.txt` (recomendado para o TCC)

- **Fonte**: [CMU Enron Email Dataset](https://www.cs.cmu.edu/~./enron/).
- **Tamanho**: ~1.4 GiB de texto contínuo concatenado a partir de
  ~500 mil emails.
- **Geração**: `scripts/prepare_datasets.sh` baixa o tarball, extrai
  para `data/enron/maildir/`, e roda
  `find ... | xargs cat > data/enron_corpus.txt`.
- **Por que importa**: tamanho >> cache, o que faz a fase de busca
  dominar o tempo total e permite reportar speedup confiável até nº
  de cores físicos.

### `simplewiki.txt`

- **Fonte**: dump da Wikipedia simples (inglês).
- **Tamanho**: ~200 MiB de texto.
- **Geração**: `scripts/acquire_corpus.sh`.
- **Uso**: iterações rápidas durante o desenvolvimento (build + run
  fica abaixo de 10s).

### Inputs sintéticos

`scripts/run_benchmarks.sh` gera um arquivo aleatório de tamanho
configurável (default 64 MiB, ajustável via `BYTES=...`) sobre um
alfabeto restrito (alfanumérico + espaço/newline). Útil para
ablações controladas — você decide a densidade de matches escolhendo
os padrões.

## Reproduzindo

```text
# Padrões + corpus do TCC:
./scripts/prepare_datasets.sh

# Corpus rápido (Simple Wikipedia):
./scripts/acquire_corpus.sh

# Tudo já preparado? Rode:
./scripts/run_snort_enron_benchmarks.sh        # Snort + Enron
./scripts/run_benchmarks.sh                    # sweep sintético
```

A pasta `data/` é **gitignored** (com exceção dos `patterns_*.txt`
versionados e do `data/README.md`). Cada usuário recria os datasets
do zero a partir dos scripts.

## Notas de tamanho e tempo

| Recurso                  | Tamanho aproximado    | Onde mora             |
|--------------------------|------------------------|------------------------|
| Snort tarball            | ~5 MiB                 | `data/snort/`         |
| Snort .rules extraídos   | ~50 MiB                | `data/snort/rules/`   |
| `patterns_snort.txt`     | dezenas a centenas KiB | `data/`               |
| Enron tarball            | ~430 MiB               | `data/enron/`         |
| Enron maildir descompactado | ~3 GiB              | `data/enron/maildir/` |
| `enron_corpus.txt`       | ~1.4 GiB               | `data/`               |

A primeira execução de `prepare_datasets.sh` em uma máquina nova
demora alguns minutos (download + descompactação + extração). Depois,
as execuções subsequentes são quase instantâneas se os arquivos já
existem.
