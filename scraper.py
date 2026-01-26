#!/usr/bin/env python3
"""
Web Scraper - Iterative URL crawler with regex-based link extraction
"""
import re
import time
import socket
import json
import urllib.request
import urllib.parse
from urllib.error import URLError, HTTPError
from typing import Set, List
from pathlib import Path
import glob as file_glob

# Maximum execution time in seconds (5 hours)
MAX_EXECUTION_TIME = 18000

# State file for saving and resuming scraper progress
STATE_FILE = "state/scraper_state.json"

# Maximum URLs per chunk file (approximately 7-8MB per chunk, well under GitHub's 50MB limit)
MAX_URLS_PER_CHUNK = 500000

def save_state(visited_urls: Set[str], to_visit_urls: Set[str], start_url: str, 
               pages_scraped: int, urls_found: int, elapsed_time: float):
    """
    Save the current scraper state to JSON files, splitting large URL lists into chunks.
    
    Args:
        visited_urls: Set of URLs that have been visited
        to_visit_urls: Set of URLs that need to be visited
        start_url: The initial URL
        pages_scraped: Number of pages scraped so far
        urls_found: Total URLs found so far
        elapsed_time: Time elapsed since start
    """
    save_start = time.time()
    
    # Ensure state directory exists
    Path(STATE_FILE).parent.mkdir(parents=True, exist_ok=True)
    
    # Clean up old chunk files first
    _cleanup_old_chunks()
    
    # Convert sets to lists
    visited_list = list(visited_urls)
    to_visit_list = list(to_visit_urls)
    
    # Split large lists into chunks
    visited_chunks = _split_into_chunks(visited_list, MAX_URLS_PER_CHUNK)
    to_visit_chunks = _split_into_chunks(to_visit_list, MAX_URLS_PER_CHUNK)
    
    # Save chunks to separate files
    visited_chunk_files = []
    for i, chunk in enumerate(visited_chunks):
        chunk_file = f"state/scraper_state_visited_{i}.json"
        with open(chunk_file, 'w') as f:
            json.dump(chunk, f)
        visited_chunk_files.append(chunk_file)
    
    to_visit_chunk_files = []
    for i, chunk in enumerate(to_visit_chunks):
        chunk_file = f"state/scraper_state_to_visit_{i}.json"
        with open(chunk_file, 'w') as f:
            json.dump(chunk, f)
        to_visit_chunk_files.append(chunk_file)
    
    # Save main state with references to chunks
    state = {
        'start_url': start_url,
        'pages_scraped': pages_scraped,
        'urls_found': urls_found,
        'elapsed_time': elapsed_time,
        'timestamp': time.time(),
        'visited_urls_chunks': visited_chunk_files,
        'to_visit_urls_chunks': to_visit_chunk_files,
        'visited_urls_count': len(visited_list),
        'to_visit_urls_count': len(to_visit_list)
    }
    
    with open(STATE_FILE, 'w') as f:
        json.dump(state, f, indent=2)
    
    save_time = time.time() - save_start
    total_chunks = len(visited_chunk_files) + len(to_visit_chunk_files)
    print(f"  State saved to {STATE_FILE} with {total_chunks} chunk files (took {save_time:.3f}s)")

def load_state():
    """
    Load scraper state from JSON files, including any chunk files.
    
    Returns:
        Dictionary with state data, or None if file doesn't exist
    """
    load_start = time.time()
    
    state_path = Path(STATE_FILE)
    if not state_path.exists():
        return None
    
    try:
        with open(STATE_FILE, 'r') as f:
            state = json.load(f)
        
        # Check if this is the new format with chunks
        if 'visited_urls_chunks' in state and 'to_visit_urls_chunks' in state:
            # Load visited URLs from chunks
            visited_urls = []
            for chunk_file in state['visited_urls_chunks']:
                if Path(chunk_file).exists():
                    with open(chunk_file, 'r') as f:
                        visited_urls.extend(json.load(f))
            
            # Load to_visit URLs from chunks
            to_visit_urls = []
            for chunk_file in state['to_visit_urls_chunks']:
                if Path(chunk_file).exists():
                    with open(chunk_file, 'r') as f:
                        to_visit_urls.extend(json.load(f))
            
            # Convert lists back to sets
            state['visited_urls'] = set(visited_urls)
            state['to_visit_urls'] = set(to_visit_urls)
        else:
            # Old format - backward compatibility
            state['visited_urls'] = set(state.get('visited_urls', []))
            state['to_visit_urls'] = set(state.get('to_visit_urls', []))
        
        load_time = time.time() - load_start
        print(f"  State loaded in {load_time:.3f}s")
        
        return state
    except (json.JSONDecodeError, KeyError) as e:
        print(f"Error loading state file: {e}")
        return None

