#!/usr/bin/env python3
"""
Web Scraper - Iterative URL crawler with regex-based link extraction
"""
import io
import json
import re
import socket
import sys
import time
import urllib.parse
import urllib.request
import zipfile
from itertools import islice
from pathlib import Path
from typing import Iterator, List, Optional, Set
from urllib.error import HTTPError, URLError

# Maximum execution time in seconds (5 hours)
MAX_EXECUTION_TIME = 18000

# State file for saving and resuming scraper progress
STATE_DIR = Path("state")
STATE_FILE = STATE_DIR / "scraper_state.json"

# Maximum URLs per chunk file (approximately 7-8MB per chunk, well under GitHub's 100MB per-file limit)
MAX_URLS_PER_CHUNK = 500000

# Pre-compiled regex patterns for URL extraction (avoids recompilation on every call)
_RE_HREF = re.compile(r'href=["\']([^"\']+)["\']', re.IGNORECASE)
_RE_SRC = re.compile(r'src=["\']([^"\']+)["\']', re.IGNORECASE)
_RE_URL = re.compile(
    r'https?://[^\s<>"{}|\\^`\[\]]+[^\s<>"{}|\\^`\[\].,;:!?\'\")]',
    re.IGNORECASE,
)
_ATTR_PATTERNS = (_RE_HREF, _RE_SRC)

# Reusable User-Agent header
_REQUEST_HEADERS = {
    'User-Agent': (
        'Mozilla/5.0 (Windows NT 10.0; Win64; x64) '
        'AppleWebKit/537.36 (KHTML, like Gecko) '
        'Chrome/58.0.3029.110 Safari/537.3'
    )
}


def _save_chunk_to_zip(chunk: List[str], zip_path: str):
    """Save a chunk of URLs to a ZIP file using streaming to reduce memory usage."""
    with zipfile.ZipFile(zip_path, 'w', zipfile.ZIP_DEFLATED) as zf:
        with zf.open('urls.json', 'w') as zip_entry:
            with io.TextIOWrapper(zip_entry, encoding='utf-8') as text_stream:
                json.dump(chunk, text_stream)


def _load_chunk_from_zip(zip_path: str) -> List[str]:
    """Load a chunk of URLs from a ZIP file using streaming to reduce memory usage."""
    try:
        with zipfile.ZipFile(zip_path, 'r') as zf:
            with zf.open('urls.json') as json_file:
                with io.TextIOWrapper(json_file, encoding='utf-8') as text_file:
                    return json.load(text_file)
    except (zipfile.BadZipFile, KeyError) as e:
        print(f"Error loading ZIP file {zip_path}: {e}")
        raise


def _iter_chunks(items: List[str], chunk_size: int) -> Iterator[List[str]]:
    """Yield successive chunks from a list without building an intermediate list of lists."""
    it = iter(items)
    while True:
        chunk = list(islice(it, chunk_size))
        if not chunk:
            return
        yield chunk


def _cleanup_old_chunks():
    """Remove old chunk files before saving new ones."""
    if not STATE_DIR.is_dir():
        return
    for path in STATE_DIR.iterdir():
        if path.suffix in ('.json', '.zip') and (
            path.name.startswith('scraper_state_visited_')
            or path.name.startswith('scraper_state_to_visit_')
        ):
            try:
                path.unlink()
            except OSError:
                pass


def _load_chunks(chunk_files: List[str]) -> Set[str]:
    """Load URL chunks from a list of chunk file paths, returning a set."""
    urls: Set[str] = set()
    for chunk_file in chunk_files:
        path = Path(chunk_file)
        if not path.exists():
            continue
        if chunk_file.endswith('.zip'):
            urls.update(_load_chunk_from_zip(chunk_file))
        else:
            with open(chunk_file, 'r') as f:
                urls.update(json.load(f))
    return urls


