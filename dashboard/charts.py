import plotly.express as px
import plotly.graph_objects as go
import pandas as pd

def _safe_hover(df: pd.DataFrame, desired: list[str]) -> list[str]:
    return [c for c in desired if c in df.columns]

# Color palette for searchers to keep it consistent across the app
SEARCHER_COLORS = {
    "sequential": "#636EFA",
    "sequential_flat": "#EF553B",
    "pthread_chunked": "#00CC96",
    "pthread_chunked_v2": "#AB63FA",
    "pthread_chunked_v3": "#FFA15A",
    "pthread_dynamic": "#19D3F3",
    "pthread_dynamic_flat": "#FF6692",
    "pthread_chunked_flat": "#B6E880",
    "pthread_chunked_v3_flat": "#FF97FF",
    "pthread_prefetch": "#FECB52",
    "pattern_sharded_prefix": "#1f77b4",
    "pthread_2d_sharded_chunked": "#ff7f0e"
}

def get_color(searcher_name: str) -> str:
    return SEARCHER_COLORS.get(searcher_name, None)

def plot_bar_counts(df: pd.DataFrame, x: str, title: str):
    counts = df[x].value_counts().reset_index()
    counts.columns = [x, 'count']
    fig = px.bar(counts, x=x, y='count', title=title, text_auto=True)
    fig.update_layout(xaxis_title=x.capitalize(), yaxis_title="Contagem")
    return fig

def plot_speedup_curve(df: pd.DataFrame, facet: bool = True):
    if df.empty:
        return go.Figure()
        
    hover_data = _safe_hover(df, ["run_id", "phase", "patterns", "corpus", "mbps", "cv_pct", "matches"])
    
    fig = px.line(
        df,
        x="thr",
        y="speedup_vs_seq",
        color="searcher",
        facet_col="patterns" if facet and len(df["patterns"].unique()) > 1 else None,
        facet_row="corpus" if facet and len(df["corpus"].unique()) > 1 else None,
        markers=True,
        title="Curva de Speedup vs Sequential",
        color_discrete_map=SEARCHER_COLORS,
        hover_data=hover_data
    )
    fig.update_layout(xaxis_title="Threads", yaxis_title="Speedup (vs Sequential)")
    return fig

def plot_throughput_curve(df: pd.DataFrame, facet: bool = True):
    if df.empty:
        return go.Figure()
        
    hover_data = _safe_hover(df, ["run_id", "phase", "patterns", "corpus", "speedup_vs_seq", "cv_pct", "matches"])
    
    fig = px.line(
        df,
        x="thr",
        y="mbps",
        color="searcher",
        facet_col="patterns" if facet and len(df["patterns"].unique()) > 1 else None,
        facet_row="corpus" if facet and len(df["corpus"].unique()) > 1 else None,
        markers=True,
        title="Throughput (MB/s)",
        color_discrete_map=SEARCHER_COLORS,
        hover_data=hover_data
    )
    fig.update_layout(xaxis_title="Threads", yaxis_title="Throughput (MB/s)")
    return fig

def plot_best_heatmap(df: pd.DataFrame):
    if df.empty:
        return go.Figure()
    
    # Create a string label for patterns/corpus
    if "corpus" in df.columns and "patterns" in df.columns:
        df["scenario"] = df["patterns"] + " / " + df["corpus"]
    else:
        df["scenario"] = df.get("patterns", df.get("corpus", "Cenário Único"))
        
    hover_data = _safe_hover(df, ["mbps", "run_id"])
    
    # Scatter as a categorical heatmap (gives precise control over color mapping)
    fig = px.scatter(
        df,
        x="thr",
        y="scenario",
        color="searcher",
        symbol="searcher",
        title="Mapa de Vencedores (Best Searcher per Scenario/Thread)",
        color_discrete_map=SEARCHER_COLORS,
        hover_data=hover_data
    )
    
    # Make markers big to simulate heatmap cells
    fig.update_traces(marker=dict(size=20, symbol="square"))
    fig.update_layout(
        xaxis_title="Threads",
        yaxis_title="Cenário",
        yaxis={'categoryorder':'category descending'}
    )
    
    return fig

