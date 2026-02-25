/*
 * cleanup_urls - Remove invalid URLs from the RocksDB state database
 *
 * Every stored URL is matched against the same URL regex used by the scraper.
 * Entries that do not match — including those containing \n, \r, or other
 * control/unprintable characters — are deleted in a single WriteBatch, and
 * the updated database is sealed into a fresh checkpoint on exit.
 *
 * Prerequisites (Ubuntu/Debian):
 *   sudo apt-get install librocksdb-dev
 *
 * Build:
 *   g++ -std=c++23 -O2 -Wall cleanup_urls.cpp -lrocksdb -o cleanup_urls
 *
 * Usage:
 *   ./cleanup_urls [--db PATH] [--dry-run]
 *
 *   --db PATH    RocksDB directory  (default: state/url_state_db,
 *                falls back to state/url_state_db_checkpoint)
 *   --dry-run    Report invalid URLs without deleting them
 *   --help, -h   Show this message
 */

#include "progress_bar.hpp"
#include "url_state_manager.hpp"

#include <filesystem>
#include <iostream>
#include <regex>
#include <string>
#include <vector>

namespace fs = std::filesystem;

// Full-string URL validation regex — same character classes as RE_URL in
// scraper.cpp.  std::regex_match() implicitly anchors both ends, so the
// entire stored key must be a valid URL.
//
// No icase flag: schemes are already normalised to lowercase on storage
// (scraper.cpp::normalizeScheme), and icase + non-ASCII bytes can trigger
// UB in std::regex on some implementations.
static const std::regex RE_VALID_URL{
    R"re(https?://[^\s<>"{}|\\^`\[\]]+[^\s<>"{}|\\^`\[\].,;:!?'"\)])re",
    std::regex::optimize};

static bool isValidUrl(const std::string& url) {
    return std::regex_match(url, RE_VALID_URL);
}

static void usage(const char* prog) {
    std::cout <<
        "Usage: " << prog << " [OPTIONS]\n\n"
        "Options:\n"
        "  --db PATH    RocksDB directory (default: state/url_state_db,\n"
        "               falls back to state/url_state_db_checkpoint)\n"
        "  --dry-run    Report invalid URLs without deleting them\n"
        "  --help, -h   Show this message\n";
}

int main(int argc, char* argv[]) {
    std::ios_base::sync_with_stdio(false);

    std::string dbPath = "state/url_state_db";
    bool        dryRun = false;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--dry-run") {
            dryRun = true;
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

    // Fall back to the committed checkpoint if the live DB is absent.
    if (!fs::exists(fs::path(dbPath) / "CURRENT")) {
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

    // Open without auto-verify to skip the full checksum scan on startup.
    UrlStateManager mgr(dbPath, /*autoVerify=*/false);

    // Pre-count for the progress bar — only when stderr is a TTY so that
    // automated pipelines skip the extra O(N) scan and run at full speed.
    std::size_t total = 0;
    if (isatty(STDERR_FILENO))
        mgr.forEachUrl([&](const std::string&) { ++total; });

    ProgressBar pbar(total, "Scanning");
    std::size_t scanned = 0;
    std::vector<std::string> invalid;

    mgr.forEachUrl([&](const std::string& url) {
        if (!isValidUrl(url))
            invalid.push_back(url);
        pbar.update(++scanned);
    });
    pbar.finish(scanned);

    std::cout << "Scanned : " << scanned        << '\n'
              << "Invalid : " << invalid.size() << '\n';

    if (invalid.empty()) {
        std::cout << "Nothing to remove.\n";
        return 0;
    }

    if (dryRun) {
        std::cout << "(dry-run) Would remove " << invalid.size() << " URL(s):\n";
        for (const auto& u : invalid)
            std::cout << "  " << u << '\n';
        return 0;
    }

    mgr.bulkDelete(invalid);
    std::cout << "Removed : " << invalid.size() << " invalid URL(s)\n";
    // UrlStateManager destructor calls seal(), which writes the updated
    // checkpoint so the cleaned state is persisted for the next run.
    return 0;
}