def save_state(visited_urls: Set[str], to_visit_urls: Set[str], start_url: str,
               pages_scraped: int, urls_found: int, elapsed_time: float):
    """Save the current scraper state to JSON files, splitting large URL lists into chunks."""
    save_start = time.monotonic()

    STATE_DIR.mkdir(parents=True, exist_ok=True)
    _cleanup_old_chunks()

    visited_list = list(visited_urls)
    to_visit_list = list(to_visit_urls)

    visited_chunk_files = []
    for i, chunk in enumerate(_iter_chunks(visited_list, MAX_URLS_PER_CHUNK)):
        chunk_file = str(STATE_DIR / f"scraper_state_visited_{i}.zip")
        _save_chunk_to_zip(chunk, chunk_file)
        visited_chunk_files.append(chunk_file)

    to_visit_chunk_files = []
    for i, chunk in enumerate(_iter_chunks(to_visit_list, MAX_URLS_PER_CHUNK)):
        chunk_file = str(STATE_DIR / f"scraper_state_to_visit_{i}.zip")
        _save_chunk_to_zip(chunk, chunk_file)
        to_visit_chunk_files.append(chunk_file)

    state = {
        'start_url': start_url,
        'pages_scraped': pages_scraped,
        'urls_found': urls_found,
        'elapsed_time': elapsed_time,
        'timestamp': time.time(),
        'visited_urls_chunks': visited_chunk_files,
        'to_visit_urls_chunks': to_visit_chunk_files,
        'visited_urls_count': len(visited_list),
        'to_visit_urls_count': len(to_visit_list),
    }

    with open(STATE_FILE, 'w') as f:
        json.dump(state, f, indent=2)

    save_time = time.monotonic() - save_start
    total_chunks = len(visited_chunk_files) + len(to_visit_chunk_files)
    print(f"  State saved to {STATE_FILE} with {total_chunks} chunk files (took {save_time:.3f}s)")


def load_state() -> Optional[dict]:
    """Load scraper state from JSON files, including any chunk files."""
    load_start = time.monotonic()

    if not STATE_FILE.exists():
        return None

    try:
        with open(STATE_FILE, 'r') as f:
            state = json.load(f)

        if 'visited_urls_chunks' in state and 'to_visit_urls_chunks' in state:
            state['visited_urls'] = _load_chunks(state['visited_urls_chunks'])
            state['to_visit_urls'] = _load_chunks(state['to_visit_urls_chunks'])
        else:
            state['visited_urls'] = set(state.get('visited_urls', []))
            state['to_visit_urls'] = set(state.get('to_visit_urls', []))

        load_time = time.monotonic() - load_start
        print(f"  State loaded in {load_time:.3f}s")
        return state
    except (json.JSONDecodeError, KeyError) as e:
        print(f"Error loading state file: {e}")
        return None


def extract_urls(html_content: str, base_url: str) -> Set[str]:
    """Extract URLs from HTML content using pre-compiled regex patterns."""
    urls: Set[str] = set()
    urljoin = urllib.parse.urljoin

    # Extract href and src attribute values, then resolve relative URLs
    for pattern in _ATTR_PATTERNS:
        for match in pattern.finditer(html_content):
            try:
                url = urljoin(base_url, match.group(1))
                if url.startswith(('http://', 'https://')):
                    urls.add(url)
            except ValueError:
                continue

    # Extract standalone URLs (already absolute)
    for match in _RE_URL.finditer(html_content):
        urls.add(match.group(0))

    return urls


def fetch_url(url: str, timeout: int = 10) -> str:
    """Fetch content from a URL."""
    try:
        req = urllib.request.Request(url, headers=_REQUEST_HEADERS)
        with urllib.request.urlopen(req, timeout=timeout) as response:
            content = response.read()
            # latin-1 never raises UnicodeDecodeError (all byte values are valid)
            try:
                return content.decode('utf-8')
            except UnicodeDecodeError:
                return content.decode('latin-1')
    except (URLError, HTTPError, socket.timeout, OSError) as e:
        print(f"    Error fetching: {e}")
        return ""
    except Exception as e:
        print(f"    Unexpected error: {e}")
        return ""


