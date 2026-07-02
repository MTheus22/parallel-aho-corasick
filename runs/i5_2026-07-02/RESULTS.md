# Run i5_2026-07-02 — piloto R (curvas replicadas) + H (corpus skew)

Piloto no **i5-1235U** (2 P-cores/4 threads lógicos = CPUs 0–3; 8 E-cores =
CPUs 4–11; L3 12 MiB), commit `172ee0f` (`runs skew`). O run executou de
2026-07-02 04:01:05 a 06:30:00 -03:00, com governor `performance`, e fechou com
**255 ok / 0 skipped / 0 failed** em 2h28m.

Este run **não é canônico para o TCC**. Ele serve para responder duas perguntas
metodológicas pendentes no i5: (1) quanto a medição muda entre processos
independentes; (2) se o corpus `clustered`, mantendo bytes e matches totais,
cria o desbalanceamento que a fila dinâmica deveria corrigir.

## Sanidade

- `runs`: 155 linhas em `R_replicated` e 100 linhas em `H_skew`.
- `worker_metrics`: 1 200 linhas, todas da fase H, com `cpu` preenchido
  (`0..11`).
- Matches consistentes:
  - `R_replicated`: `patterns_snort/enron_corpus` = 17 853 987 matches em todas
    as 155 linhas.
  - `H_skew`, Snort: uniform e clustered = 94 304 128 matches.
  - `H_skew`, ET_32: uniform e clustered = 141 136 256 matches.
- Bytes dos corpora skew: 4 294 967 296 bytes por corpus.

## Fase R — réplicas independentes

Baseline sequencial mediano: **246,7 MB/s**. A curva cresce bem até T=8, mas
T=12 fica instável e frequentemente pior que T=8. Isso confirma que, no i5
híbrido saturado, medir só uma invocação é frágil: o resultado pode cair em um
modo rápido ou lento dependendo de escalonamento, turbo/temperatura e mistura
P/E.

Medianas de throughput (MB/s) por T:

| Searcher | T=1 | T=2 | T=4 | T=8 | T=12 |
|---|---:|---:|---:|---:|---:|
| `pthread_chunked_v2` | 246,3 | 439,8 | 619,8 | 1059,3 | 701,2 |
| `pthread_chunked_v3` | 245,9 | 440,6 | 605,9 | 1054,1 | 755,3 |
| `pthread_chunked_flat` | 259,5 | 459,9 | 651,7 | 1115,6 | 1071,9 |
| `pthread_chunked_v3_flat` | 258,8 | 459,9 | 639,7 | 1112,4 | 841,3 |
| `pthread_dynamic` | 246,8 | 439,0 | 694,9 | 1078,9 | 710,5 |
| `pthread_dynamic_flat` | 258,9 | 458,3 | 727,4 | 1145,7 | 912,1 |

No ponto saturado T=12, a leitura por mediana é:

| Searcher | Mediana MB/s | Speedup mediano | IQR/mediana | Min–max/mediana | CV intra mediano |
|---|---:|---:|---:|---:|---:|
| `pthread_chunked_flat` | 1071,9 | 4,34× | 48,0% | 70,9% | 56,7% |
| `pthread_dynamic_flat` | 912,1 | 3,70× | 86,7% | 99,2% | 47,4% |
| `pthread_chunked_v3_flat` | 841,3 | 3,41× | 101,0% | 104,3% | 67,1% |
| `pthread_chunked_v3` | 755,3 | 3,06× | 10,1% | 12,0% | 64,2% |
| `pthread_dynamic` | 710,5 | 2,88× | 95,1% | 105,2% | 48,9% |
| `pthread_chunked_v2` | 701,2 | 2,84× | 27,8% | 106,8% | 48,3% |

Conclusão da R: T=8 é o ponto mais defensável neste i5 para curvas comuns. T=12
mede "usar todos os threads lógicos do laptop", mas não um regime estável. Para
texto acadêmico, use mediana + IQR/min-max; não cite uma réplica isolada.

## Fase H — corpus skew

Esta é a parte mais informativa do run. Como uniform e clustered têm os mesmos
bytes e o mesmo total de matches, o delta vem da **distribuição espacial do
trabalho**, não de "mais dados" ou "mais matches".

Resumo por mediana de 5 processos independentes:

