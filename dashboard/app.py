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
        st.error("Nenhum banco de dados `sweep.db` encontrado em `runs/*/sweep.db`.")
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
        "Réplicas R",
        "Skew H",
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
    elif page == "Réplicas R":
        render_replicates(all_runs_df, filters)
    elif page == "Skew H":
        render_skew(selected_run_ids, run_dbs, filters)
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

def render_replicates(all_runs_df, filters):
    df_r = ui.apply_filters(all_runs_df, filters)
    df_r = df_r[df_r["phase"] == "R_replicated"] if "phase" in df_r.columns else df_r
    df_r_baseline = ui.apply_filters(
        all_runs_df,
        filters,
        exclude=["searcher", "thr", "show_sequential"],
    )
    df_r_baseline = df_r_baseline[df_r_baseline["phase"] == "R_replicated"] if "phase" in df_r_baseline.columns else df_r_baseline

    if df_r.empty:
        st.warning("Sem dados da fase `R_replicated` para os filtros aplicados.")
        return

    summary = ui.summarize_replicates(
        df_r,
        ["run_id", "patterns", "corpus", "searcher", "thr"],
        value_col="mbps",
        prefix="mbps",
    )
    baseline_summary = ui.summarize_replicates(
        df_r_baseline,
        ["run_id", "patterns", "corpus", "searcher", "thr"],
        value_col="mbps",
        prefix="mbps",
    )
    summary = ui.add_speedup_from_median_seq(summary, baseline_summary=baseline_summary)
    summary = summary.sort_values(by=["run_id", "patterns", "corpus", "thr", "median_mbps"])

    st.markdown(
        "A fase R resume processos independentes por mediana e IQR. "
        "O speedup usa a mediana do `sequential` do mesmo run/cenário como baseline."
    )
    st.plotly_chart(charts.plot_replicated_speedup(summary), width="stretch")
    st.plotly_chart(charts.plot_replicated_throughput(summary), width="stretch")

    st.subheader("Resumo das Réplicas")
    cols = [
        "run_id", "patterns", "corpus", "searcher", "thr", "reps",
        "median_mbps", "q1_mbps", "q3_mbps", "min_mbps", "max_mbps",
        "iqr_pct_mbps", "range_pct_mbps", "median_cv_pct",
        "speedup_vs_seq_median",
    ]
    st.dataframe(summary[[c for c in cols if c in summary.columns]])

