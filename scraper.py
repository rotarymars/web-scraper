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
from typing import Set
from pathlib import Path

# Maximum execution time in seconds (6 hours)
MAX_EXECUTION_TIME = 21600

# State file for saving and resuming scraper progress
STATE_FILE = "scraper_state.json"

def save_state(visited_urls: Set[str], to_visit_urls: Set[str], start_url: str, 
               pages_scraped: int, urls_found: int, elapsed_time: float):
    """
    Save the current scraper state to a JSON file.
    
    Args:
        visited_urls: Set of URLs that have been visited
        to_visit_urls: Set of URLs that need to be visited
        start_url: The initial URL
        pages_scraped: Number of pages scraped so far
        urls_found: Total URLs found so far
        elapsed_time: Time elapsed since start
    """
    state = {
        'start_url': start_url,
        'visited_urls': list(visited_urls),
        'to_visit_urls': list(to_visit_urls),
        'pages_scraped': pages_scraped,
        'urls_found': urls_found,
        'elapsed_time': elapsed_time,
        'timestamp': time.time()
    }
    
    with open(STATE_FILE, 'w') as f:
        json.dump(state, f, indent=2)
    
    print(f"  State saved to {STATE_FILE}")

def load_state():
    """
    Load scraper state from a JSON file.
    
    Returns:
        Dictionary with state data, or None if file doesn't exist
    """
    state_path = Path(STATE_FILE)
    if not state_path.exists():
        return None
    
    try:
        with open(STATE_FILE, 'r') as f:
            state = json.load(f)
        
        # Convert lists back to sets
        state['visited_urls'] = set(state['visited_urls'])
        state['to_visit_urls'] = set(state['to_visit_urls'])
        
        return state
    except (json.JSONDecodeError, KeyError) as e:
        print(f"Error loading state file: {e}")
        return None

def extract_urls(html_content: str, base_url: str) -> Set[str]:
    """
    Extract URLs from HTML content using regex.
    
    Args:
        html_content: The HTML content to parse
        base_url: The base URL to resolve relative URLs
        
    Returns:
        Set of extracted URLs
    """
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
    try:
        headers = {
            'User-Agent': 'Mozilla/5.0 (Web Scraper Bot)'
        }
        req = urllib.request.Request(url, headers=headers)
        with urllib.request.urlopen(req, timeout=timeout) as response:
            content = response.read()
            # Try to decode with common encodings
            try:
                return content.decode('utf-8')
            except UnicodeDecodeError:
                try:
                    return content.decode('latin-1')
                except UnicodeDecodeError:
                    return content.decode('utf-8', errors='ignore')
    except (URLError, HTTPError, socket.timeout) as e:
        print(f"Error fetching {url}: {e}")
        return ""
    except Exception as e:
        print(f"Unexpected error fetching {url}: {e}")
        return ""

def scrape(start_url: str, max_pages: int = 100, resume: bool = False, save_interval: int = 10) -> dict:
    """
    Scrape URLs starting from a single URL, using iterative processing.
    
    Args:
        start_url: The initial URL to start scraping from
        max_pages: Maximum number of pages to scrape
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
    
    # Iterative processing instead of recursion
    while to_visit_urls:
        # Check execution time (session time + any previous elapsed time)
        session_elapsed = time.time() - session_start_time
        total_elapsed = original_elapsed + session_elapsed
        
        if total_elapsed > MAX_EXECUTION_TIME:
            print(f"\nExecution time limit reached ({total_elapsed:.2f} seconds)")
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
    
    # Check for resume flag
    resume = '--resume' in sys.argv
    
    if resume:
        # When resuming, URL is optional (loaded from state)
        if len(sys.argv) < 2:
            print("Usage: python scraper.py --resume [max_pages]")
            print("Example: python scraper.py --resume")
            print("Example: python scraper.py --resume 200")
            sys.exit(1)
        
        # Parse arguments for resume mode
        start_url = ""  # Will be loaded from state
        max_pages = 100
        
        for arg in sys.argv[1:]:
            if arg != '--resume' and arg.isdigit():
                max_pages = int(arg)
    else:
        # Normal mode requires URL
        if len(sys.argv) < 2:
            print("Usage: python scraper.py <start_url> [max_pages] [--resume]")
            print("Example: python scraper.py https://example.com 50")
            print("Example: python scraper.py https://example.com 50 --resume")
            print("\nOptions:")
            print("  --resume    Resume from saved state (scraper_state.json)")
            sys.exit(1)
        
        start_url = sys.argv[1]
        max_pages = 100
        
        # Parse arguments
        for arg in sys.argv[2:]:
            if arg.isdigit():
                max_pages = int(arg)
        
        # Validate URL
        if not start_url.startswith(('http://', 'https://')):
            print("Error: URL must start with http:// or https://")
            sys.exit(1)
    
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
    
    print(f"\nResults saved to {output_file}")
    print(f"State saved to {STATE_FILE}")
    print(f"\nTo resume scraping, run: python scraper.py {results['start_url']} {max_pages} --resume")

if __name__ == "__main__":
    main()
