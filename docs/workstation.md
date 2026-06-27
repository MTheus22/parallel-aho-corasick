# Parecer — corrida única na Workstation (AMD Ryzen 9 9950X)

> **Status:** PRONTO PARA EXECUTAR, com 6 ressalvas operacionais que precisam ser
> tratadas *antes* de você sair da máquina (seção 4). A infraestrutura de teste
> (scripts, grade, resiliência, extração) já existe, está revisada e foi
> verificada arquivo a arquivo neste parecer. O risco residual **não está no
> código do sweep** — está no *provisionamento da máquina alugada*, na
> *transferência por pendrive* e em *garantir que o processo sobreviva à noite*.
>
> **Escopo deste documento:** revisão de execução e risco (go/no-go). A
> justificativa científica da grade (quais searchers, por que, quantas runs)
> já está em `parallel-aho-corasick/docs/sweep-test-inventory.md`,
> `docs/testes-workstation.md` e `runs/workstation/README.md`. Não a repito;
> aponto para ela.

---

## 1. Veredito em uma página

| Pergunta | Resposta |
|---|---|
| A grade de testes está definida e justificada? | **Sim.** 112 runs núcleo (A B D E) + 4 opcionais (S). Calibrada para cores homogêneos (descarta `v3`/`v3_flat`, foca estático vs. dinâmico). |
| O script é à prova de falhas? | **Em grande parte.** Resume-from-crash por run, `flock`, traps SIGINT/TERM, `.FAIL` não derruba o sweep, snapshot de ambiente rico. Os pontos cegos são externos ao script (abaixo). |
| Os dados existem? | **Sim, no laptop.** `enron_corpus.txt` (1,42 GB), `patterns_et_32.txt` (797 KiB — **insubstituível**), `patterns_snort.txt`. `enron_x8.txt` é regenerável no host. |
| Posso confiar nos números no dia seguinte? | **Só depois de checar** `v_correctness = 1` e zero `.FAIL` (seção 9). |
| Cabe numa noite? | **Folgado.** Estimativa 3–5 h para A B D E S (seção 8). |

**Os 6 itens que podem arruinar a corrida única** (detalhados na seção 4):

1. **Binário errado** — copiar `build/` do laptop (`-march=native` para Alder
   Lake) faz o sweep *não* recompilar e medir um binário subótimo no Zen 5.
2. **Pendrive FAT32** — limite de 4 GiB por arquivo mata a cópia de
   `enron_x8.txt` (10,6 GiB) silenciosamente. (Solução: não copie esse arquivo.)
3. **RAM insuficiente** — `enron_x8` é `mmap` com `MAP_POPULATE` → ~10,6 GiB
   residentes; sem folga, OOM/swap derruba as fases A/D.
4. **Rodar a partir do pendrive** — `MAP_POPULATE` lendo de USB a cada run
   destrói o throughput e a comparação.
5. **A máquina suspender / a sessão SSH cair** — `nohup` sozinho não basta no
   Ubuntu moderno (logind mata processos da sessão no logout). ✔ **Já tratado
   pelo wrapper `workstation_all.sh`** (`setsid` + `systemd-inhibit`).
6. **Sem `sudo`** — sem governador `performance` a variância sobe; recuperável,
   mas precisa ser uma decisão consciente (não uma surpresa nos dados). ✔ **O
   wrapper tenta e segue sem travar se não houver sudo.**

> Os 6 itens acima estão **resolvidos pelo fluxo de um comando** (seção 6) +
> pela transferência por pendrive correta (seção 5). Esta seção 1 é o "porquê";
> a 4 detalha cada um.

---

## 2. O que vai ser medido (contexto mínimo)

TCC sobre **paralelização do Aho–Corasick em CPU multicore de memória
compartilhada (Pthreads)**, aplicado a IDS (dicionários Snort/Emerging-Threats
varrendo corpus Enron em escala de GiB). Métricas: **MB/s, Gbps, speedup vs. nº
de threads**, mais tempo por thread e tempo de construção do autômato.

Esta corrida **não substitui** a fonte de verdade da tese (o sweep do laptop
i5-1235U, `runs/i5/sweep.db`, 2026-05-29). Ela entra como **seção de
portabilidade/generalização**: o mesmo experimento num chip de **16 núcleos
homogêneos / 32 threads, L3 de 64 MiB *não unificado* (2 CCDs × 32 MiB),
DDR5-5600**. Três coisas novas e citáveis que só este hardware dá:

