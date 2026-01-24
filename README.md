# web-scraper
A web scraping repository that recursively crawls websites and extracts URLs using regex.

## Features

- **Iterative URL Processing**: Uses two sets (visited and to-visit) instead of recursive functions
- **Regex-based URL Extraction**: Extracts URLs from HTML content using regular expressions
- **Time Monitoring**: Automatically stops after 6 hours of execution
- **State Persistence**: Saves progress to file and can resume from where it left off
- **Progress Tracking**: Real-time statistics on scraped pages and found URLs
- **No External Dependencies**: Uses only Python standard library

## Requirements

- Python 3.6 or higher
- No external packages required (uses standard library only)

## Usage

### Quick Start with Wikipedia

The easiest way to start scraping is with the Wikipedia scraper:

```bash
# Start scraping Wikipedia (default: 100 pages)
python scrape_wikipedia.py

# Scrape with custom page limit
python scrape_wikipedia.py 50

# Resume from saved state
python scrape_wikipedia.py --resume
```

### General Usage

For any website:

```bash
# Basic usage
python scraper.py <start_url> [max_pages] [--resume]
```

Examples:
```bash
# Scrape example.com with default limit of 100 pages
python scraper.py https://example.com

# Scrape with custom page limit
python scraper.py https://example.com 50

# Resume from saved state
python scraper.py https://example.com 200 --resume

# Scrape a different site
python scraper.py https://www.python.org 200
```

## State Persistence

The scraper automatically saves its state to `scraper_state.json` every 10 pages and at the end of scraping. This allows you to:

- **Resume interrupted scraping**: If the scraper stops (time limit, crash, manual stop), you can resume from where it left off
- **Continue across sessions**: Stop scraping and resume later without losing progress

The state file contains:
- Start URL
- Set of visited URLs
- Queue of URLs to visit
- Statistics (pages scraped, URLs found, elapsed time)

To resume scraping, simply add the `--resume` flag when running the scraper.

## How It Works

1. **Start with a single URL**: The scraper begins with one URL provided by the user
2. **Fetch content**: Downloads the HTML content from the URL
3. **Extract URLs**: Uses regex patterns to find all URLs in the content
4. **Iterative processing**: 
   - Maintains two sets: `visited_urls` (already processed) and `to_visit_urls` (queue)
   - Continuously processes URLs from the queue
   - Adds newly found URLs to the queue
   - Skips already visited URLs
5. **Time monitoring**: Checks elapsed time before each URL and stops if execution exceeds 6 hours
6. **State persistence**: Saves progress every 10 pages and at the end
7. **Results**: Saves all visited URLs and statistics to `scraper_results.txt`

## Output

The scraper provides:
- Real-time console output with progress
- Final statistics (pages scraped, URLs found, execution time)
- Results file (`scraper_results.txt`) with all visited URLs
- State file (`scraper_state.json`) for resuming

## Files

- **`scraper.py`**: Main scraper module with state persistence
- **`scrape_wikipedia.py`**: Convenience script for scraping Wikipedia
- **`test_scraper.py`**: Unit tests

## Automated Scraping with GitHub Actions

This repository includes a GitHub Actions workflow that automatically scrapes Wikipedia every 2 hours.

### How it works

- **Schedule**: Runs every 2 hours via cron schedule (`0 */2 * * *`)
- **Manual trigger**: Can also be triggered manually via GitHub Actions UI
- **Resume support**: Automatically resumes from saved state
- **Data persistence**: Commits and pushes `scraper_state.json` and `scraper_results.txt` after each run
- **Page limit**: Scrapes 50 pages per run to avoid timeout

### Workflow file

The workflow is defined in `.github/workflows/scraper.yml` and:
1. Checks out the repository
2. Sets up Python
3. Runs the scraper with `--resume` flag (or starts fresh if no state exists)
4. Commits and pushes the results back to the repository

### Viewing the results

- Check `scraper_state.json` for the current scraper state
- Check `scraper_results.txt` for the complete list of scraped URLs
- View workflow runs in the "Actions" tab on GitHub

## Limitations

- Maximum execution time: 6 hours
- Default maximum pages: 100 (configurable)
- Timeout per request: 10 seconds
- Only follows http:// and https:// URLs