| Patterns | Searcher | Uniform MB/s | Clustered MB/s | Delta | Spread uniform | Spread clustered | Ociosidade uniform | Ociosidade clustered |
|---|---|---:|---:|---:|---:|---:|---:|---:|
| Snort | `pthread_chunked_flat` | 473,1 | 333,2 | -29,6% | 17,4% | 175,5% | 6,7% | 55,5% |
| Snort | `pthread_chunked_v2` | 446,4 | 317,0 | -29,0% | 18,0% | 134,4% | 7,0% | 48,9% |
| Snort | `pthread_chunked_v3` | 394,9 | 316,7 | -19,8% | 5,9% | 173,4% | 2,6% | 57,1% |
| Snort | `pthread_dynamic` | 371,6 | 430,8 | +15,9% | 18,5% | 8,1% | 7,4% | 2,7% |
| Snort | `pthread_dynamic_flat` | 390,6 | 456,8 | +16,9% | 21,0% | 6,9% | 7,8% | 3,0% |
| ET_32 | `pthread_chunked_flat` | 287,8 | 241,0 | -16,2% | 13,9% | 98,0% | 5,6% | 39,6% |
| ET_32 | `pthread_chunked_v2` | 238,7 | 196,0 | -17,9% | 15,9% | 103,1% | 6,6% | 40,4% |
| ET_32 | `pthread_chunked_v3` | 193,3 | 206,4 | +6,8% | 8,4% | 62,4% | 2,6% | 31,1% |
| ET_32 | `pthread_dynamic` | 205,1 | 261,8 | +27,7% | 15,0% | 10,7% | 5,3% | 4,4% |
| ET_32 | `pthread_dynamic_flat` | 245,3 | 320,8 | +30,7% | 13,5% | 12,4% | 5,7% | 4,6% |

Veredito: **o mecanismo foi confirmado neste piloto**. No corpus clustered, os
searchers estáticos perdem throughput e explodem o spread por worker; os
dinâmicos mantêm o spread baixo e passam a vencer. O efeito é mais limpo em
Snort, mas também aparece em ET_32. `pthread_chunked_v3` ajuda quando o problema
é P/E/topologia no corpus uniforme, mas não resolve conteúdo concentrado: em
Snort clustered, seu spread sobe para 173,4% e a ociosidade de barreira para
57,1%.

Exemplo concreto em Snort clustered:

- `pthread_chunked_flat`, `rep03`: workers 0–2 carregam ~19,2 M matches cada e
  levam 12,3–13,3 s; os demais ficam em ~4,1 M matches e 2,7–3,8 s.
- `pthread_dynamic_flat`, `rep04`: os tempos ficam entre 10,95 s e 11,31 s,
  apesar de matches por worker variando de ~6,8 M a ~9,9 M.

O campo `cpu=` reforça que o straggler do clustered está nos chunks quentes, não
apenas em E-core: nos estáticos Snort clustered, o worker mais lento cai sempre
entre workers 0–2 e amostra CPU P no fim do scan. Como `cpu` é amostrado apenas
ao final, isso não prova ausência de migração, mas é suficiente para não reduzir
o resultado a "caiu em E-core".

## Implicações

- Para **defender dinâmica por balanceamento de conteúdo**, este piloto é melhor
  evidência que Enron uniforme no i5 histórico. Ele cria o cenário em que a
  fila dinâmica tem trabalho real a redistribuir.
- Para **headline do TCC**, ainda falta repetir H na workstation homogênea. O i5
  mistura skew de conteúdo com heterogeneidade P/E, então o resultado é
  diagnóstico, não canônico.
- Para o dashboard, este run deve ser lido por mediana/IQR nas páginas
  "Réplicas R" e "Skew H"; as curvas comuns com linhas cruas podem induzir a
  leitura errada quando cada ponto é uma réplica.

## Limitações

- **Confundido por P/E**: skew de conteúdo + heterogeneidade de cores agem
  juntos; o contraste válido é *within-machine* (uniform×clustered, confundidor
  ~constante). **Não substitui a coleta canônica na workstation homogênea.**
- **Não-canônico**: nada daqui vira headline do TCC nem entra em
  `partes/*.tex` sem decisão explícita do autor; runs canônicos =
  `runs/workstation_2026-06-30/` e `runs/i5/` (ver `runs/MANIFEST.md`).
- Governor verificado em `env/start.txt` e `env/end.txt`: `performance`.
  Telemetria térmica chegou a 93 °C em zonas térmicas durante o sweep; isso é
  compatível com a instabilidade observada em T=12 e deve ser tratado como fator
  de ambiente do piloto.
- `cpu` é amostrado no FIM do scan de cada worker — migrações no meio do laço
  não aparecem.
