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
    
    if "i5" in selected_runs:
        st.sidebar.warning("⚠️ i5 ativo. Use o i5 apenas para evidência P/E, não como ranking global de máquinas.")
        
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
        
    return df
