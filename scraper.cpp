/*
 * Web Scraper - Iterative URL crawler with regex-based link extraction
 *
 * Prerequisites (Ubuntu/Debian):
 *   sudo apt-get install libcurl4-openssl-dev librocksdb-dev
 *
 * Build:
 *   g++ -std=c++23 -O2 -Wall -pthread scraper.cpp -lcurl -lrocksdb -o scraper
 */

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <iostream>
#include <mutex>
#include <queue>
#include <regex>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

#include <curl/curl.h>

#include "progress_bar.hpp"
#include "url_state_manager.hpp"

namespace fs = std::filesystem;
using UrlSet = std::unordered_set<std::string>;

// ── Constants ─────────────────────────────────────────────────────────────────

static constexpr double MAX_EXECUTION_TIME = 12600.0; // 3.5 hours

static const fs::path   STATE_DIR       {"state"};
static const fs::path   STATE_FILE      = STATE_DIR / "scraper_state.dat";
static const fs::path   ROCKSDB_DIR     = STATE_DIR / "url_state_db";
static const fs::path   CHECKPOINT_DIR  = STATE_DIR / "url_state_db_checkpoint";
static const std::string USER_AGENT =
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) "
    "AppleWebKit/537.36 (KHTML, like Gecko) "
    "Chrome/58.0.3029.110 Safari/537.3";

// ── Pre-compiled regexes ──────────────────────────────────────────────────────

// href=["']...["']  /  src=["']...["']
static const std::regex RE_HREF{
    R"re(href=["']([^"']+)["'])re",
    std::regex::icase | std::regex::optimize};
static const std::regex RE_SRC{
    R"re(src=["']([^"']+)["'])re",
    std::regex::icase | std::regex::optimize};
// Bare absolute URLs embedded in HTML; second char-class trims trailing punctuation
static const std::regex RE_URL{
    R"re(https?://[^\s<>"{}|\\^`\[\]]+[^\s<>"{}|\\^`\[\].,;:!?'"\)])re",
    std::regex::icase | std::regex::optimize};

// ── Thread pool ───────────────────────────────────────────────────────────────

class ThreadPool {
public:
    explicit ThreadPool(size_t n) {
        for (size_t i = 0; i < n; ++i)
            workers_.emplace_back([this] { loop(); });
    }
    ~ThreadPool() {
        { std::unique_lock lk(mu_); stop_ = true; }
        cv_.notify_all();
        for (auto& t : workers_) t.join();
    }

    template <typename F>
    auto submit(F&& f) -> std::future<std::invoke_result_t<F>> {
        using R = std::invoke_result_t<F>;
        auto task = std::make_shared<std::packaged_task<R()>>(std::forward<F>(f));
        auto fut  = task->get_future();
        { std::unique_lock lk(mu_); tasks_.emplace([task] { (*task)(); }); }
        cv_.notify_one();
        return fut;
    }

private:
    void loop() {
        while (true) {
            std::function<void()> task;
            {
                std::unique_lock lk(mu_);
                cv_.wait(lk, [this] { return stop_ || !tasks_.empty(); });
                if (stop_ && tasks_.empty()) return;
                task = std::move(tasks_.front());
                tasks_.pop();
            }
            task();
        }
    }

    std::vector<std::thread>            workers_;
    std::queue<std::function<void()>>   tasks_;
    std::mutex                          mu_;
    std::condition_variable             cv_;
    bool                                stop_ = false;
};

// ── Migration helpers (used only when upgrading from the old chunk-file format) ─

/// Read URLs from a plain-text chunk file (one URL per line).
static std::vector<std::string> loadChunkFromTxt(const std::string& path) {
    std::ifstream f(path);
    if (!f) throw std::runtime_error("loadChunkFromTxt: cannot open " + path);
    std::vector<std::string> result;
    std::string line;
    while (std::getline(f, line))
        if (!line.empty()) result.push_back(std::move(line));
    return result;
}