def scrape(start_url: str, max_pages: int = 100, resume: bool = False,
           save_interval: int = 10) -> dict:
    """
    Scrape URLs starting from a single URL, using iterative processing.

    Args:
        start_url: The initial URL to start scraping from
        max_pages: Maximum number of pages to scrape. When resuming, this is
                   treated as additional pages to scrape (not absolute limit)
        resume: Whether to resume from saved state
        save_interval: Save state every N pages (default: 10)

    Returns:
        Dictionary with scraping statistics
    """
    original_elapsed = 0

    if resume:
        state = load_state()
        if state:
            print("Resuming from saved state...")
            print(f"  Previous start URL: {state['start_url']}")
            print(f"  Pages already scraped: {state['pages_scraped']}")
            print(f"  URLs in queue: {len(state['to_visit_urls'])}")
            print(f"  URLs visited: {len(state['visited_urls'])}")
            print("-" * 80)

            visited_urls = state['visited_urls']
            to_visit_urls = state['to_visit_urls']
            pages_scraped = state['pages_scraped']
            urls_found = state['urls_found']
            original_elapsed = state['elapsed_time']
            actual_start_url = state['start_url']

            max_pages = pages_scraped + max_pages
            print(f"  Will scrape up to {max_pages} total pages (continuing from {pages_scraped})")
        else:
            print("No saved state found. Starting fresh...")
            visited_urls = set()
            to_visit_urls = {start_url}
            pages_scraped = 0
            urls_found = 0
            actual_start_url = start_url
    else:
        visited_urls = set()
        to_visit_urls = {start_url}
        pages_scraped = 0
        urls_found = 0
        actual_start_url = start_url

    session_start = time.monotonic()

    print(f"Starting web scraper from: {actual_start_url}")
    print(f"Max execution time: {MAX_EXECUTION_TIME} seconds ({MAX_EXECUTION_TIME / 3600:.1f} hours)")
    print(f"Max pages: {max_pages}")
    print(f"State will be saved every {save_interval} pages")
    print("-" * 80)

    if resume and not to_visit_urls:
        print("\nWarning: No URLs in queue to scrape!")
        print("The scraper has already visited all discoverable URLs from the start URL.")
        print("Scraping cannot continue without URLs in the queue.")
        print("-" * 80)

    while to_visit_urls:
        session_elapsed = time.monotonic() - session_start
        if session_elapsed > MAX_EXECUTION_TIME:
            print(f"\nSession execution time limit reached ({session_elapsed:.2f} seconds)")
            break

        if pages_scraped >= max_pages:
            print(f"\nMax pages limit reached ({pages_scraped} pages)")
            break

        current_url = to_visit_urls.pop()

        if current_url in visited_urls:
            continue

        visited_urls.add(current_url)
        pages_scraped += 1
        total_elapsed = original_elapsed + session_elapsed

        print(f"[{pages_scraped}] Scraping: {current_url}")

        content = fetch_url(current_url)
        if not content:
            continue

        found_urls = extract_urls(content, current_url)
        urls_found += len(found_urls)

        new_urls = found_urls - visited_urls
        to_visit_urls.update(new_urls)

        print(f"  Found {len(found_urls)} URLs ({len(new_urls)} new)")
        print(f"  Queue size: {len(to_visit_urls)}, Visited: {len(visited_urls)}")
        print(f"  Elapsed time: {total_elapsed:.2f}s")

        if pages_scraped % save_interval == 0:
            save_state(visited_urls, to_visit_urls, actual_start_url,
                       pages_scraped, urls_found, total_elapsed)

    session_elapsed = time.monotonic() - session_start
    total_time = original_elapsed + session_elapsed

    save_state(visited_urls, to_visit_urls, actual_start_url,
               pages_scraped, urls_found, total_time)

    print("-" * 80)
    print("Scraping completed!")
    print(f"Total time: {total_time:.2f} seconds")
    print(f"Pages scraped: {pages_scraped}")
    print(f"Total URLs found: {urls_found}")
    print(f"Unique URLs visited: {len(visited_urls)}")
    print(f"URLs remaining in queue: {len(to_visit_urls)}")

    return {
        'start_url': actual_start_url,
        'total_time': total_time,
        'pages_scraped': pages_scraped,
        'urls_found': urls_found,
        'visited_count': len(visited_urls),
        'queue_remaining': len(to_visit_urls),
        'visited_urls': visited_urls,
        'to_visit_urls': to_visit_urls,
    }


