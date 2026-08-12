/*
 * extract_urls - Dump URLs from the zip-chunk store as plain text
 *
 * Prerequisites (Ubuntu/Debian):
 *   sudo apt-get install libzip-dev
 *
 * Build:
 *   g++ -std=c++23 -O2 -Wall extract_urls.cpp -lzip -o extract_urls
 *
 * Usage:
 *   ./extract_urls [--visited] [--to-visit] [--all] [--clean] [--output FILE]
 *
 * One URL per line is written to the output.  Pipe through  | sort -u  if you
 * need it sorted; the store is already deduplicated, so that is rarely needed.
 *
 *   ./extract_urls --visited | wc -l              # count crawled URLs
 *   ./extract_urls --all --clean -o all.txt       # dump everything, filtered
 *
 * ── Why --clean exists ───────────────────────────────────────────────────────
 *
 * The store is a faithful record of what the crawler resolved, which is not the
 * same as a list of URLs worth having.  Two kinds of entry are not URLs at all:
 *
 *   1. Template placeholders that resolveUrl() turned into absolute URLs:
 *      "http://$1", "http://%%sponsor_url%%", "http://#".  These were never
 *      links; they are unsubstituted markup from the source page.
 *
 *   2. Fragments.  A stored URL containing a raw newline is written across two
 *      lines and reads back as two records, the second of which is a tail like
 *      "&noscript=1".  Rejecting anything without a valid scheme+host drops it.
 *
 * --clean rejects both on structure alone, so it never drops a well-formed URL.
 * Without it the output is byte-for-byte what the store holds.
 *
 * ── Long URLs are NOT filtered by default ────────────────────────────────────
 *
 * Some entries are enormous: a URL like
 *   accounts.google.com/AccountChooser?continue=...%2525253Dtrue...
 * re-escapes its own query string every time it is crawled, so it grows without
 * bound.  In the visited set these ran to 8 KB and were ~8% of the lines but
 * ~65% of the bytes.
 *
 * They are still well-formed URLs, so --clean keeps them.  Discarding them
 * needs an explicit --max-length, because any threshold is a guess: there is no
 * length at which a URL stops being real, only one past which it is unlikely to
 * be.  Choosing that number is the caller's call, not this tool's.
 */

#include "progress_bar.hpp"
#include "url_store.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace fs = std::filesystem;

static void usage(const char* prog) {
    std::cout <<
        "Usage: " << prog << " [OPTIONS]\n\n"
        "Options:\n"
        "  --visited        Output crawled URLs  [default]\n"
        "  --to-visit       Output pending URLs (discovered, not yet crawled)\n"
        "  --all            Output both sets\n"
        "  --output FILE    Write to FILE instead of stdout\n"
        "  --store PATH     Store root directory (default: urls)\n"
        "  --clean          Drop entries that are not well-formed http(s) URLs\n"
        "  --max-length N   Drop URLs longer than N bytes (default: unlimited).\n"
        "                   Runaway redirect chains reach 8 KB+; nothing is cut\n"
        "                   on length unless you ask for it.\n"
        "  --help, -h       Show this message\n";
}

/// Chunk files in `dir`, ordered by their numeric name so replay is stable.
/// Mirrors UrlStore::sortedChunks, which is private to the store.
static std::vector<fs::path> sortedChunks(const fs::path& dir) {
    std::vector<fs::path> out;
    if (!fs::exists(dir)) return out;
    for (const auto& e : fs::directory_iterator(dir))
        if (e.is_regular_file() && e.path().extension() == ".zip")
            out.push_back(e.path());
    std::sort(out.begin(), out.end(), [](const fs::path& a, const fs::path& b) {
        return a.filename().string() < b.filename().string();
    });
    return out;
}

/// True when `url` parses as an http(s) URL with a plausible host.
///
/// This is deliberately stricter than the crawler's own isValidHttpUrl: the
/// crawler errs toward following anything that might work, whereas an exported
/// list is more useful when the obvious non-URLs are gone.
static bool looksReal(std::string_view url) {
    std::string_view rest;
    if (url.starts_with("https://"))     rest = url.substr(8);
    else if (url.starts_with("http://")) rest = url.substr(7);
    else return false;

    // Whitespace never survives in a real href.  Its presence means two URLs
    // were concatenated ("http:// http://www...") or markup leaked in.
    if (rest.find_first_of(" \t\r\n") != std::string_view::npos) return false;

    const std::size_t hostEnd = rest.find_first_of("/?#");
    const std::string_view host = rest.substr(0, hostEnd);
    if (host.empty()) return false;

    // Unsubstituted template syntax: $1, %%sponsor_url%%, {{var}}.
    if (host.find('$') != std::string_view::npos) return false;
    if (host.find("%%") != std::string_view::npos) return false;
    if (host.find_first_of("{}<>\"'|\\^`") != std::string_view::npos) return false;

    // An IPv6 literal is bracketed; anything else needs a dot to be routable.
    const bool bracketed = host.front() == '[';
    if (!bracketed && host.find('.') == std::string_view::npos) return false;

    // A host label cannot start or end with a dot, nor contain an empty label.
    if (!bracketed && (host.front() == '.' || host.back() == '.')) return false;
    if (host.find("..") != std::string_view::npos) return false;

    return true;
}

