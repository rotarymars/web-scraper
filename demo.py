#!/usr/bin/env python3
"""
Demo script to test the web scraper functionality with mock data
"""
import time
from scraper import extract_urls

def demo_url_extraction():
    """Demonstrate URL extraction functionality"""
    print("=" * 80)
    print("Web Scraper Demo - URL Extraction")
    print("=" * 80)
    
    # Sample HTML content
    sample_html = """
    <!DOCTYPE html>
    <html>
    <head>
        <title>Sample Page</title>
        <link rel="stylesheet" href="/css/style.css">
        <script src="https://cdn.example.com/jquery.js"></script>
    </head>
    <body>
        <h1>Welcome to the Sample Page</h1>
        <nav>
            <a href="https://example.com/page1">Page 1</a>
            <a href="https://example.com/page2">Page 2</a>
            <a href="/about">About</a>
            <a href="/contact">Contact</a>
        </nav>
        <div>
            <p>Check out https://github.com for more information.</p>
            <img src="https://example.com/images/logo.png" alt="Logo">
            <img src="/images/banner.jpg" alt="Banner">
        </div>
        <footer>
            <a href="mailto:test@example.com">Email Us</a>
            <a href="javascript:void(0)">JS Link</a>
            <a href="http://example.com/terms">Terms</a>
        </footer>
    </body>
    </html>
    """
    
    base_url = "https://example.com"
    print(f"\nBase URL: {base_url}")
    print(f"Extracting URLs from sample HTML...")
    
    urls = extract_urls(sample_html, base_url)
    
    print(f"\nFound {len(urls)} unique URLs:")
    print("-" * 80)
    for i, url in enumerate(sorted(urls), 1):
        print(f"{i:2d}. {url}")
    
    print("\n" + "=" * 80)
    print("Demo completed successfully!")
    print("=" * 80)

def demo_iterative_processing():
    """Demonstrate the iterative processing logic"""
    print("\n" + "=" * 80)
    print("Web Scraper Demo - Iterative Processing Logic")
    print("=" * 80)
    
    # Simulate the two-set approach
    visited_urls = set()
    to_visit_urls = {"https://example.com"}
    
    print("\nSimulating iterative URL processing...")
    print("(This demonstrates the two-set approach without actual HTTP requests)")
    print()
    
    # Mock data: URL -> list of URLs it contains
    mock_pages = {
        "https://example.com": [
            "https://example.com/about",
            "https://example.com/contact",
            "https://example.com/blog"
        ],
        "https://example.com/about": [
            "https://example.com",
            "https://example.com/team"
        ],
        "https://example.com/contact": [
            "https://example.com",
            "https://example.com/support"
        ],
        "https://example.com/blog": [
            "https://example.com",
            "https://example.com/blog/post1",
            "https://example.com/blog/post2"
        ]
    }
    
    iteration = 0
    max_iterations = 10
    start_time = time.time()
    
    while to_visit_urls and iteration < max_iterations:
        iteration += 1
        
        # Get next URL
        current_url = to_visit_urls.pop()
        
        # Skip if already visited
        if current_url in visited_urls:
            print(f"[{iteration}] Skipping (already visited): {current_url}")
            continue
        
        # Mark as visited
        visited_urls.add(current_url)
        
        print(f"[{iteration}] Processing: {current_url}")
        
        # Get URLs from this page (mock)
        found_urls = set(mock_pages.get(current_url, []))
        
        # Add new URLs to queue
        new_urls = found_urls - visited_urls
        to_visit_urls.update(new_urls)
        
        print(f"     Found {len(found_urls)} URLs ({len(new_urls)} new)")
        print(f"     Queue: {len(to_visit_urls)} | Visited: {len(visited_urls)}")
        print()
    
    elapsed = time.time() - start_time
    
    print("-" * 80)
    print(f"Simulation completed!")
    print(f"Iterations: {iteration}")
    print(f"Pages visited: {len(visited_urls)}")
    print(f"URLs remaining in queue: {len(to_visit_urls)}")
    print(f"Time: {elapsed:.4f} seconds")
    
    print("\nVisited URLs:")
    for i, url in enumerate(sorted(visited_urls), 1):
        print(f"  {i}. {url}")
    
    if to_visit_urls:
        print("\nRemaining in queue:")
        for i, url in enumerate(sorted(to_visit_urls), 1):
            print(f"  {i}. {url}")

if __name__ == "__main__":
    demo_url_extraction()
    demo_iterative_processing()
    print("\n" + "=" * 80)
    print("All demos completed! The scraper is ready to use.")
    print("Run: python scraper.py <url> [max_pages]")
    print("=" * 80)