- **Escalabilidade real até 16 cores físicos + SMT** (a curva 1→2→4→8→16→24→32
  atravessa: dentro de 1 CCD ≤8, fronteira entre CCDs 8→16, ganho marginal de
  SMT 16→32).
- **Cross-over de cache deslocado**: `patterns_snort` (~56 MiB de autômato) cai
  *entre* o L3 de um CCD (32 MiB) e o total (64 MiB) — ponto de saturação novo.
- **Contraste estático vs. dinâmico limpo** em cores idênticos: `chunked_v2` vs.
  `dynamic` (encadeada) e `chunked_flat` vs. `dynamic_flat` (flat). O
  `pthread_dynamic_flat` é o candidato a campeão homogêneo.

---

## 3. Ativos já existentes (e verificados neste parecer)

| Ativo | Caminho | Estado |
|---|---|---|
| **Wrapper "um comando"** | `scripts/workstation_all.sh` | ✨ **criado neste parecer.** dados→build→test→sweep→empacota; auto-background imune a suspend/logout. Sintaxe validada; mecanismo de desacoplamento testado. |
| Driver do sweep | `scripts/run_workstation_sweep.sh` | ✔ revisado. Default `A B D E`; `S` opcional. Lock, resume, traps, snapshot. |
| Pré-flight de dados | `scripts/prepare_workstation_data.sh` | ✔ valida/gera dados; **PARA** se `patterns_et_32.txt` faltar (insubstituível). Checa disco e tamanho exato do `enron_x8`. |
| Extratores | `scripts/extract_sweep_csv.py` + `build_sweep_db.py` | ✔ `--known-only` reconhece os 4 searchers da grade + baselines (confirmado linha 25–31). Geram `sweep.csv` + `sweep.db` SQLite. |
| Contrato de saída | `src/main.c` (getopt_long) | ✔ `--patterns/--input/--searcher/--threads/--warmup/--iters/--per-thread` existem; `AC_BUILD_PARALLEL`/`AC_BUILD_THREADS` lidos. |
| Doc auto-contido | `runs/workstation/README.md` | ✔ explica run + análise (views SQLite). Já versionado. |
| Justificativa da grade | `docs/sweep-test-inventory.md`, `docs/testes-workstation.md` | ✔ fonte canônica do *porquê* de cada teste. |

Verificações que fiz no laptop agora:

- `data/` tem **tudo**: `enron_corpus.txt` (1.421.183.736 B), `enron_x8.txt`
  (11.369.469.888 B = **exatamente** 8×, o pré-flight vai aceitar),
  `patterns_et_32.txt` (816.148 B), `patterns_snort.txt` (88.716 B).
- **Não existe `build/`** no laptop agora → baixo risco de levar binário velho
  *se você copiar agora*. Mesmo assim, force `make clean` no host (seção 4.1).
- `.gitignore`: `data/*` e `build/` são ignorados; `runs/*` ignorado **com
  exceção** `!runs/workstation/` → os resultados são versionáveis. `git remote`
  = `git@github.com:MTheus22/parallel-aho-corasick.git` (SSH).
- Disco do laptop: 325 GiB livres. RAM 31 GiB.

**Testes ao vivo do wrapper (mesmo Ubuntu da workstation):**

- ✅ `bash -n` (sintaxe) + mecanismo de desacoplamento (`setsid`+`systemd-inhibit`):
  validei que o sweep roda em **sessão própria** e sobrevive à saída do pai.
- ✅ Lógica do wrapper com stubs: **caminho feliz** (fases 1→4 em ordem, `PHASES`
  propagado, background com PID, `ws-results.tgz` gerado) **e os 2 aborts**
  (pré-flight NÃO PRONTO → para no [1/4]; build falho → para no [3/4]; ambos
  exit 1, sem iniciar o sweep).
- ✅ **Build real** `-O3 -march=native` (<1 s, 12 searchers registrados,
  incluindo `pthread_dynamic_flat`) **e `make test` real → "All correctness
  tests PASSED"** em `{1,2,3,4,7,8}`. Logo, o passo [3/4] passa neste SO.
