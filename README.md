# web-scraper
A web scraping repository that recursively crawls websites and extracts URLs using regex.

It is meant to crawl all URLs in the world.

## Storage Optimization

The scraper uses ZIP compression for storing URL state files to optimize storage space:

- **Compression Ratio**: Achieves ~68% space reduction compared to plain JSON
- **File Size Limit**: All compressed files stay well under GitHub's 100MB per-file limit
- **Backward Compatible**: Can still read old JSON format files for migration
- **Automatic**: Compression happens automatically when saving state

### Space Savings

With ZIP compression enabled:
- Original JSON size: ~44MB per chunk file
- Compressed ZIP size: ~14MB per chunk file
- Space saved: ~67.8% reduction

This helps keep the repository size manageable and avoids hitting GitHub's 10GiB repository size limit.

Also, URLs that exceed the length of `8192` characters are now truncated to prevent stack overflow errors during processing(because it uses regex for URL validation).