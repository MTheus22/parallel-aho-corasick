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
    """Discover sweep.db files under runs/ with canonical runs first."""
    runs_root = root / "runs"
    known_labels = {
        "workstation_2026-06-30": "Workstation 2026-06-30 (canonical)",
        "i5": "i5 P/E historical",
        "i5_2026-07-02": "i5 2026-07-02 R/H pilot",
    }
    priority = {
        "workstation_2026-06-30": 0,
        "i5": 1,
        "i5_2026-07-02": 2,
    }

    dbs = []
    if not runs_root.exists():
        return dbs

    discovered = []
    for db_path in runs_root.glob("*/sweep.db"):
        run_id = db_path.parent.name
        discovered.append((priority.get(run_id, 100), run_id, db_path))

    for _, run_id, db_path in sorted(discovered):
        label = known_labels.get(run_id, run_id.replace("_", " "))
        dbs.append(RunDb(run_id, str(db_path), label))

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