- ✅ **Pendrive inspecionado:** `/dev/sda1` (`/media/matheusbarros/2776-BEDC`),
  **FAT32, ~19 GB livres** → confirma 4.2 (não copiar `enron_x8`); payload
  enxuto (~1,5 GB) cabe e respeita o limite de 4 GiB. `sudo` **pede senha** e
  `cpupower` existe → o passo [2/4] vai pedir a senha (no foreground, ok).

---

## 4. Análise de risco (o núcleo do parecer)

Ordenado por severidade. Cada item: *o que é → por que mata a corrida → mitigação*.

### 🔴 CRÍTICO — invalida ou derruba a corrida

**4.1 Binário compilado para a CPU errada (`-march=native`).**
O Makefile usa `-O3 -march=native`. Compilado no laptop, o binário é gerado
para Alder Lake e **não exercita o Zen 5** (codegen e ISA diferentes; no melhor
caso roda subótimo, no pior `SIGILL`). Pior: o sweep só recompila *se* o binário
faltar (`if [[ ! -x build/aclab ]]`), e mesmo `make -j` com `.o` copiados não
reconstrói. → **Resultado: você mede o chip errado e nem percebe.**
**Mitigação:** **não leve `build/` no pendrive** (já é gitignored; exclua no
rsync) e, no host, rode `make clean && make` *explicitamente* antes do sweep.
Compilar **na** workstation é obrigatório.

**4.2 Pendrive FAT32 (limite de 4 GiB por arquivo).**
Se o pendrive for FAT32 (padrão de fábrica de muitos), copiar `enron_x8.txt`
(10,6 GiB) **falha em silêncio ou corta o arquivo em 4 GiB**.
**Mitigação:** **não copie `enron_x8.txt`** — regenere no host (seção 5). Assim
o maior arquivo no pendrive vira `enron_corpus.txt` (1,42 GB < 4 GiB, cabe em
FAT32). Se *insistir* em copiar o `x8`, formate o pendrive como exFAT/NTFS/ext4
antes.

**4.3 RAM insuficiente para `enron_x8` (`MAP_POPULATE` ~10,6 GiB residentes).**
As fases A e D varrem `enron_x8` mapeado com `MAP_POPULATE` → ~10,6 GiB de
páginas residentes *por run*, mais o autômato (até ~515 MiB no ET-32). Sem
folga, vira swap/OOM e as runs viram `.FAIL`. Além disso, se a RAM for justa, o
*page cache* do corpus é despejado entre runs e cada run relê 10,6 GiB do disco.
**Mitigação:** confirme **`free -h` com ≳ 20–24 GiB livres** antes do sweep (um
9950X de workstation costuma ter 32–64 GiB; confirme). O snapshot
`env/start.txt` registra isso, mas confirme *antes*, não depois.

**4.4 Rodar com os dados no pendrive (em vez do disco local).**
`MAP_POPULATE` lendo 10,6 GiB de um USB a cada run colapsa o throughput e mede a
banda do pendrive, não da DRAM. **Mitigação:** copie o repo + `data/` para o
**SSD/NVMe local** do host (ex.: `~/pac/`) e rode de lá. Nunca rode de `/media/...`.

### 🟠 ALTO — degrada a qualidade dos dados (corrida ainda "vale", mas com ressalva)

**4.5 Sobrevivência do processo à noite (suspensão + queda de sessão).**
Dois problemas distintos:
- *Suspensão/idle*: desktop de laboratório pode suspender por inatividade →
  pausa o sweep no meio. **Mitigação:** desabilite suspensão durante o run, ou
  envolva o sweep em `systemd-inhibit` (seção 6).
- *Queda de sessão*: `nohup ... &` sozinho **não** garante sobrevivência no
  Ubuntu moderno — se o `logind` estiver com `KillUserProcesses=yes`, o logout
  mata o processo apesar do `nohup`. **Mitigação:** rode dentro de **`tmux`**
  (ou `screen`), que sobrevive a logout/queda de SSH por design (seção 6). É a
  defesa mais simples e robusta.

**4.6 Governador de frequência sem `sudo`.**
Sem `cpufreq -g performance`, o Zen 5 (`amd-pstate`) pode escalar frequência e
injetar variância (cv% alto). Se você **não** tiver `sudo` na máquina alugada:
o sweep **roda mesmo assim** e registra o governador real em `env/start.txt`. O
`iters=8` da fase A já aperta a variância. **Mitigação:** tente
`sudo cpupower frequency-set -g performance`; se negado, anote o caveat e siga —
mas saiba *na hora*, não no dia seguinte.