def render_skew(selected_run_ids, run_dbs, filters):
    df_h = data.load_runs(selected_run_ids, run_dbs)
    df_h = ui.derived_metrics(df_h)
    df_h = ui.apply_filters(df_h, filters)
    df_h = df_h[df_h["phase"] == "H_skew"] if "phase" in df_h.columns else df_h

    if df_h.empty:
        st.warning("Sem dados da fase `H_skew` para os filtros aplicados.")
        return

    summary = ui.summarize_replicates(
        df_h,
        ["run_id", "patterns", "corpus", "searcher"],
        value_col="mbps",
        prefix="mbps",
    )
    st.markdown(
        "A fase H compara corpora com os mesmos bytes e matches totais, mudando a distribuição espacial "
        "do trabalho: `enron_skew_uniform` contra `enron_skew_clustered`."
    )
    st.plotly_chart(charts.plot_skew_throughput(summary), width="stretch")

    delta = summary.pivot_table(
        index=["run_id", "patterns", "searcher"],
        columns="corpus",
        values="median_mbps",
    ).reset_index()
    delta.columns.name = None
    if {"enron_skew_uniform", "enron_skew_clustered"}.issubset(delta.columns):
        delta = delta.rename(columns={
            "enron_skew_uniform": "uniform_mbps",
            "enron_skew_clustered": "clustered_mbps",
        })
        delta["clustered_vs_uniform_pct"] = (
            (delta["clustered_mbps"] - delta["uniform_mbps"])
            / delta["uniform_mbps"]
            * 100.0
        )
        st.plotly_chart(charts.plot_skew_delta(delta), width="stretch")

        st.subheader("Delta Clustered vs Uniform")
        st.dataframe(delta.sort_values(by=["patterns", "clustered_vs_uniform_pct"], ascending=[True, False]))

    v_balance = data.load_worker_balance(selected_run_ids, run_dbs)
    v_balance = ui.apply_filters(v_balance, filters)
    v_balance = v_balance[v_balance["phase"] == "H_skew"] if "phase" in v_balance.columns else v_balance
    if not v_balance.empty:
        balance_summary = ui.summarize_replicates(
            v_balance,
            ["run_id", "patterns", "corpus", "searcher"],
            value_col="spread_pct",
            prefix="spread_pct",
        )
        imbalance = v_balance.groupby(
            ["run_id", "patterns", "corpus", "searcher"],
            dropna=False,
        )["imbalance_ratio"].median().reset_index(name="median_imbalance_ratio")
        balance_summary = balance_summary.merge(
            imbalance,
            on=["run_id", "patterns", "corpus", "searcher"],
            how="left",
        )
        st.plotly_chart(charts.plot_worker_spread_summary(balance_summary), width="stretch")

        st.subheader("Resumo de Balanceamento por Worker")
        st.dataframe(balance_summary.sort_values(by=["patterns", "corpus", "searcher"]))

    worker_df = data.load_worker_metrics(selected_run_ids, run_dbs)
    worker_df = ui.derived_metrics(worker_df)
    worker_df = ui.apply_filters(worker_df, filters)
    worker_df = worker_df[worker_df["phase"] == "H_skew"] if "phase" in worker_df.columns else worker_df
    if not worker_df.empty and "i5_core_class" in worker_df.columns:
        per_log = worker_df.groupby(
            ["run_id", "patterns", "corpus", "searcher", "tag", "log_file"],
            dropna=False,
        )["milliseconds"].agg(
            min_worker_ms="min",
            mean_worker_ms="mean",
            max_worker_ms="max",
        ).reset_index()
        per_log["barrier_idle_pct"] = (
            (per_log["max_worker_ms"] - per_log["mean_worker_ms"])
            / per_log["max_worker_ms"]
            * 100.0
        )
        barrier_summary = ui.summarize_replicates(
            per_log,
            ["run_id", "patterns", "corpus", "searcher"],
            value_col="barrier_idle_pct",
            prefix="barrier_idle_pct",
        )
        st.plotly_chart(charts.plot_barrier_idle_summary(barrier_summary), width="stretch")
        st.subheader("Resumo de Ociosidade de Barreira")
        st.dataframe(barrier_summary.sort_values(by=["patterns", "corpus", "searcher"]))

        st.subheader("CPU/P-E Amostrado no Fim do Worker")
        cpu_summary = worker_df.groupby(
            ["run_id", "patterns", "corpus", "searcher", "i5_core_class"],
            dropna=False,
        ).agg(
            worker_rows=("worker_id", "count"),
            median_ms=("milliseconds", "median"),
            median_mbps=("mbps", "median"),
            median_matches=("matches_found", "median"),
        ).reset_index()
        st.dataframe(cpu_summary.sort_values(by=["patterns", "corpus", "searcher", "i5_core_class"]))

        idx = worker_df.groupby(["run_id", "log_file"])["milliseconds"].idxmax()
        stragglers = worker_df.loc[idx]
        straggler_summary = stragglers.groupby(
            ["run_id", "patterns", "corpus", "searcher", "i5_core_class"],
            dropna=False,
        ).size().reset_index(name="straggler_logs")
        st.subheader("Classe do Worker Mais Lento")
        st.dataframe(straggler_summary.sort_values(by=["patterns", "corpus", "searcher", "i5_core_class"]))

    st.subheader("Resumo de Throughput")
    st.dataframe(summary.sort_values(by=["patterns", "corpus", "searcher"]))

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
