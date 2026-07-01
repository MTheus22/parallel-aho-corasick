# Painel Streamlit dos Sweeps

Este é um painel interativo construído com Streamlit para análise visual dos benchmarks Aho-Corasick gerados nos sweeps.

## Escopo e Limitações

- O painel é estritamente **read-only**. Ele consome os bancos de dados SQLite preservados em `runs/`.
- Ele **não** gera novos dados, nem subscreve os resultados.
- Foca em leitura visual (curvas de speedup, heatmaps de vencedores, balanceamento de workers, etc.).

## Instalação

Crie um ambiente virtual (recomendado) e instale as dependências:

```bash
python3 -m venv .venv
source .venv/bin/activate
pip install -r requirements-dashboard.txt
```

## Execução

Inicie o painel a partir da raiz do repositório:

```bash
streamlit run dashboard/app.py
```

O aplicativo abrirá no seu navegador, usualmente em `http://localhost:8501`.

## Entendendo os Dados

- **Workstation (`workstation_2026-06-30`)**: A fonte canônica (TCC). Contém as análises principais (fases A a G).
- **i5 (`i5`)**: Fonte usada exclusivamente para demonstrar o comportamento da arquitetura P/E (Performance/Efficiency cores).

Recomendamos usar a barra lateral para filtrar por **Run**, **Fase**, **Searcher**, etc., para não sobrecarregar as visualizações.