**4.7 Throttling térmico a 32 threads / contenção com outros processos.**
`env/thermal.tsv` registra temp + MHz médio por run; cv% alto denuncia
throttling. **Mitigação:** rode com a máquina ociosa (à noite), confira `loadavg`
no `env/start.txt`, e olhe os 2–3 primeiros runs no `MASTER.log`/`thermal.tsv`
antes de ir embora.

### 🟡 MÉDIO — incômodos recuperáveis (já mitigados pelo design)

| Item | Risco | Mitigação |
|---|---|---|
| 4.8 | `... A B` posicional só roda a **primeira** fase | Use o **default** (sem args = `A B D E`) ou a env `PHASES="A B D E S"`. |
| 4.9 | `python3` ausente no host → `finalize` não gera `sweep.db` | Os `.log` são a fonte de verdade; regenere `csv`/`db` no laptop (seção 9.3). Ubuntu tem `python3`. |
| 4.10 | `sqlite3` CLI ausente | Não é necessário para *gerar* o DB (`build_sweep_db.py` usa o módulo stdlib). Só para *consultar* — faça no laptop. |
| 4.11 | `.lock` órfão de tentativa anterior bloqueia o sweep | `rm runs/workstation/.lock` se não houver sweep rodando. |
| 4.12 | `dmidecode`/`numactl`/`lstopo` ausentes ou sem root | Snapshot degrada com `|| true`; só perde a linha "tipo/velocidade de RAM" — preencha a tabela de hardware do TCC manualmente. |

---

## 5. Estratégia de transferência (pendrive)

**Princípio:** leve o **fonte do repo + os dados pequenos**; **regenere os
grandes no host**. Isso minimiza o pendrive, evita o limite FAT32 e elimina o
risco de binário velho.

### O que levar
| Arquivo/dir | Tamanho | Por quê |
|---|---|---|
| repo `parallel-aho-corasick/` (fonte) | ~poucos MB | scripts, Makefile, src, docs, `runs/i5` (logs pequenos). |
| `data/enron_corpus.txt` | 1,42 GB | base do `enron_x8` (regenerado) e corpus das fases B/E. |
| `data/patterns_snort.txt` | 87 KiB | fases A/B/D/E. |
| `data/patterns_et_32.txt` | 797 KiB | **insubstituível — sem ele, fases A/B/E falham em massa.** |

### O que NÃO levar
- `build/` (compile no host — ver 4.1).
- `data/enron_x8.txt` (regenere — evita 10,6 GiB e o limite FAT32; ver 4.2).
- `data/simplewiki*`, `data/enron/`, `data/snort/` (não usados na grade da
  workstation; a fase C foi cortada).

### Comando de cópia (laptop → pendrive)
```bash
# Ajuste o destino do pendrive. Exclui o que deve ser regenerado/compilado no host.
rsync -av --info=progress2 \
  --exclude 'build/' \
  --exclude 'data/enron_x8.txt' \
  --exclude 'data/simplewiki*' \
  --exclude 'data/enron/' \
  --exclude 'data/snort/' \
  /home/matheusbarros/projects/idp/tcc/parallel-aho-corasick/ \
  /media/matheusbarros/2776-BEDC/pac/
```
> **Pendrive confirmado:** `/dev/sda1` em `/media/matheusbarros/2776-BEDC`,
> **FAT32 (vfat), ~19 GB livres**. O payload enxuto tem **~1,5 GB** (fonte 55 MB
> + `enron_corpus.txt` 1,4 GB + `.git` 4,3 MB) — cabe folgado, e o maior arquivo
> (`enron_corpus.txt`, 1,4 GB) fica **abaixo** do limite de 4 GiB do FAT32.
> Mantive o `.git` (4,3 MB) de propósito: preserva o hash do commit no binário/
> snapshot e habilita `git push` (seção 7). **Não** copie `enron_x8.txt` (10,6 GiB
> — FAT32 **cortaria/recusaria**); ele é regenerado no host pelo pré-flight.