/// Read URLs from a legacy ZIP chunk file.
/// Runs `unzip -p` via fork/execvp (no shell, no injection risk).
/// The path is resolved to its canonical form and verified to remain inside
/// STATE_DIR before the child process is created.
static std::vector<std::string> loadChunkFromZip(const std::string& zipPath) {
    // Resolve to a canonical (no symlinks, no "..") absolute path.
    std::error_code ec;
    fs::path canonical = fs::canonical(zipPath, ec);
    if (ec)
        throw std::runtime_error(
            "loadChunkFromZip: cannot resolve '" + zipPath + "': " + ec.message());

    // Ensure the resolved path is inside STATE_DIR (directory traversal guard).
    fs::path stateAbs = fs::weakly_canonical(STATE_DIR);
    auto rel = canonical.lexically_relative(stateAbs);
    if (rel.empty() || (!rel.native().empty() && rel.native().rfind("..", 0) == 0))
        throw std::runtime_error(
            "loadChunkFromZip: path is outside state directory: " + zipPath);

    // Create a pipe and run `unzip -p <file> urls.txt` in a child process.
    // Using execvp avoids the shell entirely — no quoting, no injection.
    int fd[2];
    if (pipe(fd) != 0)
        throw std::runtime_error("loadChunkFromZip: pipe() failed");

    pid_t pid = fork();
    if (pid < 0) {
        close(fd[0]); close(fd[1]);
        throw std::runtime_error("loadChunkFromZip: fork() failed");
    }
    if (pid == 0) {
        // Child: wire stdout → pipe write end, suppress stderr, exec unzip.
        close(fd[0]);
        if (dup2(fd[1], STDOUT_FILENO) < 0) _exit(1);
        close(fd[1]);
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) { dup2(devnull, STDERR_FILENO); close(devnull); }
        execlp("unzip", "unzip", "-p", canonical.c_str(), "urls.txt",
               static_cast<char*>(nullptr));
        _exit(1); // exec failed
    }

    // Parent: read lines from the pipe.
    close(fd[1]);
    std::vector<std::string> result;
    {
        FILE* f = fdopen(fd[0], "r");
        if (f) {
            char buf[4096];
            std::string leftover;
            while (fgets(buf, sizeof(buf), f)) {
                leftover += buf;
                size_t pos;
                while ((pos = leftover.find('\n')) != std::string::npos) {
                    std::string line = leftover.substr(0, pos);
                    if (!line.empty()) result.push_back(std::move(line));
                    leftover = leftover.substr(pos + 1);
                }
            }
            if (!leftover.empty()) result.push_back(std::move(leftover));
            fclose(f); // also closes fd[0]
        } else {
            close(fd[0]);
        }
    }
    waitpid(pid, nullptr, 0);
    return result;
}

/// Delete old ZIP and TXT chunk files left over from the migration.
static void cleanupOldChunks() {
    if (!fs::is_directory(STATE_DIR)) return;
    for (const auto& e : fs::directory_iterator(STATE_DIR)) {
        const std::string name = e.path().filename().string();
        const std::string ext  = e.path().extension().string();
        if ((ext == ".zip" || ext == ".txt") &&
            (name.rfind("scraper_state_visited_",  0) == 0 ||
             name.rfind("scraper_state_to_visit_", 0) == 0)) {
            std::error_code ec;
            fs::remove(e.path(), ec);
        }
    }
}

// State file format (state/scraper_state.dat):
//   One "key value" pair per line.
//
//   start_url      <url>
//   pages_scraped  <int>
//   urls_found     <int>
//   elapsed_time   <double>
//
// Full URL state lives in state/url_state_db_checkpoint/ (committed to git).

// ── URL utilities ─────────────────────────────────────────────────────────────