def plot_footprint_scatter(df: pd.DataFrame):
    if df.empty:
        return go.Figure()
        
    hover_data = _safe_hover(df, ["run_id", "patterns", "corpus", "mbps", "cv_pct"])
    
    fig = px.scatter(
        df,
        x="automaton_mib",
        y="mbps",
        color="searcher",
        symbol="thr",
        log_x=True,
        title="Impacto do Tamanho do Autômato (Cache Footprint)",
        color_discrete_map=SEARCHER_COLORS,
        hover_data=hover_data
    )
    fig.update_layout(xaxis_title="Tamanho do Autômato (MiB, log)", yaxis_title="Throughput (MB/s)")
    return fig

def plot_cross_corpus_bar(df: pd.DataFrame):
    if df.empty:
        return go.Figure()
        
    fig = px.bar(
        df,
        x="corpus",
        y="mbps",
        color="searcher",
        facet_col="thr",
        barmode="group",
        title="Comparação Cross-Corpus (Throughput)",
        color_discrete_map=SEARCHER_COLORS,
    )
    fig.update_layout(xaxis_title="Corpus", yaxis_title="Throughput (MB/s)")
    return fig

def plot_worker_time_bar(df: pd.DataFrame):
    if df.empty:
        return go.Figure()
        
    fig = px.bar(
        df,
        x="worker_id",
        y="milliseconds",
        color="searcher",
        title="Tempo de Execução por Worker",
        barmode="group",
        color_discrete_map=SEARCHER_COLORS
    )
    fig.update_layout(xaxis_title="Worker ID", yaxis_title="Tempo (ms)")
    return fig

def plot_worker_scatter(df: pd.DataFrame):
    if df.empty:
        return go.Figure()
        
    fig = px.scatter(
        df,
        x="milliseconds",
        y="matches_found",
        size="bytes_scanned",
        color="searcher",
        hover_data=["worker_id", "thr"],
        title="Matches vs Tempo (Tamanho = Bytes Scanned)",
        color_discrete_map=SEARCHER_COLORS
    )
    fig.update_layout(xaxis_title="Tempo (ms)", yaxis_title="Matches Found")
    return fig

def plot_build_curve(df: pd.DataFrame):
    if df.empty:
        return go.Figure()
        
    fig = px.line(
        df,
        x="build_threads",
        y="build_speedup",
        color="patterns",
        markers=True,
        title="Speedup do Build Paralelo"
    )
    # Add horizontal line at 1
    fig.add_hline(y=1.0, line_dash="dash", line_color="red")
    fig.update_layout(xaxis_title="Build Threads", yaxis_title="Speedup (vs Seq)")
    return fig

def plot_granularity_curve(df: pd.DataFrame):
    if df.empty:
        return go.Figure()
        
    fig = px.line(
        df,
        x="tpt",
        y="mbps",
        color="searcher",
        markers=True,
        title="Impacto da Granularidade Dinâmica",
        color_discrete_map=SEARCHER_COLORS
    )
    fig.update_layout(xaxis_title="Tasks per Thread", yaxis_title="Throughput (MB/s)")
    return fig

def plot_replicated_speedup(summary: pd.DataFrame):
    if summary.empty:
        return go.Figure()
    required = {"speedup_vs_seq_median", "q1_speedup_vs_seq", "q3_speedup_vs_seq"}
    if not required.issubset(summary.columns):
        fig = go.Figure()
        fig.update_layout(title="Fase R: baseline sequential indisponível para speedup")
        return fig

    df = summary.copy()
    df["err_plus"] = df["q3_speedup_vs_seq"] - df["speedup_vs_seq_median"]
    df["err_minus"] = df["speedup_vs_seq_median"] - df["q1_speedup_vs_seq"]
    hover_data = _safe_hover(df, [
        "run_id", "patterns", "corpus", "reps", "median_mbps",
        "iqr_pct_mbps", "range_pct_mbps", "median_cv_pct",
    ])

    fig = px.line(
        df,
        x="thr",
        y="speedup_vs_seq_median",
        color="searcher",
        markers=True,
        error_y="err_plus",
        error_y_minus="err_minus",
        title="Fase R: Speedup por Mediana das Réplicas",
        color_discrete_map=SEARCHER_COLORS,
        hover_data=hover_data,
    )
    fig.update_layout(
        xaxis_title="Threads",
        yaxis_title="Speedup vs sequential mediano",
    )
    return fig