### No host (workstation), copie do pendrive para o **disco local**
```bash
cp -a /media/matheusbarros/2776-BEDC/pac /home/$USER/pac     # NUNCA rode do pendrive (4.4)
cd /home/$USER/pac
```

---

## 6. Execução: UM comando (`scripts/workstation_all.sh`)

> **Você NÃO precisa instalar nada** (nem `tmux`). O wrapper usa só
> `nohup`/`setsid`/`systemd-inhibit`, que já vêm em qualquer Ubuntu
> (coreutils/util-linux/systemd). Validei o mecanismo de desacoplamento: o
> sweep roda numa **sessão própria**, imune a logout/queda de SSH, e
> `systemd-inhibit` impede a máquina de dormir durante a corrida.

Depois de copiar o repo + `data/` para o **disco local** (seção 5), o fluxo
inteiro é:

```bash
cd /home/$USER/pac
chmod +x scripts/*.sh                 # FAT32 não guarda o bit +x; garante execução
./scripts/workstation_all.sh          # roda A B D E S (PHASES="A B D E" p/ pular o opcional)
```

O que o wrapper faz, em ordem, sozinho:

1. **Pré-flight de dados** (`prepare_workstation_data.sh`) — gera `enron_x8`
   (8×) e os dicts reduzidos; **ABORTA se NÃO PRONTO** (não roda sweep com dado
   faltando).
2. **Governador `performance`** — tenta `sudo cpupower`; **se não houver sudo,
   apenas avisa e segue** (não trava). — resolve 4.6
3. **`make clean && make`** no host (binário Zen 5; build é fatal) + **`make
   test`** (correção; se falhar, avisa mas segue — o portão real é
   `v_correctness` amanhã). — resolve 4.1
4. **Mostra `free -h` e `nproc`** para você conferir a RAM antes de sair. — 4.3
5. **Dispara o sweep em background**, imune a **suspensão** (`systemd-inhibit`)
   e a **logout/queda de SSH** (`setsid`), e **empacota** `runs/workstation` em
   `~/ws-results.tgz` ao fim. — resolve 4.5

As etapas 1–4 rodam **em foreground** (~poucos min, você assistindo — pode pedir
senha do sudo). Quando aparecer:

```
 Sweep rodando em BACKGROUND (PID ...). PODE SAIR / DESCONECTAR.
```

…o trabalho longo já está desacoplado: **pode fechar o terminal e ir embora.**

- Acompanhar (opcional, antes de sair): `tail -f runs/workstation/MASTER.log`
  — confira `OK` acumulando (não `FAIL`) e, em `env/thermal.tsv`, MHz estável.
- Reanexar depois? Não precisa: é background real. Só `tail -f` o log quando
  voltar/reconectar.
- Travou tudo no meio e quer recomeçar? Rodar o wrapper de novo **pula o que já
  completou** (resume-from-crash do sweep) — é seguro reexecutar.

---

## 7. Coleta / upload dos resultados — SEM voltar à faculdade

**Boa notícia:** o artefato é minúsculo. `runs/workstation/` final = logs de
texto + `env/` + `sweep.csv` + `sweep.db` (poucos MB; `sweep.db` ~100 KiB). O
wrapper já empacota tudo em `~/ws-results.tgz` ao terminar **e pode publicá-lo
sozinho** (overnight, você dormindo) por canais que você controla. Escolha
pelo menos um e configure-o **hoje**, durante o setup:

**A) `git push` com PAT (mais limpo; você pediu "commitar o sweep").**
`runs/workstation/` é versionável e **não depende de `gh`** (a workstation pode
não ter). Gere um **PAT fine-grained** (escopo *Contents: write* só neste repo) e:
```bash
WS_GIT_PUSH=1 WS_GH_PAT=github_pat_xxx ./scripts/workstation_all.sh
```
O token vai por **ambiente** — não fica gravado em `.git/config` nem aparece em
`ps` (helper inline; a URL HTTPS é derivada do `origin`). De casa: `git pull`.
**Revogue o PAT** depois da corrida.

> Alternativa sem `WS_GH_PAT` (token fica no `.git/config` local — revogue depois):
> `git remote set-url origin https://<PAT>@github.com/MTheus22/parallel-aho-corasick.git`
> e rode `WS_GIT_PUSH=1 ./scripts/workstation_all.sh`.

