#!/usr/bin/env bash
# Download and prepare the Simple English Wikipedia as a benchmark corpus.
#
# Output files (in data/):
#   simplewiki.xml.bz2   - raw dump (kept for reproducibility)
#   simplewiki_raw.xml   - decompressed XML
#   simplewiki.txt       - plain text (XML tags stripped)
#   patterns_en.txt      - realistic English pattern dictionary
#   patterns_code.txt    - patterns mimicking security / log scanning
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

# ---- 4. Pattern dictionaries -------------------------------------------

# English: mix of common words (high hit-rate) and rare ones (low hit-rate)
cat > "$DATA/patterns_en.txt" <<'EOF'
# High-frequency English words (dense matches expected)
the
of
and
to
a
in
that
is
was
for
on
as
with
his
they
be
at
one
have
this
from
or
by
an
which
not
but
what
all
were
we
when
your
can
there
use
an
each
she
do
how
their
if
will
up
other
about
out
many
then
them
these
so
some
her
would
make
like
into
him
time
has
look
two
more
write
go
see
number
no
way
could
people
my
than
first
water
been
called
who
its
now
find
long
down
day
did
get
come
made
may
part
EOF

# Code/security: patterns useful for log analysis and intrusion detection
cat > "$DATA/patterns_code.txt" <<'EOF'
# Security and log-analysis patterns
error
ERROR
FATAL
warning
WARNING
failed
denied
rejected
unauthorized
forbidden
exception
traceback
segfault
null pointer
buffer overflow
stack overflow
out of memory
timeout
connection refused
permission denied
authentication failed
login failed
invalid password
SQL syntax
SELECT *
DROP TABLE
INSERT INTO
<script>
javascript:
eval(
document.cookie
alert(
onerror=
onload=
../../
/etc/passwd
/etc/shadow
cmd.exe
powershell
wget http
curl http
base64_decode
EOF

echo
echo "# Done. Files in $DATA/:"
ls -lh "$DATA/"
echo
echo "# Quick benchmark:"
echo "#   ./build/aclab --patterns $DATA/patterns_en.txt \\"
echo "#                 --input    $DATA/simplewiki.txt  \\"
echo "#                 --searcher pthread_chunked --threads $(nproc) \\"
echo "#                 --warmup 1 --iters 3"
