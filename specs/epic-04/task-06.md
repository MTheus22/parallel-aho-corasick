# Task 06 — Complementar o texto do TCC (LaTeX) com o resultado do skew

## Objetivo

Levar o achado do corpus skew ao texto final do TCC: uma subseção de Resultados
com a tabela spread estático × dinâmico, o enquadramento metodológico do
invariante de controle, e as citações (FHBM, DEFCON17, Ródenas) — **usando keys
que já existem** em `referencias.bib`.

## Escopo

- **In scope:** `acceleration-of-pattern-matching-algorithms-using-parallel-programming/partes/results.tex`
  e `.../partes/conclusion.tex`; `.../referencias.bib` (só **conferir/usar**
  keys; adicionar entrada nova só se o paper já estiver em `articles/` e faltar
  a key). É repo git (LaTeX) — commitar só se o usuário pedir.
- **Out of scope:** reescrever seções não relacionadas; inventar keys BibTeX;
  incluir pcap/tráfego real; mudar números canônicos de A–G.

## Implementação

1. **Localize as keys** em `.../referencias.bib` para:
   - FHBM (Lee & Yang 2017, *Algorithms* 10(2):58) — método de injeção de
     densidade.
   - DEFCON17 benchmarks (Aldwairi, Alshboul & Seyam 2018, ICIT) — alvo ~40–45%.
   - Ródenas Picó — moldura "maior fatia limita o makespan".
   ⚠️ **Confirme** que a key do "Ródenas" aponta para a fonte certa: o PDF em
   `articles/` é a **tese de graph matching** dele, não um paper de AC. Cite como
   *moldura de balanceamento transferível* (NPB-MZ), **não** como precedente de
   Aho–Corasick. Se a key não existir, não invente — sinalize ao usuário.
2. **Subseção em `results.tex`** ("Carga desigual e o papel do dispatch
   dinâmico"):
   - Motivação: em CPU homogênea + corpus uniforme o dinâmico empata/perde
     (citar os números A: `chunked_flat` 18,95× vs `dynamic` 17,98× em
     ET-32+Enron T=32).
   - Construção: par uniforme×clustered com **bytes e matches idênticos**
     (invariante de controle \cite{rodenas}), blocos quentes por injeção FHBM
     \cite{fhbm} a densidade DEFCON *ugly* \cite{defcon}.
   - Tabela: `mbps_med`, `spread_med [IQR]`, `barrier_idle_med [IQR]` por searcher
     × corpus (da Task 03).
   - Leitura: no clustered o spread estático ≫ dinâmico e o dinâmico recupera
     vazão; no uniform empatam → o ganho do dinâmico é **condicional à carga
     desigual**.
3. **`conclusion.tex`:** uma frase fechando que a família dinâmica se justifica
   **sob carga espacialmente desigual**, não no regime uniforme — resultado
   condicional honesto, não overclaim. Se a Task 03 deu negativo, escrever o
   negativo (dinâmico não compensa nem sob skew por causa da emissão flat barata)
   e o que isso implica.
4. **Consistência:** os números batem com `runs/<dir>/H_skew/skew_analysis.md` e
   com `results.md` (Task 05). Sem termos internos do repo no texto (searcher →
   descrição algorítmica, como já feito na tese).

## Validação

```bash
cd acceleration-of-pattern-matching-algorithms-using-parallel-programming
xelatex -interaction=nonstopmode main.tex >/dev/null && echo BUILD_OK
grep -n "skew\|desigual\|makespan" partes/results.tex partes/conclusion.tex
# checar que toda \cite nova resolve (sem '??' no PDF):
grep -c '??' main.log || true
```

## Critérios de Aceite

- `results.tex` tem a subseção do skew com a tabela e as três citações
  resolvendo (sem `??` no PDF).
- `conclusion.tex` fecha o resultado condicional (ou o negativo, se for o caso).
- Nenhuma key BibTeX inventada; a do "Ródenas" enquadrada corretamente (não como
  precedente de AC).
- `xelatex` compila sem erro; números idênticos aos da Task 03/05.
- Commit apenas se o usuário autorizar.