**B) Sua VPS (scp).** Você controla o destino:
```bash
WS_UPLOAD_CMD='scp "$WS_RESULTS" usuario@suavps:~/' ./scripts/workstation_all.sh
```
(Leve sua chave SSH da VPS para o host, ou use `ssh-copy-id` hoje.)

**C) Zero-setup — nuvem efêmera + aviso no celular (ntfy.sh).** Não instala nada:
```bash
WS_UPLOAD_CMD='U=$(curl -sF "file=@$WS_RESULTS" https://0x0.st); \
  curl -d "TCC sweep pronto: $U" ntfy.sh/SEU-TOPICO-SECRETO' ./scripts/workstation_all.sh
```
Assine `ntfy.sh/SEU-TOPICO-SECRETO` no app/web do celular; de manhã chega o link.

> `WS_GIT_PUSH` e `WS_UPLOAD_CMD` são **independentes e combináveis** (e
> best-effort: não derrubam o run). Recomendo **A + C** juntos: o push entrega
> o resultado versionado, e o ntfy te avisa que terminou (com link de backup).
> O pendrive (cópia de volta) vira só a 3ª rede de segurança, não a obrigação.

```bash
# exemplo combinado (A + C):
WS_GIT_PUSH=1 WS_GH_PAT=github_pat_xxx \
WS_UPLOAD_CMD='U=$(curl -sF "file=@$WS_RESULTS" https://0x0.st); curl -d "sweep pronto: $U" ntfy.sh/meu-topico' \
  ./scripts/workstation_all.sh
```

### Se o pendrive falhar: clonar e re-rodar
Agora é um fallback **completo** (commitei o `patterns_et_32.txt`, que era o
único arquivo insubstituível):
```bash
git clone https://github.com/MTheus22/parallel-aho-corasick.git && cd parallel-aho-corasick
AUTO_DOWNLOAD=1 ./scripts/prepare_workstation_data.sh   # baixa Snort + Enron (~1,4 GB) e gera enron_x8
chmod +x scripts/*.sh && WS_GIT_PUSH=1 WS_GH_PAT=github_pat_xxx ./scripts/workstation_all.sh
```
Custa o download do Enron (CMU, ~1,4 GB — mais lento que o pendrive, mas funciona
sem ele). `enron_corpus`/`patterns_snort` são rebaixáveis; o `et_32` agora vem no
clone.

---

## 8. Estimativa de tempo

Dominada pelas baselines **sequenciais** da fase A sobre `enron_x8` (10,6 GiB,
warmup 2 + iters 8 = 10 passadas). No laptop i5 isso seria ~6 min (snort) a
~12 min (et_32) por run; o 9950X (Zen 5, DDR5-5600, 2 CCDs) reduz bastante.

| Fase | Conteúdo | Ordem de grandeza |
|---|---|---|
| A | escalonamento `enron_x8`, 60 runs (4 baselines lentas dominam) | ~2–3 h |
| B | footprint `enron_corpus` (8× menor), 24 runs | ~30–60 min |
| D | per-thread `enron_x8`, 4 runs | ~10–20 min |
| E | build-time `enron_corpus`, 24 runs (iter=1) | ~15–30 min |
| S | opcional, 4 runs `enron_x8` | ~10–20 min |
| **Total** | A B D E (+S) | **~3–5 h** |

**Cabe folgado numa noite.** Mantenha **iters=8** na fase A (o ponto da corrida é
robustez/portabilidade; a variância apertada vale o tempo). Não há motivo para
cortar para iters=5 com uma noite inteira disponível.

---

## 9. Pós-coleta — validação (faça ANTES de citar qualquer número)

### 9.1 Sanidade de correção (bloqueante)
```bash
sqlite3 -header -column runs/workstation/sweep.db "SELECT * FROM v_correctness;"
```
**Tem que dar `distinct_match_counts = 1`** por `(patterns, corpus)`. Se algum
for ≠ 1, os searchers discordam no nº de matches → **bug real**, não use os
números até investigar.

### 9.2 Zero falhas
```bash
find runs/workstation -name '*.FAIL'        # tem que vir vazio
grep -E 'FAIL|failed=' runs/workstation/MASTER.log | tail
```
Se houver `.FAIL`, **rerodar o script reexecuta só as que faltaram** (resume).

