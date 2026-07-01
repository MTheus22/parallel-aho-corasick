import sqlite3
import pandas as pd
from pathlib import Path
import streamlit as st

class RunDb:
    def __init__(self, run_id: str, db_path: str, label: str):
        self.run_id = run_id
        self.db_path = db_path
        self.label = label

def discover_databases(root: Path) -> list[RunDb]:
    """Discover known sweep.db files according to the plan."""
    dbs = []
    
    ws_db = root / "runs" / "workstation_2026-06-30" / "sweep.db"
    if ws_db.exists():
        dbs.append(RunDb("workstation_2026-06-30", str(ws_db), "Workstation 2026-06-30"))
        
    i5_db = root / "runs" / "i5" / "sweep.db"
    if i5_db.exists():
        dbs.append(RunDb("i5", str(i5_db), "i5 P/E"))
        
    return dbs

@st.cache_data
def load_table(db_path: str, table_or_view: str) -> pd.DataFrame:
    """Load a single table or view from a specific database using read-only mode."""
    uri = f"file:{db_path}?mode=ro"
    try:
        with sqlite3.connect(uri, uri=True) as conn:
            return pd.read_sql_query(f"SELECT * FROM {table_or_view}", conn)
    except sqlite3.Error as e:
        st.warning(f"Failed to load {table_or_view} from {db_path}: {e}")
        return pd.DataFrame()

def load_view(selected_run_ids: list[str], run_dbs: list[RunDb], view_name: str) -> pd.DataFrame:
    """Load a view from selected runs and concatenate them with run_id."""
    dfs = []
    for run in run_dbs:
        if run.run_id in selected_run_ids:
            df = load_table(run.db_path, view_name)
            if not df.empty:
                df["run_id"] = run.run_id
                dfs.append(df)
    
    if dfs:
        return pd.concat(dfs, ignore_index=True)
    return pd.DataFrame()

def load_runs(selected_run_ids: list[str], run_dbs: list[RunDb]) -> pd.DataFrame:
    return load_view(selected_run_ids, run_dbs, "runs")

def load_worker_metrics(selected_run_ids: list[str], run_dbs: list[RunDb]) -> pd.DataFrame:
    return load_view(selected_run_ids, run_dbs, "worker_metrics")

def load_worker_balance(selected_run_ids: list[str], run_dbs: list[RunDb]) -> pd.DataFrame:
    return load_view(selected_run_ids, run_dbs, "v_worker_balance")

def get_filter_options(df: pd.DataFrame) -> dict:
    """Extract unique values for filters."""
    options = {}
    if df.empty:
        return options
        
    for col in ["phase", "patterns", "corpus", "searcher"]:
        if col in df.columns:
            options[col] = sorted(df[col].dropna().unique().tolist())
            
    if "thr" in df.columns:
        options["thr"] = sorted(df["thr"].dropna().unique().tolist())
        
    return options