/// Returns true iff `url` has a lowercase http:// or https:// scheme and a
/// non-empty hostname.  Rejects "Https://...", "http:///path", etc.
static bool isValidHttpUrl(const std::string& url) {
    const bool isHttp  = url.size() > 7 && url.compare(0, 7,  "http://")  == 0;
    const bool isHttps = url.size() > 8 && url.compare(0, 8,  "https://") == 0;
    if (!isHttp && !isHttps) return false;
    const size_t hostStart = isHttps ? 8 : 7;
    const size_t hostEnd   = url.find('/', hostStart);
    const size_t hostLen   = (hostEnd == std::string::npos)
                             ? url.size() - hostStart
                             : hostEnd - hostStart;
    // Also reject query-only hosts like "http://?foo" (hostLen==0 after '?')
    return hostLen > 0 && url[hostStart] != '?' && url[hostStart] != '#';
}

/// Lowercases the URL scheme (the text before "://"), e.g. "Https://" → "https://".
static std::string normalizeScheme(std::string url) {
    const size_t colonPos = url.find("://");
    if (colonPos == std::string::npos) return url;
    for (size_t i = 0; i < colonPos; ++i)
        url[i] = static_cast<char>(std::tolower(static_cast<unsigned char>(url[i])));
    return url;
}

static std::string resolveUrl(const std::string& base, const std::string& rel) {
    if (rel.empty()) return "";

    // Already absolute – normalise scheme to lowercase and validate
    {
        std::string norm = normalizeScheme(rel);
        if (norm.size() >= 7 &&
            (norm.compare(0, 7, "http://") == 0 || norm.compare(0, 8, "https://") == 0))
            return isValidHttpUrl(norm) ? norm : "";
    }

    // Skip fragment-only, query-only, and non-http schemes (mailto:, javascript:, data:…)
    if (rel[0] == '#' || rel[0] == '?') return "";
    auto colon = rel.find(':');
    auto slash  = rel.find('/');
    if (colon != std::string::npos && (slash == std::string::npos || colon < slash))
        return "";

    // Extract scheme + host from base
    size_t schemeEnd = base.find("://");
    if (schemeEnd == std::string::npos) return "";
    const std::string scheme    = base.substr(0, schemeEnd);
    size_t            hostStart = schemeEnd + 3;
    size_t            hostEnd   = base.find('/', hostStart);
    const std::string host      = (hostEnd == std::string::npos)
                                  ? base.substr(hostStart)
                                  : base.substr(hostStart, hostEnd - hostStart);

    // Reject base URLs with an empty hostname (e.g. "http:///path")
    if (host.empty()) return "";

    // Protocol-relative  //host/path
    if (rel.size() >= 2 && rel.substr(0, 2) == "//")
        return scheme + ":" + rel;

    // Root-relative  /path
    if (rel[0] == '/')
        return scheme + "://" + host + rel;

    // Relative path – resolve against base directory
    std::string basePath = (hostEnd == std::string::npos) ? "/" : base.substr(hostEnd);
    for (char c : {'?', '#'}) {
        auto pos = basePath.find(c);
        if (pos != std::string::npos) basePath = basePath.substr(0, pos);
    }
    size_t lastSlash = basePath.rfind('/');
    std::string dir  = (lastSlash == std::string::npos) ? "/" : basePath.substr(0, lastSlash + 1);
    return scheme + "://" + host + dir + rel;
}