### 9.3 Se `sweep.db` não nasceu na máquina (python3 ausente — 4.9)
```bash
python3 scripts/extract_sweep_csv.py runs/workstation -o runs/workstation/sweep.csv --known-only
python3 scripts/build_sweep_db.py    runs/workstation/sweep.csv -o runs/workstation/sweep.db
```
(Pode rodar isso no laptop a partir dos `.log` trazidos no pendrive.)

### 9.4 Sanity vs. o laptop (a corrida é de *portabilidade*, não de recorde)
Confronte com `runs/i5/sweep.db` usando as **mesmas views**
(`v_speedup`, `v_self_speedup`, `v_footprint`, `v_build`, `v_best`). Espere:
- speedups iguais ou **maiores** (16 cores físicos homogêneos + DDR5);
- joelho da curva visível em 8→16 (fronteira de CCD) e 16→32 (SMT);
- cross-over de cache da fase B deslocado por causa do L3 por-CCD (32 MiB).
Números **pré-2026-05-29** (7,49× etc.) foram descartados — não os reintroduza.

---

## 10. Checklist Go/No-Go (a lista do "antes de ir embora")

O wrapper faz a maior parte das checagens (e **aborta** se dados/build
falharem). O que **você** confirma com os olhos antes de ir embora:

- [ ] Repo + `data/` estão no **disco local** (não rodou do pendrive). — 4.4
- [ ] O wrapper imprimiu **`Sweep rodando em BACKGROUND (PID ...)`** (logo, o
      pré-flight deu PRONTO, o build passou e o sweep começou). — 4.1/4.2
- [ ] `free -h` (impresso pelo wrapper) mostra **≳ 20–24 GiB livres**. — 4.3
- [ ] Governador `performance` (o wrapper diz `[ok]` ou `[aviso]` — saiba qual). — 4.6
- [ ] (opcional, ~2 min) `tail runs/workstation/MASTER.log`: `OK` acumulando, sem
      `FAIL`; `env/thermal.tsv` sem MHz despencando. — 4.7
- [ ] Você viu o aviso de `make test` (`[ok]` ou `[AVISO]`) — se AVISO, lembre de
      checar `v_correctness` amanhã.

---

## 11. Apêndice — sequência exata, copiável (uma tela)

```bash
# ---------- no LAPTOP ----------
rsync -av --info=progress2 \
  --exclude 'build/' --exclude 'data/enron_x8.txt' \
  --exclude 'data/simplewiki*' --exclude 'data/enron/' --exclude 'data/snort/' \
  --exclude '.git/' \
  /home/matheusbarros/projects/idp/tcc/parallel-aho-corasick/ \
  /media/matheusbarros/2776-BEDC/pac/

# ---------- na WORKSTATION (3 comandos) ----------
cp -a /media/<PENDRIVE>/pac /home/$USER/pac && cd /home/$USER/pac
chmod +x scripts/*.sh                # FAT32 não guarda +x; garante execução
./scripts/workstation_all.sh         # dados→build→test→sweep→empacota, sozinho.
#   (assista ~poucos min até "PODE SAIR / DESCONECTAR", então vá embora.)
#   Sem sudo? o wrapper segue mesmo assim. Pular fase S? PHASES="A B D E" ./scripts/workstation_all.sh

# ---------- de MANHÃ ----------
cd /home/$USER/pac
sqlite3 -header -column runs/workstation/sweep.db "SELECT * FROM v_correctness;"   # =1
find runs/workstation -name '*.FAIL'                                              # vazio
cp -a runs/workstation /media/matheusbarros/2776-BEDC/workstation-results                  # leve embora (ou ~/ws-results.tgz)
```

---

### Referências internas
- Grade e justificativa: `parallel-aho-corasick/docs/sweep-test-inventory.md`,
  `docs/testes-workstation.md`.
- Run + análise (views SQLite): `parallel-aho-corasick/runs/workstation/README.md`.
- Scripts: **`scripts/workstation_all.sh`** (wrapper "um comando"),
  `scripts/run_workstation_sweep.sh`, `scripts/prepare_workstation_data.sh`,
  `scripts/extract_sweep_csv.py`, `scripts/build_sweep_db.py`.
- Fonte de verdade do TCC (laptop): `runs/i5/sweep.db`, `docs/tcc-synthesis.html`.
