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
 * same as a list of URLs worth having.  Three kinds of entry are junk:
 *
 *   1. Unbounded redirect chains.  A URL like
 *        accounts.google.com/AccountChooser?continue=...%2525253Dtrue...
 *      re-escapes its own query string every time it is crawled, so it grows
 *      without bound.  In the visited set these were ~8% of lines but ~65% of
 *      the bytes.  --max-length is what actually cuts them.
 *
 *   2. Template placeholders that resolveUrl() turned into absolute URLs:
 *      "http://$1", "http://%%sponsor_url%%", "http://#".  These were never
 *      links; they are unsubstituted markup from the source page.
 *
 *   3. Fragments.  A stored URL containing a raw newline is written across two
 *      lines and reads back as two records, the second of which is a tail like
 *      "&noscript=1".  Rejecting anything without a valid scheme+host drops it.
 *
 * Without --clean the output is byte-for-byte what the store holds.
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

/// Length beyond which a URL is assumed to be a runaway redirect chain rather
/// than a real address.  Applied only under --clean; override with
/// --max-length.  The longest legitimate-looking URL observed in the store is
/// well under 1 KB, while the runaway ones run to 8 KB+.
static constexpr std::size_t CLEAN_MAX_LENGTH = 2000;

static void usage(const char* prog) {
    std::cout <<
        "Usage: " << prog << " [OPTIONS]\n\n"
        "Options:\n"
        "  --visited        Output crawled URLs  [default]\n"
        "  --to-visit       Output pending URLs (discovered, not yet crawled)\n"
        "  --all            Output both sets\n"
        "  --output FILE    Write to FILE instead of stdout\n"
        "  --store PATH     Store root directory (default: urls)\n"
        "  --clean          Drop malformed entries and runaway redirect chains\n"
        "  --max-length N   Drop URLs longer than N bytes\n"
        "                   (default: unlimited, or " << CLEAN_MAX_LENGTH << " under --clean)\n"
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
    bool        maxLengthSet = false;
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
                maxLengthSet = true;
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
    if (clean && !maxLengthSet)   maxLength = CLEAN_MAX_LENGTH;

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
