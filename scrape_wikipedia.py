#!/usr/bin/env python3
"""
Wikipedia Scraper - Start scraping from Wikipedia main page
"""
import sys
from scraper import scrape, STATE_FILE

# Wikipedia main page URL
WIKIPEDIA_URL = "https://en.wikipedia.org/wiki/Main_Page"

def main():
    """Run the scraper starting from Wikipedia."""
    print("=" * 80)
    print("Wikipedia Web Scraper")
    print("=" * 80)
    print()
    
    # Parse command line arguments
    max_pages = 100
    resume = False
    
    if len(sys.argv) > 1:
        for arg in sys.argv[1:]:
            if arg == '--resume':
                resume = True
            elif arg.isdigit():
                max_pages = int(arg)
            elif arg in ['-h', '--help']:
                print("Usage: python scrape_wikipedia.py [max_pages] [--resume]")
                print()
                print("Arguments:")
                print("  max_pages   Maximum number of pages to scrape (default: 100)")
                print("  --resume    Resume from saved state")
                print()
                print("Examples:")
                print("  python scrape_wikipedia.py")
                print("  python scrape_wikipedia.py 50")
                print("  python scrape_wikipedia.py 200 --resume")
                sys.exit(0)
    
    if resume:
        print("Attempting to resume from saved state...")
    else:
        print(f"Starting fresh scrape from: {WIKIPEDIA_URL}")
    
    print()
    
    # Run the scraper
    results = scrape(WIKIPEDIA_URL, max_pages=max_pages, resume=resume)
    
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
    print(f"Results saved to: scraper_results.txt")
    print(f"State saved to: {STATE_FILE}")
    print()
    print(f"To resume scraping, run: python scrape_wikipedia.py {max_pages} --resume")
    print("=" * 80)

if __name__ == "__main__":
    main()
