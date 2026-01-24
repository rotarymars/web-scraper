# web-scraper
A web scraping repository that recursively crawls websites and extracts URLs using regex.

## Features

- **Iterative URL Processing**: Uses two sets (visited and to-visit) instead of recursive functions
- **Regex-based URL Extraction**: Extracts URLs from HTML content using regular expressions
- **Time Monitoring**: Automatically stops after 1 hour of execution
- **Progress Tracking**: Real-time statistics on scraped pages and found URLs
- **No External Dependencies**: Uses only Python standard library

## Requirements

- Python 3.6 or higher
- No external packages required (uses standard library only)

## Usage

Basic usage:
```bash
python scraper.py <start_url> [max_pages]
```

Examples:
```bash
# Scrape example.com with default limit of 100 pages
python scraper.py https://example.com

# Scrape with custom page limit
python scraper.py https://example.com 50

# Scrape a different site
python scraper.py https://www.python.org 200
```

## How It Works

1. **Start with a single URL**: The scraper begins with one URL provided by the user
2. **Fetch content**: Downloads the HTML content from the URL
3. **Extract URLs**: Uses regex patterns to find all URLs in the content
4. **Iterative processing**: 
   - Maintains two sets: `visited_urls` (already processed) and `to_visit_urls` (queue)
   - Continuously processes URLs from the queue
   - Adds newly found URLs to the queue
   - Skips already visited URLs
5. **Time monitoring**: Checks elapsed time before each URL and stops if execution exceeds 1 hour
6. **Results**: Saves all visited URLs and statistics to `scraper_results.txt`

## Output

The scraper provides:
- Real-time console output with progress
- Final statistics (pages scraped, URLs found, execution time)
- Results file (`scraper_results.txt`) with all visited URLs

## Limitations

- Maximum execution time: 1 hour
- Default maximum pages: 100 (configurable)
- Timeout per request: 10 seconds
- Only follows http:// and https:// URLs
