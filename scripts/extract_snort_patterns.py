#!/usr/bin/env python3
"""
Extract AC-compatible literal byte patterns from Snort / Suricata rule files.

Snort rules contain content fields like:
    content:"SMBr";
    content:"|FF|SMB|73|"; nocase;
    content:"User-Agent|3A 20|curl";

This script extracts them as raw byte strings suitable for --patterns.
Regex patterns (pcre:) are skipped — they need a different engine.

Usage:
    python3 scripts/extract_snort_patterns.py rules/*.rules > data/patterns_snort.txt
    python3 scripts/extract_snort_patterns.py -i community.rules -o patterns.txt --min-len 4 --stats
"""

import argparse
import re
import sys
import os
from pathlib import Path


def parse_hex_segment(hex_str: str) -> bytes:
    """Convert Snort hex notation |DE AD BE EF| to bytes."""
    cleaned = re.sub(r'\s+', '', hex_str)
    try:
        return bytes.fromhex(cleaned)
    except ValueError:
        return b''


def parse_content_value(raw: str) -> bytes:
    """
    Parse a Snort content string value (without outer quotes).
    Handles mixed literal + |hex| segments.
    """
    result = bytearray()
    i = 0
    while i < len(raw):
        if raw[i] == '|':
            end = raw.find('|', i + 1)
            if end == -1:
                result.extend(raw[i:].encode('latin-1', errors='replace'))
                break
            result.extend(parse_hex_segment(raw[i+1:end]))
            i = end + 1
        else:
            result.append(ord(raw[i]) & 0xFF)
            i += 1
    return bytes(result)


# Matches: content:"..."; or content:'...'; with optional preceding not (!)
_CONTENT_RE = re.compile(
    r'content\s*:\s*(!?)\s*"((?:[^"\\]|\\.)*)"|'
    r"content\s*:\s*(!?)\s*'((?:[^'\\]|\\.)*)'",
    re.IGNORECASE
)

# Matches: nocase; appearing after a content field (rough proximity check)
_NOCASE_RE = re.compile(r'\bnocase\b', re.IGNORECASE)

# Matches: pcre:"..."; — we skip these
_PCRE_RE = re.compile(r'\bpcre\s*:', re.IGNORECASE)


def extract_from_rule(line: str, nocase: bool, min_len: int, max_len: int):
    """
    Yield (pattern_bytes, is_nocase, negated) tuples from one Snort rule line.
    """
    # Strip inline comments
    line = line.split('#', 1)[0].strip()
    if not line or line.startswith('#'):
        return

    # Find option block: everything between first ( and last )
    paren_open  = line.find('(')
    paren_close = line.rfind(')')
    if paren_open == -1 or paren_close == -1:
        return
    options = line[paren_open+1:paren_close]

    for m in _CONTENT_RE.finditer(options):
        negated = bool(m.group(1) or m.group(3))
        raw     = m.group(2) if m.group(2) is not None else m.group(4)
        data    = parse_content_value(raw)
        if len(data) < min_len:
            continue
        if max_len and len(data) > max_len:
            data = data[:max_len]
        is_nocase_field = bool(_NOCASE_RE.search(
            options[m.end():m.end()+60]))  # rough look-ahead
        if nocase or is_nocase_field:
            data = data.lower()
        yield data, is_nocase_field, negated


def main():
    ap = argparse.ArgumentParser(description='Extract literal patterns from Snort rules')
    ap.add_argument('inputs', nargs='*', metavar='FILE',
                    help='Snort rule files (default: stdin)')
    ap.add_argument('-i', '--input',  help='Single input file')
    ap.add_argument('-o', '--output', default='-', help='Output file (default: stdout)')
    ap.add_argument('--min-len', type=int, default=4,
                    help='Minimum pattern length in bytes (default: 4)')
    ap.add_argument('--max-len', type=int, default=0,
                    help='Truncate patterns longer than this (0 = no limit)')
    ap.add_argument('--nocase', action='store_true',
                    help='Force lowercase on all patterns')
    ap.add_argument('--skip-negated', action='store_true',
                    help='Skip content:! (negated) fields')
    ap.add_argument('--dedupe', action='store_true', default=True,
                    help='Remove duplicate patterns (default on)')
    ap.add_argument('--no-dedupe', dest='dedupe', action='store_false')
    ap.add_argument('--stats', action='store_true', help='Print stats to stderr')
    args = ap.parse_args()

    sources = []
    if args.input:
        sources.append(open(args.input, 'r', encoding='utf-8', errors='replace'))
    for path in args.inputs:
        sources.append(open(path, 'r', encoding='utf-8', errors='replace'))
    if not sources:
        sources = [sys.stdin]

    out = open(args.output, 'wb') if args.output != '-' else sys.stdout.buffer

    seen = set()
    total_rules = total_extracted = total_skipped_short = total_dupes = 0

    for fh in sources:
        for line in fh:
            line = line.rstrip('\n')
            if not line.strip() or line.strip().startswith('#'):
                continue
            total_rules += 1
            for pat, is_nc, negated in extract_from_rule(
                    line, args.nocase, args.min_len, args.max_len):
                if args.skip_negated and negated:
                    continue
                if len(pat) < args.min_len:
                    total_skipped_short += 1
                    continue
                # Only emit printable ASCII patterns; embed hex for others
                try:
                    txt = pat.decode('ascii')
                    if any(c < ' ' for c in txt):
                        raise ValueError
                    line_out = (txt + '\n').encode()
                except (UnicodeDecodeError, ValueError):
                    # Represent non-ASCII as \xNN escapes (comment in output)
                    escaped = ''.join(f'\\x{b:02x}' if b > 127 or b < 32
                                      else chr(b) for b in pat)
                    line_out = (escaped + '\n').encode()

                key = pat
                if args.dedupe and key in seen:
                    total_dupes += 1
                    continue
                seen.add(key)
                out.write(line_out)
                total_extracted += 1

    if out is not sys.stdout.buffer:
        out.close()
    for fh in sources:
        if fh is not sys.stdin:
            fh.close()

    if args.stats:
        print(f'Rules processed:   {total_rules}',        file=sys.stderr)
        print(f'Patterns extracted:{total_extracted}',     file=sys.stderr)
        print(f'Skipped (too short):{total_skipped_short}', file=sys.stderr)
        print(f'Duplicates removed: {total_dupes}',        file=sys.stderr)


if __name__ == '__main__':
    main()
