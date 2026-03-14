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

// Full-string URL validation regex.  std::regex_match() implicitly anchors
// both ends, so the entire stored key must be a valid URL.
//
// The host section is validated structurally:
//   • IPv6 literals in brackets            e.g. [::1], [::ffff:192.0.2.1]
//   • Dotted-quad IPv4                     e.g. 192.168.1.1
//   • DNS hostname (RFC 1123 labels)       e.g. example.com, xn--nxasmq6b.com
//     — Each label starts and ends with an alphanumeric char (or underscore
//       for internal/non-standard hosts); hyphens are allowed in the middle.
//     — Labels may NOT start or end with a hyphen, and the host may NOT
//       start with $, *, !, or any other non-alphanumeric character.
// Optional RFC 3986 userinfo (user[:pass]@) is permitted before the host.
//
// No icase flag: schemes are already normalised to lowercase on storage
// (scraper.cpp::normalizeScheme), and icase + non-ASCII bytes can trigger
// UB in std::regex on some implementations.
static const std::regex RE_VALID_URL{
    // scheme
    R"re(https?://)re"
    // optional userinfo  (user or user:pass, followed by @)
    R"re((?:[a-zA-Z0-9._~!$&'()*+,;=:%-]+@)?)re"
    // host: IPv6 literal | dotted-quad IPv4 | DNS hostname
    R"re((?:\[[0-9a-fA-F:.]+\]|\d{1,3}(?:\.\d{1,3}){3}|[a-zA-Z0-9_](?:[a-zA-Z0-9_-]*[a-zA-Z0-9_])?(?:\.[a-zA-Z0-9_](?:[a-zA-Z0-9_-]*[a-zA-Z0-9_])?)*))re"
    // optional port
    R"re((?::\d{1,5})?)re"
    // optional path, and/or query/fragment (RFC 3986 allows ?query with empty
    // path, e.g. https://youtube.com?v=X); last char must not be punctuation
    R"re((?:(?:/[^\s<>"{}|\\^`\[\]]*)?(?:[?#][^\s<>"{}|\\^`\[\]]*)?[^\s<>"{}|\\^`\[\].,;:!?'"\)])?)re",
    std::regex::optimize};

static bool isValidUrl(const std::string& url) {
    // Guard against stack overflow: GCC's std::regex uses a recursive NFA
    // whose call depth is O(url.size()) for patterns like [A]+[B].  On a
    // typical 8 MB stack this overflows at ~50 k characters, producing
    // SIGSEGV.  Any URL longer than 8 192 bytes is malformed anyway.
    static constexpr std::size_t MAX_URL_LEN = 8192;
    if (url.size() > MAX_URL_LEN) return false;
    if (url.starts_with("http:///") || url.starts_with("https:///")) return false;
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