def main():
    """Main entry point for the scraper."""
    WIKIPEDIA_URL = "https://en.wikipedia.org/wiki/Main_Page"

    args = set(sys.argv[1:])
    resume = '--resume' in args
    use_wikipedia = bool(args & {'--wikipedia', '-w'})
    show_help = bool(args & {'-h', '--help'})

    if show_help:
        print("Usage: python scraper.py [url|--wikipedia] [max_pages] [--resume]")
        print()
        print("Arguments:")
        print("  url                URL to start scraping from")
        print("  --wikipedia, -w    Start scraping from Wikipedia main page")
        print("  max_pages          Maximum number of pages to scrape (default: 100)")
        print("  --resume           Resume from saved state")
        print("  --help, -h         Show this help message")
        print()
        print("Examples:")
        print("  python scraper.py --wikipedia 50")
        print("  python scraper.py --wikipedia --resume")
        print("  python scraper.py https://example.com 100")
        print("  python scraper.py https://example.com 50 --resume")
        print("  python scraper.py --resume")
        sys.exit(0)

    max_pages = 100
    for arg in sys.argv[1:]:
        if arg.isdigit():
            max_pages = int(arg)
            break

    start_url = ""
    if use_wikipedia:
        start_url = WIKIPEDIA_URL
        if not resume:
            print("=" * 80)
            print("Wikipedia Web Scraper")
            print("=" * 80)
            print(f"Starting from: {WIKIPEDIA_URL}")
            print()
    elif not resume:
        if len(sys.argv) < 2:
            print("Usage: python scraper.py [url|--wikipedia] [max_pages] [--resume]")
            print("Example: python scraper.py https://example.com 50")
            print("Example: python scraper.py --wikipedia 100")
            print("Example: python scraper.py --resume")
            print()
            print("Use --help for more information")
            sys.exit(1)

        for arg in sys.argv[1:]:
            if not arg.startswith('-') and not arg.isdigit():
                start_url = arg
                break

        if not start_url:
            print("Error: URL required (or use --wikipedia or --resume)")
            sys.exit(1)

        if not start_url.startswith(('http://', 'https://')):
            print("Error: URL must start with http:// or https://")
            sys.exit(1)

    results = scrape(start_url, max_pages, resume=resume)

    output_file = "scraper_results.txt"
    with open(output_file, 'w') as f:
        f.write(f"Web Scraper Results\n")
        f.write(f"{'=' * 80}\n")
        f.write(f"Start URL: {results['start_url']}\n")
        f.write(f"Total Time: {results['total_time']:.2f} seconds\n")
        f.write(f"Pages Scraped: {results['pages_scraped']}\n")
        f.write(f"Total URLs Found: {results['urls_found']}\n")
        f.write(f"Unique URLs Visited: {results['visited_count']}\n")

    print()
    print("=" * 80)
    print("Scraping Summary")
    print("=" * 80)
    print(f"Start URL: {results['start_url']}")
    print(f"Pages scraped: {results['pages_scraped']}")
    print(f"Total URLs found: {results['urls_found']}")
    print(f"Unique URLs visited: {results['visited_count']}")
    print(f"URLs remaining in queue: {results['queue_remaining']}")
    print(f"Total time: {results['total_time']:.2f} seconds")
    print()
    print(f"Results saved to: {output_file}")
    print(f"State saved to: {STATE_FILE}")
    print()

    if use_wikipedia:
        print(f"To resume scraping, run: python scraper.py --wikipedia {max_pages} --resume")
    else:
        print(f"To resume scraping, run: python scraper.py --resume")
    print("=" * 80)


if __name__ == "__main__":
    main()
