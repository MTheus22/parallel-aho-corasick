#!/usr/bin/env python3
"""
Extract AC-compatible literal byte patterns from YARA rule files.

YARA strings:
    $s1 = "literal text"              -> ASCII literal
    $s2 = "literal" nocase            -> lowercased
    $s3 = { DE AD BE EF }             -> hex literal
    $s4 = { DE ?? BE EF }             -> hex with wildcards (skipped)
    $s5 = /regex/                     -> regex (skipped)

Usage:
    python3 scripts/extract_yara_patterns.py rules/*.yar > data/patterns_yara.txt
    python3 scripts/extract_yara_patterns.py -i mal.yar --min-len 6 --stats
"""

import argparse
import re
import sys


# Quoted string: $foo = "..." [nocase] [ascii] [wide] [fullword]
_STR_RE = re.compile(
    r'\$\w*\s*=\s*"((?:[^"\\]|\\.)*)"((?:\s+\w+)*)',
    re.IGNORECASE
)

# Hex string: $foo = { DE AD ... }  (no wildcards / jumps)
_HEX_RE = re.compile(
    r'\$\w*\s*=\s*\{([^}]*)\}',
    re.IGNORECASE
)

_NOCASE_MODIFIER = re.compile(r'\bnocase\b', re.IGNORECASE)


def unescape_yara_string(s: str) -> bytes:
    """Handle YARA escape sequences in double-quoted strings."""
    result = bytearray()
    i = 0
    while i < len(s):
        if s[i] == '\\' and i + 1 < len(s):
            nx = s[i+1]
            if nx == 'n':  result.append(0x0A)
            elif nx == 't': result.append(0x09)
            elif nx == 'r': result.append(0x0D)
            elif nx == '\\': result.append(0x5C)
            elif nx == '"': result.append(0x22)
            elif nx == 'x' and i + 3 < len(s):
                try:
                    result.append(int(s[i+2:i+4], 16))
                    i += 2
                except ValueError:
                    result.append(ord(nx))
            else:
                result.append(ord(nx))
            i += 2
        else:
            result.append(ord(s[i]) & 0xFF)
            i += 1
    return bytes(result)


def parse_hex_string(raw: str) -> bytes | None:
    """
    Parse YARA hex string body. Returns None if it contains wildcards or
    jump operators (those can't be represented as exact AC patterns).
    """
    if '?' in raw or '[' in raw or '(' in raw or '|' in raw:
        return None
    cleaned = re.sub(r'\s+', '', raw)
    try:
        return bytes.fromhex(cleaned)
    except ValueError:
        return None


def extract_from_file(fh, nocase: bool, min_len: int, max_len: int):
    """Yield (pattern_bytes,) from one YARA file."""
    in_strings = False
    for line in fh:
        stripped = line.strip()
        # Crude section detector: YARA strings section starts with "strings:"
        if re.match(r'^\s*strings\s*:', stripped, re.IGNORECASE):
            in_strings = True
        elif re.match(r'^\s*(condition|meta)\s*:', stripped, re.IGNORECASE):
            in_strings = False
        if not in_strings:
            continue

        # Literal strings
        for m in _STR_RE.finditer(line):
            modifiers = m.group(2)
            pat = unescape_yara_string(m.group(1))
            if nocase or bool(_NOCASE_MODIFIER.search(modifiers)):
                pat = pat.lower()
            if len(pat) >= min_len:
                yield pat[:max_len] if max_len else pat

        # Hex strings
        for m in _HEX_RE.finditer(line):
            pat = parse_hex_string(m.group(1))
            if pat is None:
                continue
            if nocase:
                pat = pat.lower()
            if len(pat) >= min_len:
                yield pat[:max_len] if max_len else pat


def main():
    ap = argparse.ArgumentParser(description='Extract literal patterns from YARA rules')
    ap.add_argument('inputs', nargs='*', metavar='FILE')
    ap.add_argument('-i', '--input',  help='Single input file')
    ap.add_argument('-o', '--output', default='-')
    ap.add_argument('--min-len', type=int, default=4)
    ap.add_argument('--max-len', type=int, default=0)
    ap.add_argument('--nocase', action='store_true')
    ap.add_argument('--dedupe', action='store_true', default=True)
    ap.add_argument('--no-dedupe', dest='dedupe', action='store_false')
    ap.add_argument('--stats', action='store_true')
    args = ap.parse_args()

    sources = []
    if args.input:
        sources.append(open(args.input, 'r', encoding='utf-8', errors='replace'))
    for p in args.inputs:
        sources.append(open(p, 'r', encoding='utf-8', errors='replace'))
    if not sources:
        sources = [sys.stdin]

    out = open(args.output, 'wb') if args.output != '-' else sys.stdout.buffer

    seen = set()
    total = dupes = 0
    for fh in sources:
        for pat in extract_from_file(fh, args.nocase, args.min_len, args.max_len):
            if args.dedupe and pat in seen:
                dupes += 1
                continue
            seen.add(pat)
            try:
                txt = pat.decode('ascii')
                if any(c < ' ' for c in txt):
                    raise ValueError
                out.write((txt + '\n').encode())
            except (UnicodeDecodeError, ValueError):
                escaped = ''.join(f'\\x{b:02x}' if b > 127 or b < 32
                                  else chr(b) for b in pat)
                out.write((escaped + '\n').encode())
            total += 1

    if out is not sys.stdout.buffer:
        out.close()
    for fh in sources:
        if fh is not sys.stdin:
            fh.close()
    if args.stats:
        print(f'Patterns extracted: {total}', file=sys.stderr)
        print(f'Duplicates removed: {dupes}', file=sys.stderr)


if __name__ == '__main__':
    main()