def plot_replicated_throughput(summary: pd.DataFrame):
    if summary.empty:
        return go.Figure()

    df = summary.copy()
    df["err_plus"] = df["q3_mbps"] - df["median_mbps"]
    df["err_minus"] = df["median_mbps"] - df["q1_mbps"]
    hover_data = _safe_hover(df, [
        "run_id", "patterns", "corpus", "reps", "speedup_vs_seq_median",
        "iqr_pct_mbps", "range_pct_mbps", "median_cv_pct",
    ])

    fig = px.line(
        df,
        x="thr",
        y="median_mbps",
        color="searcher",
        markers=True,
        error_y="err_plus",
        error_y_minus="err_minus",
        title="Fase R: Throughput Mediano com IQR",
        color_discrete_map=SEARCHER_COLORS,
        hover_data=hover_data,
    )
    fig.update_layout(xaxis_title="Threads", yaxis_title="Mediana MB/s")
    return fig

def plot_skew_throughput(summary: pd.DataFrame):
    if summary.empty:
        return go.Figure()

    df = summary.copy()
    df["err_plus"] = df["q3_mbps"] - df["median_mbps"]
    df["err_minus"] = df["median_mbps"] - df["q1_mbps"]
    fig = px.bar(
        df,
        x="searcher",
        y="median_mbps",
        color="corpus",
        facet_col="patterns" if "patterns" in df.columns and df["patterns"].nunique() > 1 else None,
        barmode="group",
        error_y="err_plus",
        error_y_minus="err_minus",
        title="Fase H: Uniforme vs Clustered (Mediana MB/s)",
        color_discrete_sequence=px.colors.qualitative.Set2,
        hover_data=_safe_hover(df, ["run_id", "reps", "iqr_pct_mbps", "median_cv_pct"]),
    )
    fig.update_layout(xaxis_title="Searcher", yaxis_title="Mediana MB/s")
    return fig

def plot_skew_delta(delta_df: pd.DataFrame):
    if delta_df.empty:
        return go.Figure()

    fig = px.bar(
        delta_df,
        x="searcher",
        y="clustered_vs_uniform_pct",
        color="searcher",
        facet_col="patterns" if "patterns" in delta_df.columns and delta_df["patterns"].nunique() > 1 else None,
        title="Fase H: Delta Clustered vs Uniform",
        color_discrete_map=SEARCHER_COLORS,
        hover_data=_safe_hover(delta_df, ["run_id", "uniform_mbps", "clustered_mbps"]),
    )
    fig.add_hline(y=0.0, line_dash="dash", line_color="gray")
    fig.update_layout(xaxis_title="Searcher", yaxis_title="Delta de throughput (%)")
    return fig

def plot_worker_spread_summary(summary: pd.DataFrame):
    if summary.empty:
        return go.Figure()

    df = summary.copy()
    df["err_plus"] = df["q3_spread_pct"] - df["median_spread_pct"]
    df["err_minus"] = df["median_spread_pct"] - df["q1_spread_pct"]
    fig = px.bar(
        df,
        x="searcher",
        y="median_spread_pct",
        color="corpus",
        facet_col="patterns" if "patterns" in df.columns and df["patterns"].nunique() > 1 else None,
        barmode="group",
        error_y="err_plus",
        error_y_minus="err_minus",
        title="Fase H: Spread de Tempo por Worker",
        color_discrete_sequence=px.colors.qualitative.Set2,
        hover_data=_safe_hover(df, ["run_id", "reps", "median_imbalance_ratio"]),
    )
    fig.update_layout(xaxis_title="Searcher", yaxis_title="Spread mediano (%)")
    return fig

def plot_barrier_idle_summary(summary: pd.DataFrame):
    if summary.empty:
        return go.Figure()

    df = summary.copy()
    df["err_plus"] = df["q3_barrier_idle_pct"] - df["median_barrier_idle_pct"]
    df["err_minus"] = df["median_barrier_idle_pct"] - df["q1_barrier_idle_pct"]
    fig = px.bar(
        df,
        x="searcher",
        y="median_barrier_idle_pct",
        color="corpus",
        facet_col="patterns" if "patterns" in df.columns and df["patterns"].nunique() > 1 else None,
        barmode="group",
        error_y="err_plus",
        error_y_minus="err_minus",
        title="Fase H: Ociosidade de Barreira",
        color_discrete_sequence=px.colors.qualitative.Set2,
        hover_data=_safe_hover(df, ["run_id", "reps", "iqr_pct_barrier_idle_pct"]),
    )
    fig.update_layout(xaxis_title="Searcher", yaxis_title="Ociosidade mediana (%)")
    return fig
