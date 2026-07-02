import streamlit as st
import pandas as pd
from typing import Callable

def sidebar_filters(df: pd.DataFrame):
    """Render the sidebar filters and return the selected values."""
    st.sidebar.header("Filtros Globais")
    
    # Defaults
    all_runs = df["run_id"].unique().tolist() if "run_id" in df.columns else []
    
    # Run ID Filter
    selected_runs = st.sidebar.multiselect(
        "Run ID",
        options=all_runs,
        default=["workstation_2026-06-30"] if "workstation_2026-06-30" in all_runs else all_runs
    )
    
    if any(str(run_id).startswith("i5") for run_id in selected_runs):
        st.sidebar.warning("⚠️ i5 ativo. Use runs i5 como evidência P/E/piloto, não como ranking global de máquinas.")
        
    # Other filters depend on the data we have, but to avoid circular filtering, 
    # we filter sequentially based on previous selections (optional) or just provide independent filters
    
    # Create a filtered df to drive the next dropdowns based on run selection
    df_runs = df[df["run_id"].isin(selected_runs)] if selected_runs else df
    
    phases = sorted(df_runs["phase"].dropna().unique().tolist()) if "phase" in df_runs.columns else []
    selected_phases = st.sidebar.multiselect("Phase", options=phases)
    
    patterns = sorted(df_runs["patterns"].dropna().unique().tolist()) if "patterns" in df_runs.columns else []
    selected_patterns = st.sidebar.multiselect("Patterns", options=patterns)
    
    corpora = sorted(df_runs["corpus"].dropna().unique().tolist()) if "corpus" in df_runs.columns else []
    selected_corpora = st.sidebar.multiselect("Corpus", options=corpora)
    
    searchers = sorted(df_runs["searcher"].dropna().unique().tolist()) if "searcher" in df_runs.columns else []
    selected_searchers = st.sidebar.multiselect("Searcher", options=searchers)
    
    if "thr" in df_runs.columns:
        thr_vals = sorted(df_runs["thr"].dropna().unique().tolist())
        selected_thr = st.sidebar.multiselect("Threads", options=thr_vals)
    else:
        selected_thr = []
        
    show_sequential = st.sidebar.checkbox("Exibir sequential / sequential_flat", value=True)
    normalize_sequential = st.sidebar.checkbox("Normalizar por sequential", value=False)
    
    return {
        "run_id": selected_runs,
        "phase": selected_phases,
        "patterns": selected_patterns,
        "corpus": selected_corpora,
        "searcher": selected_searchers,
        "thr": selected_thr,
        "show_sequential": show_sequential,
        "normalize_sequential": normalize_sequential
    }

def apply_filters(df: pd.DataFrame, filters: dict, exclude: list = None) -> pd.DataFrame:
    """Apply the dictionary of filters to a dataframe."""
    if df.empty:
        return df
        
    exclude = exclude or []
    mask = pd.Series(True, index=df.index)
    
    if "run_id" not in exclude and filters["run_id"] and "run_id" in df.columns:
        mask &= df["run_id"].isin(filters["run_id"])
        
    if "phase" not in exclude and filters["phase"] and "phase" in df.columns:
        mask &= df["phase"].isin(filters["phase"])
        
    if "patterns" not in exclude and filters["patterns"] and "patterns" in df.columns:
        mask &= df["patterns"].isin(filters["patterns"])
        
    if "corpus" not in exclude and filters["corpus"] and "corpus" in df.columns:
        mask &= df["corpus"].isin(filters["corpus"])
        
    if "searcher" not in exclude and filters["searcher"] and "searcher" in df.columns:
        mask &= df["searcher"].isin(filters["searcher"])
        
    if "thr" not in exclude and filters["thr"] and "thr" in df.columns:
        mask &= df["thr"].isin(filters["thr"])
        
    if "show_sequential" not in exclude and not filters.get("show_sequential", True) and "searcher" in df.columns:
        mask &= ~df["searcher"].isin(["sequential", "sequential_flat"])
        
    return df[mask]