int main(int argc, char* argv[]) {
    // Decouple C++ streams from C stdio so large sequential writes are fast.
    std::ios_base::sync_with_stdio(false);

    bool        doVisited = false;
    bool        doToVisit = false;
    bool        clean     = false;
    std::size_t maxLength = 0;            // 0 = unlimited
    std::string outputFile;
    fs::path    storeRoot = "urls";

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--visited") {
            doVisited = true;
        } else if (a == "--to-visit") {
            doToVisit = true;
        } else if (a == "--all") {
            doVisited = doToVisit = true;
        } else if (a == "--clean") {
            clean = true;
        } else if ((a == "--output" || a == "-o") && i + 1 < argc) {
            outputFile = argv[++i];
        } else if (a == "--store" && i + 1 < argc) {
            storeRoot = argv[++i];
        } else if (a == "--max-length" && i + 1 < argc) {
            try {
                maxLength = static_cast<std::size_t>(std::stoull(argv[++i]));
            } catch (const std::exception&) {
                std::cerr << "Error: --max-length needs a number\n";
                return 1;
            }
        } else if (a == "--help" || a == "-h") {
            usage(argv[0]);
            return 0;
        } else {
            std::cerr << "Unknown option: " << a << '\n';
            usage(argv[0]);
            return 1;
        }
    }

    if (!doVisited && !doToVisit) doVisited = true;

    const fs::path visitedDir = storeRoot / "visited";
    const fs::path queueDir   = storeRoot / "queue";

    auto visitedChunks = sortedChunks(visitedDir);
    auto queueChunks   = sortedChunks(queueDir);

    if (visitedChunks.empty() && queueChunks.empty()) {
        std::cerr << "Error: no chunks found under '" << storeRoot.string() << "'\n"
                  << "       Expected " << visitedDir.string() << "/*.zip and/or "
                  << queueDir.string() << "/*.zip\n"
                  << "Run the scraper first, or pass --store PATH.\n";
        return 1;
    }

    // A 256 KB write buffer amortises syscall overhead when streaming tens of
    // millions of URLs, without using meaningful RAM (contrast: collecting the
    // queue into a vector first would cost ~5.6 GB).
    static char stdoutBuf[256 * 1024];
    static char fileBuf  [256 * 1024];

    std::ostream* out = &std::cout;
    std::ofstream fout;
    if (!outputFile.empty()) {
        fout.open(outputFile);
        if (!fout) {
            std::cerr << "Error: cannot open output file '" << outputFile << "'\n";
            return 1;
        }
        fout.rdbuf()->pubsetbuf(fileBuf, sizeof(fileBuf));
        out = &fout;
    } else {
        std::cout.rdbuf()->pubsetbuf(stdoutBuf, sizeof(stdoutBuf));
    }

    // "Pending" means present in queue/ but not in visited/ -- the store never
    // deletes a queue entry to record progress, so the visited hashes are what
    // separates the two.  Only needed when emitting pending URLs.
    HashSet64 visitedHashes;
    if (doToVisit) {
        std::cerr << "Loading visited set from " << visitedChunks.size() << " chunk(s)...\n";
        for (const auto& c : visitedChunks) {
            try {
                readZipChunk(c, [&](std::string&& url) {
                    visitedHashes.insert(hashUrl(url));
                });
            } catch (const std::exception& e) {
                std::cerr << "Warning: skipping " << c.string() << ": " << e.what() << '\n';
            }
        }
        std::cerr << "  " << visitedHashes.size() << " visited URLs loaded\n";
    }

    // Total is unknown without a pre-scan (which would mean decompressing the
    // whole store twice), so run the bar in count-only mode.
    ProgressBar pbar(0, "Extracting");
    std::size_t written = 0, skipped = 0;

    auto emit = [&](std::string&& url) {
        if (maxLength && url.size() > maxLength) { ++skipped; return; }
        if (clean && !looksReal(url))            { ++skipped; return; }
        *out << url << '\n';
        pbar.update(++written);
    };

    auto emitChunks = [&](const std::vector<fs::path>& chunks, bool skipVisited) {
        for (const auto& c : chunks) {
            try {
                readZipChunk(c, [&](std::string&& url) {
                    if (skipVisited && visitedHashes.contains(hashUrl(url))) return;
                    emit(std::move(url));
                });
            } catch (const std::exception& e) {
                // One corrupt chunk should not lose the other sixty-five.
                std::cerr << "\rWarning: skipping " << c.string() << ": " << e.what() << '\n';
            }
        }
    };

    if (doVisited) emitChunks(visitedChunks, /*skipVisited=*/false);
    if (doToVisit) emitChunks(queueChunks,   /*skipVisited=*/true);

    pbar.finish(written);
    out->flush();

    std::cerr << written << " URLs written";
    if (skipped) std::cerr << ", " << skipped << " skipped";
    std::cerr << '\n';

    return 0;
}
