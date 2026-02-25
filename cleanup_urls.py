#!/usr/bin/env python3
"""
URL cleanup script - reads all stored URL chunk files, removes any URL that
does not match the URL regex (including those containing \\n, \\r, or other
control/unprintable characters that corrupt the line-separated text format),
and resaves the files.
"""
import json
import re
import sys
import zipfile
import io
from pathlib import Path

from scraper import (
    STATE_DIR,
    STATE_FILE,
    _load_chunk_from_zip,
    _save_chunk_to_zip,
)

# Full-string URL validation regex: same pattern as scraper._RE_URL but
# anchored so the entire stored value must be a valid URL.  This rejects
# any entry that contains whitespace (including \n, \r, \t) or other
# characters that are illegal inside a URL.
_URL_RE = re.compile(
    r'^https?://[^\s<>"{}|\\^`\[\]]+[^\s<>"{}|\\^`\[\].,;:!?\'\")]$',
    re.IGNORECASE,
)


def _is_valid_url(url: str) -> bool:
    """Return True iff *url* matches the URL regex (no control chars allowed)."""
    return bool(_URL_RE.match(url))


def _load_txt_chunk(path: str) -> list[str]:
    """Load URLs from a plain-text file (one URL per line)."""
    with open(path, 'r', encoding='utf-8', errors='replace') as f:
        return [line for line in f.read().splitlines() if line]


def _save_txt_chunk(urls: list[str], path: str) -> None:
    """Save URLs to a plain-text file (one URL per line)."""
    with open(path, 'w', encoding='utf-8') as f:
        f.write('\n'.join(urls))
        if urls:
            f.write('\n')


def _process_chunk(chunk_file: str) -> tuple[int, int]:
    """
    Load *chunk_file*, filter out invalid URLs, and resave it.

    Returns ``(count_before, count_after)``.  If the file does not exist it
    is silently skipped and ``(0, 0)`` is returned.
    """
    path = Path(chunk_file)
    if not path.exists():
        print(f"  Warning: chunk file not found, skipping: {chunk_file}")
        return 0, 0

    if chunk_file.endswith('.zip'):
        urls = _load_chunk_from_zip(chunk_file)
    else:
        urls = _load_txt_chunk(chunk_file)

    before = len(urls)
    valid = [u for u in urls if _is_valid_url(u)]
    after = len(valid)

    if after < before:
        if chunk_file.endswith('.zip'):
            _save_chunk_to_zip(valid, chunk_file)
        else:
            _save_txt_chunk(valid, chunk_file)
        removed = before - after
        print(f"  {chunk_file}: removed {removed} invalid URL(s) ({after} kept)")
    else:
        print(f"  {chunk_file}: all {before} URL(s) valid")

    return before, after


def cleanup_urls() -> None:
    """Read every stored URL chunk, remove invalid entries, and resave."""
    if not STATE_FILE.exists():
        print(f"Error: state file not found: {STATE_FILE}")
        sys.exit(1)

    with open(STATE_FILE, 'r') as f:
        state = json.load(f)

    print("=" * 80)
    print("URL Cleanup")
    print("=" * 80)

    visited_before = visited_after = 0
    to_visit_before = to_visit_after = 0

    visited_chunks = state.get('visited_urls_chunks', [])
    if visited_chunks:
        print(f"\nProcessing visited URLs ({len(visited_chunks)} chunk file(s)):")
        for cf in visited_chunks:
            b, a = _process_chunk(cf)
            visited_before += b
            visited_after += a

    to_visit_chunks = state.get('to_visit_urls_chunks', [])
    if to_visit_chunks:
        print(f"\nProcessing to-visit URLs ({len(to_visit_chunks)} chunk file(s)):")
        for cf in to_visit_chunks:
            b, a = _process_chunk(cf)
            to_visit_before += b
            to_visit_after += a

    total_removed = (visited_before - visited_after) + (to_visit_before - to_visit_after)

    print(f"\n{'=' * 80}")
    print(f"Visited URLs  : {visited_before} -> {visited_after}"
          f" ({visited_before - visited_after} removed)")
    print(f"To-visit URLs : {to_visit_before} -> {to_visit_after}"
          f" ({to_visit_before - to_visit_after} removed)")
    print(f"Total removed : {total_removed}")
    print(f"{'=' * 80}")

    if total_removed > 0:
        state['visited_urls_count'] = visited_after
        state['to_visit_urls_count'] = to_visit_after
        with open(STATE_FILE, 'w') as f:
            json.dump(state, f, indent=2)
        print(f"State file updated: {STATE_FILE}")


if __name__ == "__main__":
    cleanup_urls()