def derived_metrics(df: pd.DataFrame) -> pd.DataFrame:
    """Add derived metrics to the dataframe if base columns exist."""
    df = df.copy()
    if "speedup_vs_seq" in df.columns and "thr" in df.columns:
        # Avoid division by zero
        df["efficiency_vs_seq"] = df.apply(
            lambda row: row["speedup_vs_seq"] / row["thr"] if row["thr"] > 0 else (1.0 if row["searcher"] == "sequential" else 0),
            axis=1
        )
    if "automaton_kib" in df.columns and "automaton_mib" not in df.columns:
        df["automaton_mib"] = df["automaton_kib"] / 1024.0
        
    if "tag" in df.columns:
        # Extract tpt for G_granularity
        import re
        def extract_tpt(tag):
            if pd.isna(tag): return None
            m = re.search(r'tpt(\d+)', tag)
            return int(m.group(1)) if m else None
        df["tpt"] = df["tag"].apply(extract_tpt)

        def extract_rep(tag):
            if pd.isna(tag): return None
            m = re.search(r'rep(\d+)', tag)
            return int(m.group(1)) if m else None
        df["rep"] = df["tag"].apply(extract_rep)

    if "cpu" in df.columns:
        def core_class(cpu):
            if pd.isna(cpu):
                return None
            cpu = int(cpu)
            return "P" if 0 <= cpu <= 3 else "E"
        df["i5_core_class"] = df["cpu"].apply(core_class)
        
    return df

def summarize_replicates(df: pd.DataFrame, group_cols: list[str], value_col: str = "mbps", prefix: str | None = None) -> pd.DataFrame:
    """Summarize independent process replicas with median, IQR, and min/max."""
    if df.empty or value_col not in df.columns:
        return pd.DataFrame()

    prefix = prefix or value_col
    grouped = df.groupby(group_cols, dropna=False)
    summary = grouped.agg(
        reps=(value_col, "count"),
        **{
            f"median_{prefix}": (value_col, "median"),
            f"q1_{prefix}": (value_col, lambda s: s.quantile(0.25)),
            f"q3_{prefix}": (value_col, lambda s: s.quantile(0.75)),
            f"min_{prefix}": (value_col, "min"),
            f"max_{prefix}": (value_col, "max"),
        }
    ).reset_index()

    summary[f"iqr_{prefix}"] = summary[f"q3_{prefix}"] - summary[f"q1_{prefix}"]
    summary[f"iqr_pct_{prefix}"] = summary[f"iqr_{prefix}"] / summary[f"median_{prefix}"] * 100.0
    summary[f"range_pct_{prefix}"] = (
        (summary[f"max_{prefix}"] - summary[f"min_{prefix}"])
        / summary[f"median_{prefix}"]
        * 100.0
    )

    if value_col != "cv_pct" and "cv_pct" in df.columns:
        cv = grouped["cv_pct"].agg(
            median_cv_pct="median",
            max_cv_pct="max",
        ).reset_index()
        summary = summary.merge(cv, on=group_cols, how="left")

    return summary

def add_speedup_from_median_seq(summary: pd.DataFrame, baseline_summary: pd.DataFrame | None = None) -> pd.DataFrame:
    """Add speedup using the median sequential row for each run/pattern/corpus."""
    if summary.empty or "median_mbps" not in summary.columns:
        return summary

    source = baseline_summary if baseline_summary is not None else summary
    keys = [c for c in ["run_id", "patterns", "corpus"] if c in summary.columns]
    seq = source[
        (source["searcher"] == "sequential")
        & (source["thr"] == 1)
    ][keys + ["median_mbps"]].rename(columns={"median_mbps": "seq_median_mbps"})

    if seq.empty:
        return summary

    out = summary.merge(seq, on=keys, how="left")
    out["speedup_vs_seq_median"] = out["median_mbps"] / out["seq_median_mbps"]
    if "q1_mbps" in out.columns and "q3_mbps" in out.columns:
        out["q1_speedup_vs_seq"] = out["q1_mbps"] / out["seq_median_mbps"]
        out["q3_speedup_vs_seq"] = out["q3_mbps"] / out["seq_median_mbps"]
    return out
