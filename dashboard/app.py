import streamlit as st
from pathlib import Path
import os
import sys

# Add project root to path for local imports
sys.path.append(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from dashboard import data
from dashboard import ui
from dashboard import charts

# Configuração da página - deve ser o primeiro comando Streamlit
st.set_page_config(page_title="Aho-Corasick Benchmark Dashboard", layout="wide")

def main():
    root = Path(__file__).parent.parent
    
    # 1. Discover Databases
    run_dbs = data.discover_databases(root)
    if not run_dbs:
        st.error("Nenhum banco de dados `sweep.db` encontrado nas pastas `runs/workstation_2026-06-30` ou `runs/i5`.")
        return
        
    # Carrega tabela 'runs' sem filtros para popular as opções da sidebar
    # Como runs pode ser grande, carregamos só o básico para os filtros.
    all_runs_df = data.load_runs([r.run_id for r in run_dbs], run_dbs)
    all_runs_df = ui.derived_metrics(all_runs_df)
    
    # 2. Render Sidebar
    filters = ui.sidebar_filters(all_runs_df)
    selected_run_ids = filters["run_id"]
    
    if not selected_run_ids:
        st.warning("Selecione pelo menos um Run ID na barra lateral.")
        return
        
    # Navegação de Páginas usando sidebar radio ou st.navigation se Streamlit >= 1.36
    pages = [
        "Visão Geral",
        "Curvas de Speedup",
        "Ranking e Vencedores",
        "Footprint e Cache Cliff",
        "Cross-Corpus",
        "Workers e Balanceamento",
        "Build Paralelo",
        "Granularidade Dinâmica",
        "Comparação Entre Runs"
    ]
    
    page = st.sidebar.radio("Navegação", pages)
    
    # Aplica filtros globais base ao dataframe principal
    filtered_runs = ui.apply_filters(all_runs_df, filters)
    
    st.title(f"Aho-Corasick Benchmark: {page}")
    
    # Renderizar a página selecionada
    if page == "Visão Geral":
        render_overview(all_runs_df, selected_run_ids, run_dbs)
    elif page == "Curvas de Speedup":
        render_speedup(filtered_runs, selected_run_ids, run_dbs, filters)
    elif page == "Ranking e Vencedores":
        render_ranking(filtered_runs, selected_run_ids, run_dbs, filters)
    elif page == "Footprint e Cache Cliff":
        render_footprint(filtered_runs, selected_run_ids, run_dbs, filters)
    elif page == "Cross-Corpus":
        render_cross_corpus(filtered_runs, selected_run_ids, run_dbs, filters)
    elif page == "Workers e Balanceamento":
        render_workers(selected_run_ids, run_dbs, filters)
    elif page == "Build Paralelo":
        render_build(selected_run_ids, run_dbs, filters)
    elif page == "Granularidade Dinâmica":
        render_granularity(filtered_runs, selected_run_ids, run_dbs, filters)
    elif page == "Comparação Entre Runs":
        render_comparison(all_runs_df, filters)

def render_overview(df, selected_run_ids, run_dbs):
    st.markdown("""
    ### Bem-vindo ao Painel dos Sweeps
    Este painel lê diretamente os arquivos `sweep.db` preservados.
    
    **Nota:** `workstation_2026-06-30` é a fonte canônica (TCC). `i5` é mantido apenas como evidência histórico/arquitetura P/E.
    """)
    
    col1, col2, col3 = st.columns(3)
    col1.metric("Runs Carregados", len(selected_run_ids))
    col2.metric("Total de Linhas (Runs)", len(df[df["run_id"].isin(selected_run_ids)]))
    
    df_sel = df[df["run_id"].isin(selected_run_ids)]
    if not df_sel.empty:
        col3.metric("Searchers Distintos", df_sel["searcher"].nunique())
        
        st.subheader("Contagem de Corridas por Fase")
        fig = charts.plot_bar_counts(df_sel, "phase", "Fases Executadas")
        st.plotly_chart(fig, width="stretch")
        
        st.subheader("Sanidade (Correctness)")
        correctness_df = data.load_view(selected_run_ids, run_dbs, "v_correctness")
        if not correctness_df.empty:
            st.dataframe(correctness_df)
        else:
            st.info("View `v_correctness` vazia ou indisponível.")

def render_speedup(df, selected_run_ids, run_dbs, filters):
    if df.empty:
        st.warning("Nenhum dado corresponde aos filtros atuais.")
        return
        
    # We want v_speedup for exact speedup numbers against sequential
    v_speedup = data.load_view(selected_run_ids, run_dbs, "v_speedup")
    v_speedup = ui.derived_metrics(v_speedup)
    v_speedup_filtered = ui.apply_filters(v_speedup, filters)
    
    if not v_speedup_filtered.empty:
        st.plotly_chart(charts.plot_speedup_curve(v_speedup_filtered), width="stretch")
    
    st.plotly_chart(charts.plot_throughput_curve(df), width="stretch")
    
    st.subheader("Tabela de Dados")
    st.dataframe(df[["run_id", "phase", "patterns", "corpus", "searcher", "thr", "mbps", "cv_pct", "matches"]].sort_values(by=["patterns", "corpus", "thr", "mbps"], ascending=[True, True, True, False]))

def render_ranking(df, selected_run_ids, run_dbs, filters):
    v_best = data.load_view(selected_run_ids, run_dbs, "v_best")
    v_best = ui.apply_filters(v_best, filters, exclude=["searcher"]) # don't filter searcher for determining best
    
    if v_best.empty:
        st.warning("Nenhum dado de vencedores para os filtros atuais.")
        return
        
    st.plotly_chart(charts.plot_best_heatmap(v_best), width="stretch")
    
    st.subheader("Top Resultados por Cenário/Thread (Rankeado por Vazão)")
    if not df.empty:
        df_rank = df.sort_values(by=["patterns", "corpus", "thr", "mbps"], ascending=[True, True, True, False])
        st.dataframe(df_rank[["run_id", "patterns", "corpus", "thr", "searcher", "mbps", "cv_pct"]])

def render_footprint(df, selected_run_ids, run_dbs, filters):
    v_footprint = data.load_view(selected_run_ids, run_dbs, "v_footprint")
    v_footprint = ui.derived_metrics(v_footprint)
    v_footprint = ui.apply_filters(v_footprint, filters)
    
    if v_footprint.empty:
        # Fallback to runs table
        v_footprint = df
        
    if v_footprint.empty:
        st.warning("Nenhum dado corresponde aos filtros atuais.")
        return
        
    st.plotly_chart(charts.plot_footprint_scatter(v_footprint), width="stretch")

def render_cross_corpus(df, selected_run_ids, run_dbs, filters):
    df_c = df[df["phase"].isin(["C_cross_corpus", "A_baseline_seq", "A_warmup"]) | df["phase"].str.startswith("A_")]
    if df_c.empty:
        st.warning("Nenhuma fase cross_corpus encontrada com os filtros atuais.")
        return
        
    st.plotly_chart(charts.plot_cross_corpus_bar(df_c), width="stretch")
    
    # Calculate percentage differences (simple pivot)
    st.subheader("Comparação Percentual")
    pivot = df_c.pivot_table(index=["run_id", "patterns", "searcher", "thr"], columns="corpus", values="mbps").reset_index()
    st.dataframe(pivot)

def render_workers(selected_run_ids, run_dbs, filters):
    st.markdown("⚠️ Esta página foca na fase `D_per_thread` (ou fases específicas com métricas por worker registradas).")
    worker_df = data.load_worker_metrics(selected_run_ids, run_dbs)
    worker_df = ui.apply_filters(worker_df, filters)
    
    if worker_df.empty:
        st.warning("Sem dados detalhados de `worker_metrics` para os filtros aplicados.")
    else:
        st.plotly_chart(charts.plot_worker_time_bar(worker_df), width="stretch")
        st.plotly_chart(charts.plot_worker_scatter(worker_df), width="stretch")
        
    v_balance = data.load_worker_balance(selected_run_ids, run_dbs)
    v_balance = ui.apply_filters(v_balance, filters)
    if not v_balance.empty:
        st.subheader("Balanceamento (Resumo)")
        st.dataframe(v_balance)

def render_build(selected_run_ids, run_dbs, filters):
    v_build = data.load_view(selected_run_ids, run_dbs, "v_build")
    v_build = ui.apply_filters(v_build, filters)
    
    if v_build.empty:
        st.warning("Nenhum dado de `v_build` para os filtros aplicados.")
        return
        
    st.plotly_chart(charts.plot_build_curve(v_build), width="stretch")
    st.dataframe(v_build)

def render_granularity(df, selected_run_ids, run_dbs, filters):
    df_g = df[df["phase"] == "G_granularity"]
    if df_g.empty:
        st.warning("Sem dados da fase `G_granularity` para os filtros aplicados.")
        return
        
    # Ensure sorted by tpt
    df_g = df_g.sort_values(by="tpt")
    
    st.plotly_chart(charts.plot_granularity_curve(df_g), width="stretch")
    st.dataframe(df_g[["run_id", "searcher", "tpt", "mbps", "cv_pct", "tag"]])

def render_comparison(all_runs_df, filters):
    if len(filters["run_id"]) < 2:
        st.info("Para comparar runs, selecione mais de um Run ID na barra lateral.")
        return
        
    df_comp = ui.apply_filters(all_runs_df, filters)
    if df_comp.empty:
        st.warning("Nenhum dado corresponde aos filtros atuais.")
        return
        
    # Line chart comparing the same searcher across different runs
    fig = charts.plot_throughput_curve(df_comp, facet=True)
    # We want to distinguish run_id by line dash or similar, but for simplicity we facet by run_id
    import plotly.express as px
    fig_comp = px.line(
        df_comp,
        x="thr",
        y="mbps",
        color="searcher",
        line_dash="run_id",
        markers=True,
        title="Comparação de Throughput (Runs)",
        color_discrete_map=charts.SEARCHER_COLORS
    )
    st.plotly_chart(fig_comp, width="stretch")
    
    # Pivot best mbps by run
    pivot = df_comp.pivot_table(index=["patterns", "corpus", "searcher", "thr"], columns="run_id", values="mbps").reset_index()
    st.subheader("Lado a Lado (MB/s)")
    st.dataframe(pivot)

if __name__ == "__main__":
    main()
