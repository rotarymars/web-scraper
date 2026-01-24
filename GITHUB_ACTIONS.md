# GitHub Actions Workflow Documentation

## Overview

The Wikipedia scraper is integrated with GitHub Actions to run automatically every 2 hours, continuously building a dataset of Wikipedia URLs.

## Workflow Details

### Schedule
- **Cron Schedule**: `0 */2 * * *` (runs at minute 0 of every 2nd hour)
- **Times**: 00:00, 02:00, 04:00, 06:00, 08:00, 10:00, 12:00, 14:00, 16:00, 18:00, 20:00, 22:00 UTC
- **Frequency**: 12 times per day

### Workflow Steps

1. **Checkout Repository**
   - Uses `actions/checkout@v4`
   - Fetches the full repository history

2. **Setup Python**
   - Uses `actions/setup-python@v5`
   - Installs latest Python 3.x

3. **Run Scraper**
   - Attempts to resume from saved state: `python scrape_wikipedia.py --resume 50`
   - If no state exists, starts fresh: `python scrape_wikipedia.py 50`
   - Scrapes 50 pages per run to stay within CI time limits

4. **Commit Results**
   - Configures Git with bot credentials
   - Commits `scraper_state.json` and `scraper_results.txt`
   - Pushes changes back to the repository
   - Uses `[skip ci]` to prevent triggering another workflow run

## Benefits

- **Continuous Data Collection**: Builds a dataset over time without manual intervention
- **Resume Support**: Each run continues from where the previous run left off
- **Data Preservation**: All scraped data is version controlled
- **No Infrastructure Cost**: Runs entirely on GitHub's free runners
- **Manual Override**: Can be triggered manually from GitHub Actions UI

## Files Managed by CI

- **`scraper_state.json`**: Current state of the scraper (visited/queued URLs, statistics)
- **`scraper_results.txt`**: Human-readable list of all scraped URLs

## Monitoring

To monitor the scraper:
1. Go to the "Actions" tab in the GitHub repository
2. Select "Wikipedia Scraper" workflow
3. View recent runs and their logs
4. Check commit history to see when data was last updated

## Manual Triggering

To manually trigger the scraper:
1. Go to the "Actions" tab
2. Select "Wikipedia Scraper" workflow
3. Click "Run workflow" button
4. Select the branch and click "Run workflow"

## Configuration

To modify the scraper behavior, edit `.github/workflows/scraper.yml`:
- Change cron schedule for different frequency
- Adjust page limit (currently 50) per run
- Modify commit message or file paths
