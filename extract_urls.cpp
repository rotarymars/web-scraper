/*
 * extract_urls - Dump URLs from the RocksDB state database as plain text
 *
 * Prerequisites (Ubuntu/Debian):
 *   sudo apt-get install librocksdb-dev
 *
 * Build:
 *   g++ -std=c++23 -O2 -Wall extract_urls.cpp -lrocksdb -o extract_urls
 *
 * Usage:
 *   ./extract_urls [--visited] [--to-visit] [--all] [--output FILE] [--db PATH]
 *
 *   --visited      Output visited (COMPLETED) URLs  [default]
 *   --to-visit     Output queued (DISCOVERED) URLs
 *   --all          Output both visited and queued URLs
 *   --output FILE  Write to FILE instead of stdout
 *   --db PATH      RocksDB directory (default: state/url_state_db)
 *
 * One URL per line is written to the output.  The output is NOT sorted;
 * pipe through  | sort  if a sorted list is needed.
 *
 * Tip – get a plain URL list from the committed text chunks without RocksDB:
 *   cat state/scraper_state_visited_*.txt | sort -u > visited_urls.txt
 */

#include "url_state_manager.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace fs = std::filesystem;

static void usage(const char* prog) {
    std::cout <<
        "Usage: " << prog << " [OPTIONS]\n\n"
        "Options:\n"
        "  --visited      Output visited (COMPLETED) URLs  [default]\n"
        "  --to-visit     Output queued (DISCOVERED) URLs\n"
        "  --all          Output both sets\n"
        "  --output FILE  Write to FILE instead of stdout\n"
        "  --db PATH      RocksDB directory (default: state/url_state_db)\n"
        "  --help, -h     Show this message\n\n"
        "Tip: to extract from the plain-text chunk files without RocksDB:\n"
        "  cat state/scraper_state_visited_*.txt | sort -u > visited_urls.txt\n";
}

int main(int argc, char* argv[]) {
    bool doVisited  = false;
    bool doToVisit  = false;
    std::string outputFile;
    std::string dbPath = "state/url_state_db";

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--visited") {
            doVisited = true;
        } else if (a == "--to-visit") {
            doToVisit = true;
        } else if (a == "--all") {
            doVisited = doToVisit = true;
        } else if ((a == "--output" || a == "-o") && i + 1 < argc) {
            outputFile = argv[++i];
        } else if (a == "--db" && i + 1 < argc) {
            dbPath = argv[++i];
        } else if (a == "--help" || a == "-h") {
            usage(argv[0]);
            return 0;
        } else {
            std::cerr << "Unknown option: " << a << '\n';
            usage(argv[0]);
            return 1;
        }
    }

    // Default: show visited URLs
    if (!doVisited && !doToVisit) doVisited = true;

    if (!fs::exists(fs::path(dbPath) / "CURRENT")) {
        std::cerr << "Error: RocksDB not found at '" << dbPath << "'\n"
                  << "Run the scraper first, or specify --db PATH.\n\n"
                  << "Tip: you can also read the committed text chunks directly:\n"
                  << "  cat state/scraper_state_visited_*.txt\n";
        return 1;
    }

    // Open database read-only (no auto-verify to avoid WAL recovery overhead)
    UrlStateManager mgr(dbPath, /*autoVerify=*/false);

    // Choose output stream
    std::ostream* out = &std::cout;
    std::ofstream fout;
    if (!outputFile.empty()) {
        fout.open(outputFile);
        if (!fout) {
            std::cerr << "Error: cannot open output file '" << outputFile << "'\n";
            return 1;
        }
        out = &fout;
    }

    if (doVisited) {
        mgr.forEachUrlByState(UrlState::COMPLETED, [&](const std::string& url) {
            *out << url << '\n';
        });
    }
    if (doToVisit) {
        mgr.forEachUrlByState(UrlState::DISCOVERED, [&](const std::string& url) {
            *out << url << '\n';
        });
    }

    return 0;
}