static UrlSet extractUrls(const std::string& raw, const std::string& baseUrl) {
    UrlSet urls;

    // std::regex::icase calls std::toupper/tolower on every character.
    // Those functions are only defined for values in [0, UCHAR_MAX] or EOF;
    // on platforms where char is signed, bytes >= 0x80 arrive as negative
    // ints, which is undefined behaviour and causes SIGSEGV on glibc.
    // Sanitise the page once up-front: replace every non-ASCII byte (>= 0x80)
    // and every null byte with a plain space.  ASCII URLs are unaffected.
    std::string html;
    html.reserve(raw.size());
    for (unsigned char c : raw)
        html += (c == 0 || c >= 0x80) ? ' ' : static_cast<char>(c);

    // href and src attributes → resolve relative URLs
    for (const auto& re : {std::cref(RE_HREF), std::cref(RE_SRC)}) {
        auto begin = std::sregex_iterator(html.begin(), html.end(), re.get());
        for (auto it = begin; it != std::sregex_iterator(); ++it) {
            std::string resolved = resolveUrl(baseUrl, (*it)[1].str());
            if (isValidHttpUrl(resolved))
                urls.insert(std::move(resolved));
        }
    }

    // Bare absolute URLs embedded in the markup; normalise scheme to lowercase
    // and validate before inserting (RE_URL uses icase so "Https://" would match).
    auto begin = std::sregex_iterator(html.begin(), html.end(), RE_URL);
    for (auto it = begin; it != std::sregex_iterator(); ++it) {
        std::string u = normalizeScheme((*it)[0].str());
        if (isValidHttpUrl(u))
            urls.insert(std::move(u));
    }

    return urls;
}

// ── HTTP fetching ─────────────────────────────────────────────────────────────

static size_t curlWrite(char* ptr, size_t size, size_t nmemb, std::string* out) {
    out->append(ptr, size * nmemb);
    return size * nmemb;
}

static std::string fetchUrl(const std::string& url, long timeout = 10) {
    // Skip malformed, empty, or non-http(s) URLs before touching libcurl.
    if (!isValidHttpUrl(url)) return "";

    CURL* curl = curl_easy_init();
    if (!curl) return "";

    std::string response;
    curl_easy_setopt(curl, CURLOPT_URL,           url.c_str());
    curl_easy_setopt(curl, CURLOPT_USERAGENT,     USER_AGENT.c_str());
    curl_easy_setopt(curl, CURLOPT_TIMEOUT,       timeout);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curlWrite);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA,     &response);

    CURLcode res = curl_easy_perform(curl);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        std::cerr << "    Error fetching " << url << ": "
                  << curl_easy_strerror(res) << '\n';
        return "";
    }
    return response;
}

// ── Scraper ───────────────────────────────────────────────────────────────────

struct ScrapeResult {
    std::string startUrl;
    double      totalTime     = 0.0;
    int         pagesScraped  = 0;
    int         urlsFound     = 0;
    size_t      visitedCount  = 0;
    size_t      queueRemaining = 0;
};