def _split_into_chunks(items: List[str], chunk_size: int) -> List[List[str]]:
    """
    Split a list into chunks of maximum size.
    
    Args:
        items: List of items to split
        chunk_size: Maximum number of items per chunk
        
    Returns:
        List of chunks (each chunk is a list)
    """
    if not items:
        return []
    
    chunks = []
    for i in range(0, len(items), chunk_size):
        chunks.append(items[i:i + chunk_size])
    return chunks

def _cleanup_old_chunks():
    """Remove old chunk files before saving new ones."""
    # Patterns to match chunk files
    patterns = ["state/scraper_state_visited_*.json", "state/scraper_state_to_visit_*.json"]
    
    for pattern in patterns:
        for old_file in file_glob.glob(pattern):
            try:
                Path(old_file).unlink()
            except OSError:
                pass

def extract_urls(html_content: str, base_url: str) -> Set[str]:
    """
    Extract URLs from HTML content using regex.
    
    Args:
        html_content: The HTML content to parse
        base_url: The base URL to resolve relative URLs
        
    Returns:
        Set of extracted URLs
    """
    extract_start = time.time()
    urls = set()
    
    # Regex patterns to match URLs in various formats
    patterns = [
        r'href=["\']([^"\']+)["\']',  # href attributes
        r'src=["\']([^"\']+)["\']',   # src attributes
        r'http[s]?://[^\s<>"{}|\\^`\[\]]+[^\s<>"{}|\\^`\[\].,;:!?\'\")]',  # direct URLs
    ]
    
    for pattern in patterns:
        matches = re.findall(pattern, html_content, re.IGNORECASE)
        for match in matches:
            # Resolve relative URLs
            try:
                absolute_url = urllib.parse.urljoin(base_url, match)
                # Only add http/https URLs
                if absolute_url.startswith(('http://', 'https://')):
                    urls.add(absolute_url)
            except ValueError:
                continue
    
    extract_time = time.time() - extract_start
    if extract_time > 0.01:  # Only log if extraction took more than 10ms
        print(f"    URL extraction took {extract_time:.3f}s")
    
    return urls

def fetch_url(url: str, timeout: int = 10) -> str:
    """
    Fetch content from a URL.
    
    Args:
        url: The URL to fetch
        timeout: Request timeout in seconds
        
    Returns:
        The fetched content as string
    """
    fetch_start = time.time()
    
    try:
        headers = {
            'User-Agent': 'Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/58.0.3029.110 Safari/537.3'
        }
        req = urllib.request.Request(url, headers=headers)
        with urllib.request.urlopen(req, timeout=timeout) as response:
            content = response.read()
            # Try to decode with common encodings
            try:
                decoded = content.decode('utf-8')
            except UnicodeDecodeError:
                try:
                    decoded = content.decode('latin-1')
                except UnicodeDecodeError:
                    decoded = content.decode('utf-8', errors='ignore')
            
            fetch_time = time.time() - fetch_start
            print(f"    Fetched in {fetch_time:.3f}s")
            return decoded
    except (URLError, HTTPError, socket.timeout) as e:
        fetch_time = time.time() - fetch_start
        print(f"    Error fetching (took {fetch_time:.3f}s): {e}")
        return ""
    except Exception as e:
        fetch_time = time.time() - fetch_start
        print(f"    Unexpected error (took {fetch_time:.3f}s): {e}")
        return ""

