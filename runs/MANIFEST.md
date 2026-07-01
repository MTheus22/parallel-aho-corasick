# Manifesto dos Runs Preservados

Estado preservado em 2026-07-01: este diretorio mantem apenas as bases de
resultado que ainda tem valor para o TCC.

## Fontes Mantidas

| Diretorio | Status | Uso correto |
|-----------|--------|-------------|
| `workstation_2026-06-30/` | Fonte canonica do TCC | Headline de desempenho na Ryzen 9 9950X: fases A B C D E G, commit unico `c19da78`, `ok=522`, `skip=0`, `fail=0`, correctness 100%. Use `sweep.db`, `sweep.csv` e `RESULTS.md` como referencia principal. |
| `i5/` | Evidencia historica P/E | Usar somente em uma secao eventual sobre arquitetura hibrida P/E no Intel i5-1235U, principalmente para discutir o efeito das variantes topology-aware `pthread_chunked_v3` e `pthread_chunked_v3_flat`. Nao usar como headline nem como comparacao direta justa contra a workstation. |

## Politica

- A workstation de 2026-06-30 e a unica fonte para numeros principais da tese.
- O i5 existe para explicar heterogeneidade P/E, nao para competir com a coleta
  canonica da workstation.
- Runs antigos, interinos, mistos ou incompletos nao devem ser citados no texto
  final.
- Novos experimentos devem entrar em um novo diretorio `runs/<slug-data>/` com
  `MASTER.log`, `env/`, `sweep.csv`, `sweep.db` e, quando promovidos, um
  `RESULTS.md`.
- Nao sobrescreva um run promovido. Se precisar corrigir ou repetir, crie outro
  diretorio e documente a decisao de promocao.

## Lacunas Conhecidas

- A coleta canonica ainda e single-run por configuracao. Para numeros mais
  defensaveis estatisticamente, rode replicas independentes das configuracoes
  chave e reporte mediana e intervalo interquartil.
- O corpus atual e essencialmente uniforme. Um corpus skewed ainda e necessario
  se a tese quiser demonstrar quando o escalonamento dinamico e necessario por
  desbalanceamento espacial de carga.