static ScrapeResult scrape(const std::string& startUrl, int maxPages,
                            bool resume, int numWorkers) {
    // For fresh starts, remove any previous RocksDB data
    if (!resume) {
        std::error_code ec;
        fs::remove_all(ROCKSDB_DIR,    ec);
        fs::remove_all(CHECKPOINT_DIR, ec);
    }

    // Auto-promote the checkpoint to the live DB location.
    // This is the normal path after every successful run (locally and in CI):
    // the checkpoint was committed to git and is now checked out; we rename it
    // to the live path before opening so the full URL state is instantly
    // available without any import step.
    if (resume &&
        !fs::exists(ROCKSDB_DIR / "CURRENT") &&
         fs::exists(CHECKPOINT_DIR / "CURRENT")) {
        std::cout << "Promoting checkpoint to live database...\n";
        std::error_code ec;
        fs::rename(CHECKPOINT_DIR, ROCKSDB_DIR, ec);
        if (ec)
            throw std::runtime_error("Failed to rename checkpoint: " + ec.message());
    }

    // True when we promoted a checkpoint or have a pre-existing local DB.
    // When false we fall back to loading legacy chunk files (one-time migration).
    const bool rocksDbExisted = fs::exists(ROCKSDB_DIR / "CURRENT");

    // Open (or create) the RocksDB-backed URL state manager
    fs::create_directories(STATE_DIR);
    UrlStateManager urlMgr(ROCKSDB_DIR.string());

    int         pagesScraped    = 0;
    int         urlsFound       = 0;
    double      originalElapsed = 0.0;
    std::string actualStartUrl  = startUrl;
    size_t      visitedCount    = 0;

    if (resume) {
        if (fs::exists(STATE_FILE)) {
            auto tResume = std::chrono::steady_clock::now();
            std::ifstream sf(STATE_FILE);
            if (!sf) throw std::runtime_error("cannot open state file");

            std::cout << "Resuming from saved state: " << STATE_FILE << '\n'
                      << std::string(80, '-') << '\n';

            std::string line;
            while (std::getline(sf, line)) {
                if (line.empty()) continue;
                auto sp = line.find(' ');
                if (sp == std::string::npos) continue;
                std::string key = line.substr(0, sp);
                std::string val = line.substr(sp + 1);

                try {
                    if      (key == "start_url")     actualStartUrl  = val;
                    else if (key == "pages_scraped") pagesScraped    = std::stoi(val);
                    else if (key == "urls_found")    urlsFound       = std::stoi(val);
                    else if (key == "elapsed_time")  originalElapsed = std::stod(val);
                    // Legacy chunk keys (visited_chunk / to_visit_chunk) are
                    // ignored; migration is handled below by auto-discovery.
                } catch (const std::exception& e) {
                    std::cerr << "  Warning: bad value for key '" << key
                              << "': " << e.what() << " (skipping)\n";
                }
            }

            if (rocksDbExisted) {
                // Fast path: checkpoint was promoted, open and go.
                visitedCount = urlMgr.countByState(UrlState::COMPLETED);
                double took  = std::chrono::duration<double>(
                    std::chrono::steady_clock::now() - tResume).count();
                std::cout << "  RocksDB ready in " << took << "s\n";
            } else {
                // Slow path: one-time migration from legacy chunk files.
                // After this run the checkpoint is committed to git and this
                // branch will never be taken again.
                size_t importedVisited = 0, importedToVisit = 0;
                size_t chunkV = 0, chunkT = 0;

                auto importChunk = [&](const fs::path& p, UrlState st,
                                       size_t& imported, size_t& idx,
                                       const char* label) {
                    std::cout << "  [" << label << " chunk " << idx
                              << "] Loading " << p.string() << " ...\n";
                    std::vector<std::string> chunk =
                        (p.extension() == ".txt") ? loadChunkFromTxt(p.string())
                                                  : loadChunkFromZip(p.string());
                    std::cout << "    " << chunk.size() << " URLs read\n";
                    imported += urlMgr.bulkImportFast(chunk, st);
                    ++idx;
                };

                for (int i = 0; ; ++i) {
                    fs::path pt = STATE_DIR / ("scraper_state_visited_" + std::to_string(i) + ".txt");
                    fs::path pz = STATE_DIR / ("scraper_state_visited_" + std::to_string(i) + ".zip");
                    if      (fs::exists(pt)) importChunk(pt, UrlState::COMPLETED,  importedVisited, chunkV, "visited");
                    else if (fs::exists(pz)) importChunk(pz, UrlState::COMPLETED,  importedVisited, chunkV, "visited");
                    else break;
                }
                for (int i = 0; ; ++i) {
                    fs::path pt = STATE_DIR / ("scraper_state_to_visit_" + std::to_string(i) + ".txt");
                    fs::path pz = STATE_DIR / ("scraper_state_to_visit_" + std::to_string(i) + ".zip");
                    if      (fs::exists(pt)) importChunk(pt, UrlState::DISCOVERED, importedToVisit, chunkT, "to-visit");
                    else if (fs::exists(pz)) importChunk(pz, UrlState::DISCOVERED, importedToVisit, chunkT, "to-visit");
                    else break;
                }

                urlMgr.flushAll();
                // Delete the temporary chunk files so they are not committed.
                cleanupOldChunks();

                visitedCount = importedVisited;
                double took  = std::chrono::duration<double>(
                    std::chrono::steady_clock::now() - tResume).count();
                std::cout << std::string(80, '-') << '\n'
                          << "  Migrated " << importedVisited << " visited + "
                          << importedToVisit << " to-visit URLs in " << took << "s\n";
            }

            maxPages = pagesScraped + maxPages;
            std::cout << std::string(80, '-') << '\n'
                      << "  Previous start URL: "    << actualStartUrl << '\n'
                      << "  Pages already scraped: " << pagesScraped   << '\n'
                      << "  URLs visited: "          << visitedCount   << '\n'
                      << "  Will scrape up to " << maxPages
                      << " total pages (continuing from " << pagesScraped << ")\n";
        } else {
            std::cout << "No saved state found. Starting fresh...\n";
            if (!startUrl.empty())
                urlMgr.checkAndSet(startUrl, UrlState::DISCOVERED);
        }
    } else {
        urlMgr.checkAndSet(startUrl, UrlState::DISCOVERED);
    }

    // Reset any interrupted CRAWLING URLs back to DISCOVERED for retry.
    // Collect them first (can't modify DB safely while the iterator is live).
    {
        std::vector<std::string> crawling;
        urlMgr.forEachUrlByState(UrlState::CRAWLING,
            [&](const std::string& url) { crawling.push_back(url); });
        for (const auto& url : crawling)
            urlMgr.setState(url, UrlState::DISCOVERED);
        if (!crawling.empty())
            std::cout << "  Reset " << crawling.size()
                      << " interrupted CRAWLING URL(s) to DISCOVERED\n";
    }

    // Build in-memory work queue directly from RocksDB without an intermediate
    // vector, halving the peak RAM needed (important when queue has 30 M+ URLs).
    std::queue<std::string> toVisitQueue;
    urlMgr.forEachUrlByState(UrlState::DISCOVERED,
        [&](const std::string& url) { toVisitQueue.push(url); });

    auto sessionStart = std::chrono::steady_clock::now();

    auto elapsed = [&] {
        return std::chrono::duration<double>(
            std::chrono::steady_clock::now() - sessionStart).count();
    };

    double maxH = MAX_EXECUTION_TIME / 3600.0;
    std::cout << "Starting web scraper from: " << actualStartUrl << '\n'
              << "Max execution time: " << static_cast<int>(MAX_EXECUTION_TIME)
              << " seconds (" << maxH << " hour" << (maxH != 1.0 ? "s" : "") << ")\n"
              << "Max pages: " << maxPages << '\n'
              << std::string(80, '-') << '\n';

    if (resume && toVisitQueue.empty())
        std::cout << "\nWarning: No URLs in queue to scrape!\n"
                  << "The scraper has already visited all discoverable URLs from the start URL.\n"
                  << "Scraping cannot continue without URLs in the queue.\n"
                  << std::string(80, '-') << '\n';

    ThreadPool pool(numWorkers);

    // Progress bar tracks pages scraped in this run only.
    // pagesScraped already includes previously scraped pages (from resume);
    // runTarget is the per-run limit relative to that baseline.
    const int runStart  = pagesScraped;
    const int runTarget = maxPages - runStart;
    ProgressBar pbar(static_cast<std::size_t>(runTarget), "Scraping");

    while (!toVisitQueue.empty()) {
        if (elapsed() > MAX_EXECUTION_TIME) {
            std::cout << "\nSession execution time limit reached (" << elapsed() << " seconds)\n";
            break;
        }
        if (pagesScraped >= maxPages) {
            std::cout << "\nMax pages limit reached (" << pagesScraped << " pages)\n";
            break;
        }

        // Build a batch of DISCOVERED URLs, up to numWorkers in size
        std::vector<std::string> batch;
        while (!toVisitQueue.empty() && static_cast<int>(batch.size()) < numWorkers) {
            std::string url = std::move(toVisitQueue.front());
            toVisitQueue.pop();
            UrlState st;
            if (urlMgr.getState(url, st) && st == UrlState::DISCOVERED) {
                urlMgr.setState(url, UrlState::CRAWLING);
                batch.push_back(std::move(url));
            }
        }
        if (batch.empty()) continue;

        // Fetch all URLs in the batch in parallel
        using Result = std::pair<std::string, UrlSet>;
        std::vector<std::future<Result>> futures;
        futures.reserve(batch.size());
        for (auto& url : batch) {
            futures.push_back(pool.submit([u = url]() -> Result {
                std::string html = fetchUrl(u);
                if (html.empty()) return {u, {}};
                return {u, extractUrls(html, u)};
            }));
        }

        // Process results as they finish (in submission order)
        for (auto& fut : futures) {
            auto [url, found] = fut.get();
            urlMgr.setState(url, UrlState::COMPLETED);
            ++pagesScraped;
            ++visitedCount;

            double totalElapsed = originalElapsed + elapsed();
            std::cout << '[' << pagesScraped << "] Scraped: " << url << '\n';

            urlsFound += static_cast<int>(found.size());
            size_t newCount = 0;
            for (const auto& u : found) {
                if (urlMgr.checkAndSet(u, UrlState::DISCOVERED)) {
                    toVisitQueue.push(u);
                    ++newCount;
                }
            }
            std::cout << "  Found " << found.size() << " URLs (" << newCount << " new)\n"
                      << "  Queue size: " << toVisitQueue.size()
                      << ", Visited: " << visitedCount << '\n'
                      << "  Elapsed time: " << totalElapsed << "s\n";

            pbar.update(static_cast<std::size_t>(pagesScraped - runStart));
        }
    }

    pbar.finish(static_cast<std::size_t>(pagesScraped - runStart));

    double totalTime = originalElapsed + elapsed();

    // Count queue remaining without materialising the full URL list.
    size_t finalQueueSize = urlMgr.countByState(UrlState::DISCOVERED);

    // Write metadata state file.
    // The full URL state is persisted in the RocksDB checkpoint that
    // UrlStateManager::seal() creates on destruction (see below).
    {
        fs::create_directories(STATE_DIR);
        std::ofstream f(STATE_FILE);
        if (!f) throw std::runtime_error("cannot write state file");
        f << "start_url "    << actualStartUrl << '\n'
          << "pages_scraped " << pagesScraped  << '\n'
          << "urls_found "    << urlsFound     << '\n'
          << "elapsed_time "  << std::fixed    << totalTime << '\n';
        std::cout << "  Metadata saved to " << STATE_FILE << '\n';
    }

    std::cout << std::string(80, '-') << '\n'
              << "Scraping completed!\n"
              << "Total time: "              << totalTime      << " seconds\n"
              << "Pages scraped: "           << pagesScraped   << '\n'
              << "Total URLs found: "        << urlsFound      << '\n'
              << "Unique URLs visited: "     << visitedCount   << '\n'
              << "URLs remaining in queue: " << finalQueueSize << '\n';

    // UrlStateManager destructor calls seal(), which creates/updates the
    // RocksDB checkpoint at state/url_state_db_checkpoint/.
    return {actualStartUrl, totalTime, pagesScraped, urlsFound,
            visitedCount, finalQueueSize};
}