def scrape(start_url: str, max_pages: int = 100, resume: bool = False, save_interval: int = 10) -> dict:
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
    # Store the original start time for tracking total elapsed time
    original_elapsed = 0
    
    # Try to resume from saved state
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
            
            # When resuming, treat max_pages as additional pages to scrape
            # rather than absolute limit, to allow continued progress
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
        # Start fresh
        visited_urls = set()
        to_visit_urls = {start_url}
        pages_scraped = 0
        urls_found = 0
        actual_start_url = start_url
    
    # Record when this session started
    session_start_time = time.time()
    
    print(f"Starting web scraper from: {actual_start_url}")
    print(f"Max execution time: {MAX_EXECUTION_TIME} seconds ({MAX_EXECUTION_TIME/3600} hour)")
    print(f"Max pages: {max_pages}")
    print(f"State will be saved every {save_interval} pages")
    print("-" * 80)
    
    # Check if queue is empty when resuming
    if resume and not to_visit_urls:
        print("\nWarning: No URLs in queue to scrape!")
        print("The scraper has already visited all discoverable URLs from the start URL.")
        print("Scraping cannot continue without URLs in the queue.")
        print("-" * 80)
    
    # Iterative processing instead of recursion
    while to_visit_urls:
        # Check execution time (per session, not cumulative across sessions)
        session_elapsed = time.time() - session_start_time
        total_elapsed = original_elapsed + session_elapsed
        
        if session_elapsed > MAX_EXECUTION_TIME:
            print(f"\nSession execution time limit reached ({session_elapsed:.2f} seconds)")
            break
        
        # Check max pages limit
        if pages_scraped >= max_pages:
            print(f"\nMax pages limit reached ({pages_scraped} pages)")
            break
        
        # Get next URL to process
        current_url = to_visit_urls.pop()
        
        # Skip if already visited
        if current_url in visited_urls:
            continue
        
        # Mark as visited
        visited_urls.add(current_url)
        pages_scraped += 1
        
        print(f"[{pages_scraped}] Scraping: {current_url}")
        
        # Fetch content
        content = fetch_url(current_url)
        if not content:
            continue
        
        # Extract URLs from content
        found_urls = extract_urls(content, current_url)
        urls_found += len(found_urls)
        
        # Add new URLs to the to_visit set (only if not already visited)
        new_urls = found_urls - visited_urls
        to_visit_urls.update(new_urls)
        
        print(f"  Found {len(found_urls)} URLs ({len(new_urls)} new)")
        print(f"  Queue size: {len(to_visit_urls)}, Visited: {len(visited_urls)}")
        print(f"  Elapsed time: {total_elapsed:.2f}s")
        
        # Save state periodically
        if pages_scraped % save_interval == 0:
            save_state(visited_urls, to_visit_urls, actual_start_url, 
                      pages_scraped, urls_found, total_elapsed)
    
    # Calculate final statistics
    session_elapsed = time.time() - session_start_time
    total_time = original_elapsed + session_elapsed
    
    # Save final state
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
        'to_visit_urls': to_visit_urls
    }

def main():
    """Main entry point for the scraper."""
    import sys
    
    # Wikipedia preset URL
    WIKIPEDIA_URL = "https://en.wikipedia.org/wiki/Main_Page"
    
    # Parse flags and arguments
    resume = '--resume' in sys.argv
    use_wikipedia = '--wikipedia' in sys.argv or '-w' in sys.argv
    show_help = '-h' in sys.argv or '--help' in sys.argv
    
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
    
    # Default values
    start_url = ""
    max_pages = 100
    
    # Parse max_pages - find first numeric argument
    for arg in sys.argv[1:]:
        if arg.isdigit():
            max_pages = int(arg)
            break
    
    # Determine mode and start_url
    if use_wikipedia:
        # Wikipedia mode
        start_url = WIKIPEDIA_URL
        if not resume:
            print("=" * 80)
            print("Wikipedia Web Scraper")
            print("=" * 80)
            print(f"Starting from: {WIKIPEDIA_URL}")
            print()
    elif not resume:
        # Normal mode - requires URL (unless resuming)
        if len(sys.argv) < 2:
            print("Usage: python scraper.py [url|--wikipedia] [max_pages] [--resume]")
            print("Example: python scraper.py https://example.com 50")
            print("Example: python scraper.py --wikipedia 100")
            print("Example: python scraper.py --resume")
            print()
            print("Use --help for more information")
            sys.exit(1)
        
        # First non-flag, non-numeric argument is the URL
        for arg in sys.argv[1:]:
            if not arg.startswith('-') and not arg.isdigit():
                start_url = arg
                break
        
        if not start_url:
            print("Error: URL required (or use --wikipedia or --resume)")
            sys.exit(1)
        
        # Validate URL
        if not start_url.startswith(('http://', 'https://')):
            print("Error: URL must start with http:// or https://")
            sys.exit(1)
    
    # If resuming without --wikipedia, start_url will be loaded from state
    # Run the scraper
    results = scrape(start_url, max_pages, resume=resume)
    
    # Optionally save results to a file
    output_file = "scraper_results.txt"
    with open(output_file, 'w') as f:
        f.write(f"Web Scraper Results\n")
        f.write(f"{'=' * 80}\n")
        f.write(f"Start URL: {results['start_url']}\n")
        f.write(f"Total Time: {results['total_time']:.2f} seconds\n")
        f.write(f"Pages Scraped: {results['pages_scraped']}\n")
        f.write(f"Total URLs Found: {results['urls_found']}\n")
        f.write(f"Unique URLs Visited: {results['visited_count']}\n")
        f.write(f"\nVisited URLs:\n")
        f.write(f"{'-' * 80}\n")
        for url in sorted(results['visited_urls']):
            f.write(f"{url}\n")
    
    # Print summary
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
    
    # Provide resume command based on mode
    if use_wikipedia:
        print(f"To resume scraping, run: python scraper.py --wikipedia {max_pages} --resume")
    else:
        print(f"To resume scraping, run: python scraper.py --resume")
    print("=" * 80)

if __name__ == "__main__":
    main()
