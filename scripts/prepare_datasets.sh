#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$SCRIPT_DIR/.."
DATA="$ROOT/data"
mkdir -p "$DATA/snort"
mkdir -p "$DATA/enron"

# ---- 1. Snort 3 Community Rules -----------------------------------------
SNORT_URL="https://www.snort.org/downloads/community/snort3-community-rules.tar.gz"
SNORT_TAR="$DATA/snort/snort3-community-rules.tar.gz"
SNORT_RULES_DIR="$DATA/snort/rules"

if [[ ! -s "$SNORT_TAR" ]]; then
  echo "# Downloading Snort 3 Community Rules..."
  wget --show-progress -c -O "$SNORT_TAR" "$SNORT_URL"
fi

if [[ ! -d "$SNORT_RULES_DIR" ]]; then
  echo "# Extracting Snort Rules..."
  mkdir -p "$SNORT_RULES_DIR"
  tar -xzf "$SNORT_TAR" -C "$SNORT_RULES_DIR" --strip-components=1
fi

echo "# Extracting patterns from Snort Rules..."
# Use the existing script
python3 "$SCRIPT_DIR/extract_snort_patterns.py" "$SNORT_RULES_DIR"/snort3-community.rules -o "$DATA/patterns_snort.txt" --stats


# ---- 2. Enron Email Dataset ---------------------------------------------
ENRON_URL="https://www.cs.cmu.edu/~./enron/enron_mail_20150507.tar.gz"
ENRON_TAR="$DATA/enron/enron_mail_20150507.tar.gz"
ENRON_DIR="$DATA/enron/maildir"
ENRON_TXT="$DATA/enron_corpus.txt"

if [[ ! -s "$ENRON_TAR" ]]; then
  echo "# Downloading Enron Email Dataset (~1.7 GB)..."
  wget --show-progress -c -O "$ENRON_TAR" "$ENRON_URL"
fi

if [[ ! -d "$ENRON_DIR" ]]; then
  echo "# Extracting Enron Dataset (this might take a while)..."
  tar -xzf "$ENRON_TAR" -C "$DATA/enron"
fi

if [[ ! -s "$ENRON_TXT" ]]; then
  echo "# Concatenating Enron emails into a single corpus file..."
  # Find all text files and concatenate them
  find "$ENRON_DIR" -type f -print0 | xargs -0 cat > "$ENRON_TXT"
  echo "# Enron corpus created at $ENRON_TXT"
  echo "# Text size: $(du -h "$ENRON_TXT" | cut -f1)"
fi

echo "# Done!"
