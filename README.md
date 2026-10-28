# web-scraper

A breadth-first web crawler that recursively fetches pages, extracts every link
it finds, and keeps going. It is meant to crawl all URLs in the world.

There is no server and no database. **The git repository is the crawl state**,
and GitHub Actions is the compute: each workflow run loads the store from the
checkout, crawls for a few hours, commits the URLs it discovered, and dispatches
its own successor.

## Build

```bash
sudo apt-get install libcurl4-openssl-dev libzip-dev
g++ -std=c++23 -O2 -Wall -pthread scraper.cpp -lcurl -lzip -o scraper
g++ -std=c++23 -O2 -Wall extract_urls.cpp -lzip -o extract_urls
```

## Run

```bash
./scraper --wikipedia 50            # crawl 50 pages from the Wikipedia main page
./scraper --resume                  # continue from the existing store
./scraper --resume 100000 --workers 10 --max-seconds 10800
./scraper https://example.com 100   # crawl any other seed
```

| Flag | Default | Meaning |
| --- | --- | --- |
| `url` / `--wikipedia`, `-w` | — | Seed URL, or the Wikipedia main page |
| `max_pages` | `100` | Pages to fetch this run (bare number) |
| `--resume` | off | Continue from the saved store; no seed URL needed |
| `--workers N` | `5` | Parallel fetch workers |
| `--max-seconds N` | `12600` | Wall-clock budget for this run (3.5 h) |

## How state is stored

All crawl state lives in the repository as **immutable zipped chunks**:

```
urls/visited/<seq>.zip   every URL that has been crawled
urls/queue/<seq>.zip     every URL that has been discovered
```

Each zip holds a single `urls.txt`, one URL per line. A URL is *pending* when it
appears in `queue/` but not in `visited/`; nothing is ever deleted to represent
progress.

Chunks are never modified or rewritten — a run only adds new ones. That is what
keeps the repository affordable: git stores each blob exactly once, so history
grows by the genuinely-new data and nothing else. Rewriting chunks instead would
cost roughly 8.5x more, because git cannot delta already-compressed data. The CI
workflow enforces this with a step that fails the run if any tracked chunk was
modified or deleted.

Deduplication is by 64-bit hash rather than by storing the strings, so ~35M URLs
cost about 600 MB of RAM instead of ~6 GB. At that size the chance of any
collision is about 3e-5, which would silently skip one URL — an acceptable trade
for a crawler. See `url_store.hpp` for the details.

`state/scraper_state.dat` holds only the cumulative counters and the seed URL;
`scraper_results.txt` is a human-readable summary written at the end of a run.

URLs longer than 8192 characters are truncated, since link extraction is
regex-shaped and very long inputs risk a stack overflow.

## Reading the store

`extract_urls` dumps the store back to plain text, one URL per line:

```bash
./extract_urls --visited | wc -l                # count crawled URLs
./extract_urls --all --clean -o all.txt         # dump everything, filtered
```

`--clean` rejects entries that were never really URLs — unsubstituted template
placeholders like `http://$1`, and fragments left by a stored URL containing a
raw newline. It rejects on structure alone, so it never drops a well-formed URL.
Long URLs are *not* filtered by default; use `--max-length` if you want that.
The header comment in `extract_urls.cpp` explains why.

## Continuous crawling

`.github/workflows/scraper.yml` runs the crawl. Each run dispatches the next one
as its final step, so the crawl is effectively continuous; the 6-hourly cron is
only a watchdog that restarts the chain if a run dies before it can hand off. A
concurrency group ensures two crawls never run at once.

To stop it: run the workflow manually with `chain` unchecked for a one-off run,
or `gh workflow disable scraper.yml` to stop it entirely.
