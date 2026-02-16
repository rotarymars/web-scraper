# Quick Start Guide

## Wikipedia Scraper

The easiest way to start using the web scraper is with Wikipedia:

### Start Scraping Wikipedia

```bash
# Scrape 100 pages from Wikipedia (default)
python scraper.py --wikipedia

# Scrape 50 pages
python scraper.py --wikipedia 50

# Scrape 200 pages
python scraper.py --wikipedia 200

# Scrape with 10 parallel workers for faster performance
python scraper.py --wikipedia 200 --workers 10
```

### Resume Scraping

If the scraper stops (time limit, manual interrupt, or any other reason), you can resume from where it left off:

```bash
# Resume from saved state (URL not needed - loaded from state)
python scraper.py --resume

# Resume with Wikipedia and set a new page limit
python scraper.py --wikipedia --resume 500
```

## Output Files

The scraper creates two important files:

1. **`state/scraper_state.json`** - Contains the current state of the scraper:
   - Which URLs have been visited
   - Which URLs are queued to visit
   - Statistics (pages scraped, URLs found, elapsed time)
   - Use this file to resume scraping

2. **`scraper_results.txt`** - Contains the final results:
   - List of all visited URLs
   - Statistics and summary

## How State Persistence Works

- State is automatically saved every 10 pages
- State is saved at the end of scraping
- If scraping is interrupted, the state file preserves your progress
- Use `--resume` flag to continue from the saved state
- The scraper will skip already-visited URLs and continue with the queue

## Example Workflow

```bash
# 1. Start scraping Wikipedia with a 6-hour limit
python scraper.py --wikipedia 1000

# ... scraper runs and might stop after 6 hours ...
# ... say it scraped 400 pages before stopping ...

# 2. Resume scraping to continue
python scraper.py --wikipedia 1000 --resume

# ... scraper continues from page 401 ...
```

## General Usage (Any Website)

For websites other than Wikipedia:

```bash
# Start scraping
python scraper.py https://example.com 100

# Resume scraping (no URL needed)
python scraper.py --resume

# Resume with new page limit
python scraper.py --resume 200
```
