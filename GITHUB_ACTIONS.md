# GitHub Actions Workflow Documentation

## Overview

`.github/workflows/scraper.yml` runs the C++ crawler continuously: each run picks
up where the previous one stopped and dispatches its own successor when it
finishes, so the crawl is effectively always going. Continuity depends on the
RocksDB URL database surviving between runs; it is carried in the GitHub Actions
cache, not in git.

## Cadence

Runs chain back to back rather than waiting for a clock. The last step of a
successful run calls `gh workflow run scraper.yml`, which starts the next one
immediately.

**`on: push` cannot do this.** A push made with `GITHUB_TOKEN` never triggers a
workflow — GitHub blocks that to prevent runaway loops. `workflow_dispatch` and
`repository_dispatch` are the only documented exceptions, which is why the chain
is built on a dispatch call rather than on the result of the push.

- **Crawl budget**: 3 hours (`--max-seconds 10800`), so a full cycle is roughly
  3¼ hours including seeding, archiving, and the cache upload
- **Job timeout**: 330 minutes, under the 6-hour hard limit for a job
- **Concurrency**: the `wikipedia-scraper` group ensures two crawls never run at
  once, which would fork the database and race on `state/scraper_state.dat`. The
  successor is dispatched before the current job exits, so it waits in the queue
  for a moment and then starts.
- **Watchdog cron**: `0 */6 * * *`. The chain only survives while runs finish
  cleanly; if one is cancelled or its runner dies it cannot dispatch a successor,
  and the chain would stop forever. The cron restarts it.

### Stopping the chain

- Run the workflow manually with **`chain` unchecked** for a one-off run.
- Disable the workflow (`gh workflow disable scraper.yml`) to stop it entirely —
  a disabled workflow rejects the dispatch, so the chain cannot restart itself.

The chain is deliberately fragile in one direction: it only continues when both
the crawl **and** the cache save succeeded. Chaining after a failed save would
send the next run back to the committed corpus and silently throw away the
crawling this run did. A run that exits successfully in under 10 minutes waits out the
remainder before dispatching, so an exhausted queue cannot spin the loop.

## Where state lives

| Data | Size | Where it lives |
| --- | --- | --- |
| `state/url_state_db/` (RocksDB) | GBs | Actions cache (`url-state-v1-<run_id>`) |
| `state/scraper_state_visited_<i>.txt` | ~11 MB in git | Committed, refreshed daily |
| `state/scraper_state_to_visit_0.txt` | ~5 MB in git | Committed, refreshed daily |
| `state/scraper_state.dat` | ~120 B | Committed every run |
| `scraper_results.txt` | ~260 B | Committed every run |

The RocksDB database itself is **not** committed — it was 1.13 GB in February
2026 and keeps growing. What is committed is the same information as plain
sorted text, which is drastically cheaper: sorted URLs share long prefixes and
compress about 80:1, so 894 MB of crawled URLs occupy ~11 MB of git.

### Why the frontier is capped

The full queue is 34.2M URLs (~342 MB in git) and is pure working state, so it
stays in the cache. Only the first 500,000 URLs are committed.

That cap is not merely a size trade-off — without *some* frontier the crawl
cannot restart at all. Every URL in `visited` is `COMPLETED`, so a recovery that
re-seeded only the start URL would find it already visited, find every link off
it already visited, drain the queue, and stall. The committed slice gives a
recovered crawl real work to do; crawling regrows the rest.

### Refresh cadence

The corpus is rewritten wholesale, so committing it every run costs roughly
5 GB/year of history versus ~1.8 GB/year once a day. `CORPUS_MAX_AGE_SECONDS`
(86400) controls this. A day-old corpus only ever costs re-crawl time, and only
after a cache loss.

Cache entries use a unique per-run key (`url-state-v1-${{ github.run_id }}`) so
the save at the end of a run never collides, and `restore-keys: url-state-v1-`
picks up the newest previous entry. After a successful save, older entries are
deleted so the multi-GB archives do not churn against the 10 GB per-repository
cache quota.

### Cache misses and recovery

Actions caches are evicted after 7 days without access. When the cache is gone,
the scraper rebuilds its database from the committed corpus automatically — the
chunk filenames are exactly the ones its existing import path scans for, so
recovery needs no extra code. Rebuilding 747k visited + 500k frontier URLs takes
about 30 seconds.

Note that the scraper **deletes** the chunks once it has imported them. The
workflow therefore only stages the corpus paths on a run that actually
re-exported them, and restores them otherwise; staging them unconditionally
would commit their deletion whenever a crawl failed after importing.

If neither cache nor corpus exists, `--resume` with a surviving
`state/scraper_state.dat` re-seeds the start URL and crawls onward, preserving
the cumulative totals.

## Workflow steps

1. **Checkout** — `fetch-depth: 1`; nothing in the job needs history.
2. **Free up runner disk space** — removes preinstalled toolchains to make room
   for the database, its archive, and RocksDB compaction scratch space.
3. **Install dependencies** — `libcurl4-openssl-dev`, `librocksdb-dev`, `zstd`.
4. **Build** — the scraper, the URL state manager test (which is run), and
   `extract_urls` as a compile check.
5. **Restore / unpack URL database** — from the Actions cache; on a miss the
   scraper rebuilds from the committed corpus.
6. **Run scraper** — `--resume` when `state/scraper_state.dat` exists, otherwise
   a fresh `--wikipedia` crawl.
7. **Pack / save URL database** — runs on `always()`, so a crawl that fails or
   times out still persists its progress. RocksDB writes with `sync=true`, so the
   live database recovers from its WAL on the next open.
8. **Prune superseded caches** — gated on the save succeeding, so a failed save
   can never delete the last good database.
9. **Commit and push results** — only the two small text files, with rebase and
   retry so a concurrent push does not lose the run.
10. **Chain the next run** — dispatches the successor, subject to the guards
    described under [Cadence](#cadence).

## Manual triggering

From the Actions tab, "Wikipedia Scraper" → "Run workflow". Inputs:

| Input | Default | Meaning |
| --- | --- | --- |
| `max_pages` | `100000` | Pages to scrape in this run |
| `workers` | `5` | Parallel fetch workers |
| `max_seconds` | `10800` | Wall-clock budget for the crawl |
| `reset` | `false` | Discard the cached database and cumulative counters |
| `chain` | `true` | Dispatch the next run on success; uncheck for a one-off run |

## Relationship to the VPS crawler

`web-scraper.server` provisions a VPS that runs the same crawler continuously
under systemd and pushes `Auto-update:` commits. It holds its own RocksDB, which
is gitignored and therefore never shared with CI. If both run at once they crawl
independently and overwrite each other's `state/scraper_state.dat`; the totals in
that file then reflect whichever pushed last, not the combined crawl. Run one or
the other unless you intend that.
