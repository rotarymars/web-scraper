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
 *                  Falls back to state/url_state_db_checkpoint automatically.
 *
 * One URL per line is written to the output.  Pipe through  | sort  if needed.
 *
 * After a git pull the checkpoint is available immediately:
 *   ./extract_urls --visited | wc -l       # count visited URLs
 *   ./extract_urls --all --output all.txt  # dump everything
 */

#include "progress_bar.hpp"
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
        "  --db PATH      RocksDB directory (default: state/url_state_db,\n"
        "                 falls back to state/url_state_db_checkpoint)\n"
        "  --help, -h     Show this message\n";
}

int main(int argc, char* argv[]) {
    // Decouple C++ streams from C stdio so large sequential writes are fast.
    std::ios_base::sync_with_stdio(false);

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
        // Try the checkpoint directory (the live DB path is in .gitignore;
        // the checkpoint is what's actually committed to git).
        std::string cpPath = dbPath + "_checkpoint";
        if (fs::exists(fs::path(cpPath) / "CURRENT")) {
            dbPath = cpPath;
        } else {
            std::cerr << "Error: RocksDB not found at '" << dbPath << "'\n"
                      << "       (also checked '" << cpPath << "')\n"
                      << "Run the scraper first, or specify --db PATH.\n";
            return 1;
        }
    }

    // Open database (no auto-verify to skip the full checksum scan on open).
    UrlStateManager mgr(dbPath, /*autoVerify=*/false);

    // Set up output stream.
    // A 256 KB write buffer amortises syscall overhead when streaming millions
    // of URLs without using meaningful RAM (contrast: loading all URLs into a
    // vector would cost ~3 GB for a 35 M-URL database).
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

    // Count total URLs for the progress bar — only when stderr is a TTY so
    // that automated scripts (where the bar would be invisible anyway) skip
    // the extra O(N) scan and run at full speed.
    std::size_t total = 0;
    if (isatty(STDERR_FILENO)) {
        if (doVisited)  total += mgr.countByState(UrlState::COMPLETED);
        if (doToVisit)  total += mgr.countByState(UrlState::DISCOVERED);
    }

    ProgressBar pbar(total, "Extracting");
    std::size_t written = 0;

    auto emit = [&](UrlState st) {
        mgr.forEachUrlByState(st, [&](const std::string& url) {
            *out << url << '\n';
            pbar.update(++written);
        });
    };

    if (doVisited)  emit(UrlState::COMPLETED);
    if (doToVisit)  emit(UrlState::DISCOVERED);

    pbar.finish(written);
    return 0;
}
