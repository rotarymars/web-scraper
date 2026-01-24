#!/usr/bin/env python3
"""
Web Scraper - Iterative URL crawler with regex-based link extraction
"""
import re
import time
import socket
import urllib.request
import urllib.parse
from urllib.error import URLError, HTTPError
from typing import Set

# Maximum execution time in seconds (1 hour)
MAX_EXECUTION_TIME = 3600

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

def scrape(start_url: str, max_pages: int = 100) -> dict:
    """
    Scrape URLs starting from a single URL, using iterative processing.
    
    Args:
        start_url: The initial URL to start scraping from
        max_pages: Maximum number of pages to scrape
        
    Returns:
        Dictionary with scraping statistics
    """
    # Set of URLs that have been visited/processed
    visited_urls: Set[str] = set()
    
    # Set of URLs that need to be processed
    to_visit_urls: Set[str] = {start_url}
    
    # Track statistics
    start_time = time.time()
    pages_scraped = 0
    urls_found = 0
    
    print(f"Starting web scraper from: {start_url}")
    print(f"Max execution time: {MAX_EXECUTION_TIME} seconds ({MAX_EXECUTION_TIME/3600} hour)")
    print(f"Max pages: {max_pages}")
    print("-" * 80)
    
    # Iterative processing instead of recursion
    while to_visit_urls:
        # Check execution time
        elapsed_time = time.time() - start_time
        if elapsed_time > MAX_EXECUTION_TIME:
            print(f"\nExecution time limit reached ({elapsed_time:.2f} seconds)")
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
        print(f"  Elapsed time: {elapsed_time:.2f}s")
    
    # Calculate final statistics
    end_time = time.time()
    total_time = end_time - start_time
    
    print("-" * 80)
    print("Scraping completed!")
    print(f"Total time: {total_time:.2f} seconds")
    print(f"Pages scraped: {pages_scraped}")
    print(f"Total URLs found: {urls_found}")
    print(f"Unique URLs visited: {len(visited_urls)}")
    print(f"URLs remaining in queue: {len(to_visit_urls)}")
    
    return {
        'start_url': start_url,
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
    
    if len(sys.argv) < 2:
        print("Usage: python scraper.py <start_url> [max_pages]")
        print("Example: python scraper.py https://example.com 50")
        sys.exit(1)
    
    start_url = sys.argv[1]
    max_pages = int(sys.argv[2]) if len(sys.argv) > 2 else 100
    
    # Validate URL
    if not start_url.startswith(('http://', 'https://')):
        print("Error: URL must start with http:// or https://")
        sys.exit(1)
    
    # Run the scraper
    results = scrape(start_url, max_pages)
    
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

if __name__ == "__main__":
    main()
