#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "${BASH_SOURCE[0]}")/.."

if [[ ! -x "build/aclab" ]]; then
    echo "Compilando aclab..."
    make -B
fi

echo "Executando gerador de corpus skew (Epic 04)..."
exec python3 scripts/make_skewed_corpus.py