// ── CLI ───────────────────────────────────────────────────────────────────────

int main(int argc, char* argv[]) {
    static const std::string WIKIPEDIA_URL =
        "https://en.wikipedia.org/wiki/Main_Page";

    std::vector<std::string> args(argv + 1, argv + argc);
    bool        resume     = false;
    bool        useWiki    = false;
    int         maxPages   = 100;
    int         numWorkers = 5;
    std::string startUrl;

    for (size_t i = 0; i < args.size(); ++i) {
        const auto& a = args[i];
        if (a == "--resume") {
            resume = true;
        } else if (a == "--wikipedia" || a == "-w") {
            useWiki = true;
        } else if (a == "-h" || a == "--help") {
            std::cout <<
                "Usage: scraper [url|--wikipedia] [max_pages] [--resume] [--workers N]\n\n"
                "Arguments:\n"
                "  url               URL to start scraping from\n"
                "  --wikipedia, -w   Start scraping from Wikipedia main page\n"
                "  max_pages         Maximum number of pages to scrape (default: 100)\n"
                "  --resume          Resume from saved state\n"
                "  --workers N       Number of parallel fetch workers (default: 5)\n"
                "  --help, -h        Show this help message\n\n"
                "Examples:\n"
                "  scraper --wikipedia 50\n"
                "  scraper --wikipedia --resume\n"
                "  scraper https://example.com 100\n"
                "  scraper https://example.com 50 --resume\n"
                "  scraper --resume\n";
            return 0;
        } else if (a == "--workers" && i + 1 < args.size()) {
            try { numWorkers = std::stoi(args[++i]); } catch (...) {}
        } else if (!a.empty() && a[0] != '-') {
            bool isNumber = std::all_of(a.begin(), a.end(), ::isdigit);
            if (isNumber) maxPages = std::stoi(a);
            else          startUrl = a;
        }
    }

    if (useWiki) {
        startUrl = WIKIPEDIA_URL;
        if (!resume) {
            std::cout << std::string(80, '=') << '\n'
                      << "Wikipedia Web Scraper\n"
                      << std::string(80, '=') << '\n'
                      << "Starting from: " << WIKIPEDIA_URL << "\n\n";
        }
    } else if (!resume) {
        if (startUrl.empty()) {
            std::cerr << "Usage: scraper [url|--wikipedia] [max_pages] [--resume]\n"
                         "Example: scraper https://example.com 50\n"
                         "Example: scraper --wikipedia 100\n"
                         "Example: scraper --resume\n\n"
                         "Use --help for more information\n";
            return 1;
        }
        if (startUrl.substr(0, 7) != "http://" && startUrl.substr(0, 8) != "https://") {
            std::cerr << "Error: URL must start with http:// or https://\n";
            return 1;
        }
    }

    curl_global_init(CURL_GLOBAL_DEFAULT);

    auto result = scrape(startUrl, maxPages, resume, numWorkers);

    curl_global_cleanup();

    {
        std::ofstream f("scraper_results.txt");
        f << "Web Scraper Results\n"
          << std::string(80, '=') << '\n'
          << "Start URL: "            << result.startUrl      << '\n'
          << "Total Time: "           << result.totalTime     << " seconds\n"
          << "Pages Scraped: "        << result.pagesScraped  << '\n'
          << "Total URLs Found: "     << result.urlsFound     << '\n'
          << "Unique URLs Visited: "  << result.visitedCount  << '\n';
    }

    std::cout << '\n' << std::string(80, '=') << '\n'
              << "Scraping Summary\n"
              << std::string(80, '=') << '\n'
              << "Start URL: "               << result.startUrl       << '\n'
              << "Pages scraped: "           << result.pagesScraped   << '\n'
              << "Total URLs found: "        << result.urlsFound      << '\n'
              << "Unique URLs visited: "     << result.visitedCount   << '\n'
              << "URLs remaining in queue: " << result.queueRemaining << '\n'
              << "Total time: "              << result.totalTime      << " seconds\n\n"
              << "Results saved to: scraper_results.txt\n"
              << "State saved to: "          << STATE_FILE            << "\n\n";

    if (useWiki)
        std::cout << "To resume scraping, run: scraper --wikipedia " << maxPages << " --resume\n";
    else
        std::cout << "To resume scraping, run: scraper --resume\n";

    std::cout << std::string(80, '=') << '\n';
    return 0;
}
