# GitHub Actions Workflow Documentation

## Overview

`.github/workflows/scraper.yml` runs the C++ crawler on a schedule, so that each
run continues the crawl the previous run left off. Continuity depends on the
RocksDB URL database surviving between runs; it is carried in the GitHub Actions
cache, not in git.

## Schedule

- **Cron**: `0 */4 * * *` — every 4 hours (00:00, 04:00, … UTC)
- **Crawl budget**: 3 hours (`--max-seconds 10800`), leaving room within the
  4-hour gap to archive and upload the database
- **Job timeout**: 330 minutes, under the 6-hour hard limit for a job
- **Concurrency**: the `wikipedia-scraper` group ensures two crawls never run at
  once, which would fork the database and race on `state/scraper_state.dat`

## Where state lives

| Data | Size | Where it lives |
| --- | --- | --- |
| `state/url_state_db/` (RocksDB) | GBs | Actions cache (`url-state-v1-<run_id>`) |
| `state/scraper_state.dat` | ~120 B | Committed to git |
| `scraper_results.txt` | ~260 B | Committed to git |

The database is **not** committed. It was 1.13 GB back in February 2026 and grows
with every run; committing it would balloon a repository whose history is already
~7.6 GB.

Cache entries use a unique per-run key (`url-state-v1-${{ github.run_id }}`) so
the save at the end of a run never collides, and `restore-keys: url-state-v1-`
picks up the newest previous entry. After a successful save, older entries are
deleted so the multi-GB archives do not churn against the 10 GB per-repository
cache quota.

### Cache misses

Actions caches are evicted after 7 days without access, and on eviction the crawl
loses its URL frontier. When that happens the run still resumes: `--resume` with
a surviving `state/scraper_state.dat` but no database re-seeds the start URL and
carries on, preserving the cumulative page and URL totals rather than resetting
them to zero.

## Workflow steps

1. **Checkout** — `fetch-depth: 1`. A shallow clone is essential: the history is
   ~7.6 GB of legacy state dumps while the tip tree is a handful of source files.
2. **Free up runner disk space** — removes preinstalled toolchains to make room
   for the database, its archive, and RocksDB compaction scratch space.
3. **Install dependencies** — `libcurl4-openssl-dev`, `librocksdb-dev`, `zstd`.
4. **Build** — the scraper, the URL state manager test (which is run), and
   `extract_urls` as a compile check.
5. **Restore / unpack URL database** — from the Actions cache.
6. **Run scraper** — `--resume` when `state/scraper_state.dat` exists, otherwise
   a fresh `--wikipedia` crawl.
7. **Pack / save URL database** — runs on `always()`, so a crawl that fails or
   times out still persists its progress. RocksDB writes with `sync=true`, so the
   live database recovers from its WAL on the next open.
8. **Prune superseded caches** — gated on the save succeeding, so a failed save
   can never delete the last good database.
9. **Commit and push results** — only the two small text files, with rebase and
   retry so a concurrent push does not lose the run.

## Manual triggering

From the Actions tab, "Wikipedia Scraper" → "Run workflow". Inputs:

| Input | Default | Meaning |
| --- | --- | --- |
| `max_pages` | `100000` | Pages to scrape in this run |
| `workers` | `5` | Parallel fetch workers |
| `max_seconds` | `10800` | Wall-clock budget for the crawl |
| `reset` | `false` | Discard the cached database and cumulative counters |

## Relationship to the VPS crawler

`web-scraper.server` provisions a VPS that runs the same crawler continuously
under systemd and pushes `Auto-update:` commits. It holds its own RocksDB, which
is gitignored and therefore never shared with CI. If both run at once they crawl
independently and overwrite each other's `state/scraper_state.dat`; the totals in
that file then reflect whichever pushed last, not the combined crawl. Run one or
the other unless you intend that.
