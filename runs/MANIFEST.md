# Manifesto dos Runs Preservados

Estado preservado atualizado em 2026-07-02: este diretorio mantem apenas as
bases de resultado que ainda tem valor para o TCC ou para pilotos metodologicos
documentados.

## Fontes Mantidas

| Diretorio | Status | Uso correto |
|-----------|--------|-------------|
| `workstation_2026-06-30/` | Fonte canonica do TCC | Headline de desempenho na Ryzen 9 9950X: fases A B C D E G, commit unico `c19da78`, `ok=522`, `skip=0`, `fail=0`, correctness 100%. Use `sweep.db`, `sweep.csv` e `RESULTS.md` como referencia principal. |
| `i5/` | Evidencia historica P/E | Usar somente em uma secao eventual sobre arquitetura hibrida P/E no Intel i5-1235U, principalmente para discutir o efeito das variantes topology-aware `pthread_chunked_v3` e `pthread_chunked_v3_flat`. Nao usar como headline nem como comparacao direta justa contra a workstation. |
| `i5_2026-07-02/` | Piloto exploratorio R+H | Run no i5 para curvas replicadas (`R_replicated`) e corpus skew (`H_skew`), commit `172ee0f`, `ok=255`, `skip=0`, `fail=0`. Use para metodologia, variancia entre replicas e diagnostico do mecanismo de balanceamento dinamico; nao usar como headline canonico. |

## Politica

- A workstation de 2026-06-30 e a unica fonte para numeros principais da tese.
- O i5 existe para explicar heterogeneidade P/E, nao para competir com a coleta
  canonica da workstation.
- `i5_2026-07-02/` existe para documentar o piloto R+H; os achados de skew
  precisam ser repetidos em CPU homogenea antes de virarem resultado principal.
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
- O corpus canonico da workstation ainda e essencialmente uniforme. O piloto i5
  com corpus skewed confirma a direcao do mecanismo, mas a demonstracao
  principal ainda precisa de uma coleta em CPU homogenea.
