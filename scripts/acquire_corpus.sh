#!/usr/bin/env bash
# Download and prepare the Simple English Wikipedia as a benchmark corpus.
#
# Output files (in data/):
#   simplewiki.xml.bz2   - raw dump (kept for reproducibility)
#   simplewiki_raw.xml   - decompressed XML
#   simplewiki.txt       - plain text (XML tags stripped)
#
# Usage:
#   ./scripts/acquire_corpus.sh               # default: Simple EN Wikipedia
#   SKIP_DOWNLOAD=1 ./scripts/acquire_corpus.sh   # reprocess existing dump
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$SCRIPT_DIR/.."
DATA="$ROOT/data"
mkdir -p "$DATA"

DUMP_URL="https://dumps.wikimedia.org/simplewiki/latest/simplewiki-latest-pages-articles.xml.bz2"
DUMP_BZ2="$DATA/simplewiki.xml.bz2"
DUMP_XML="$DATA/simplewiki_raw.xml"
DUMP_TXT="$DATA/simplewiki.txt"

# ---- 1. Download -------------------------------------------------------
if [[ "${SKIP_DOWNLOAD:-0}" == "1" && -s "$DUMP_BZ2" ]]; then
  echo "# Skipping download (SKIP_DOWNLOAD=1)"
else
  echo "# Downloading Simple English Wikipedia dump (~200 MB)..."
  wget --show-progress -c -O "$DUMP_BZ2" "$DUMP_URL"
fi
echo "# Compressed size: $(du -h "$DUMP_BZ2" | cut -f1)"

# ---- 2. Decompress -----------------------------------------------------
if [[ ! -s "$DUMP_XML" ]]; then
  echo "# Decompressing..."
  bunzip2 --keep --stdout "$DUMP_BZ2" > "$DUMP_XML"
fi
echo "# XML size: $(du -h "$DUMP_XML" | cut -f1)"

# ---- 3. Strip XML tags -> plain text -----------------------------------
echo "# Extracting plain text from XML..."
python3 - "$DUMP_XML" "$DUMP_TXT" <<'PYEOF'
import sys, re

src, dst = sys.argv[1], sys.argv[2]
tag_re    = re.compile(r'<[^>]+>')
ref_re    = re.compile(r'&[a-zA-Z]+;|&#\d+;')
blank_re  = re.compile(r'\n{3,}')
written   = 0

with open(src, 'r', encoding='utf-8', errors='replace') as fin, \
     open(dst, 'w', encoding='utf-8') as fout:
    in_text = False
    buf = []
    for line in fin:
        if '<text ' in line or line.strip() == '<text>':
            in_text = True
            # grab content on same line after the tag
            m = re.search(r'<text[^>]*>(.*)', line)
            if m:
                buf.append(m.group(1))
        elif '</text>' in line:
            m = re.search(r'^(.*)</text>', line)
            if m:
                buf.append(m.group(1))
            block = ' '.join(buf)
            block = tag_re.sub(' ', block)
            block = ref_re.sub(' ', block)
            block = blank_re.sub('\n\n', block).strip()
            if len(block) > 20:
                fout.write(block)
                fout.write('\n\n')
                written += len(block) + 2
            buf = []
            in_text = False
        elif in_text:
            buf.append(line.rstrip())

print(f'# Plain text: {written / (1<<20):.1f} MiB written to {dst}')
PYEOF

echo "# Text size: $(du -h "$DUMP_TXT" | cut -f1)"

echo
echo "# Done. Files in $DATA/:"
ls -lh "$DATA/"
echo
echo "# Quick benchmark:"
echo "#   ./build/aclab --patterns $DATA/patterns_snort.txt \\"
echo "#                 --input    $DATA/simplewiki.txt      \\"
echo "#                 --searcher pthread_chunked_v3 --threads $(nproc) \\"
echo "#                 --warmup 1 --iters 3"
