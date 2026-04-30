#!/usr/bin/env bash
# Sweep the registered searchers across a range of thread counts on a
# synthetic corpus. Edit BYTES / PATTERN_FILE for your own experiments.
set -euo pipefail

cd "$(dirname "$0")/.."

BIN=build/aclab
[[ -x "$BIN" ]] || make -j

DATA_DIR=build/bench_data
mkdir -p "$DATA_DIR"

TEXT="$DATA_DIR/text.bin"
PATS="$DATA_DIR/patterns.txt"
BYTES="${BYTES:-67108864}" # 64 MiB by default

# Generate a reproducible synthetic corpus if not present.
if [[ ! -s "$TEXT" || $(stat -c%s "$TEXT") -ne "$BYTES" ]]; then
  echo "# Generating $BYTES bytes of synthetic text into $TEXT"
  head -c "$BYTES" /dev/urandom | tr -dc 'a-zA-Z0-9 \n' | head -c "$BYTES" > "$TEXT" || true
  # Top up if tr stripped too many bytes.
  while (( $(stat -c%s "$TEXT") < BYTES )); do
    head -c "$BYTES" /dev/urandom | tr -dc 'a-zA-Z0-9 \n' >> "$TEXT" || true
  done
  truncate -s "$BYTES" "$TEXT"
fi

if [[ ! -s "$PATS" ]]; then
  cat > "$PATS" <<'EOF'
the
and
of
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
at
be
this
have
from
or
one
had
by
word
but
not
what
all
were
we
when
your
can
said
there
EOF
fi

NPROC=${NPROC:-$(nproc)}

echo
echo "## Sequential baseline"
"$BIN" --patterns "$PATS" --input "$TEXT" --searcher sequential --warmup 1 --iters 5

echo
echo "## Parallel sweep"
for T in 1 2 4 8 "$NPROC"; do
  "$BIN" --patterns "$PATS" --input "$TEXT" \
    --searcher pthread_chunked --threads "$T" \
    --warmup 1 --iters 5
done
