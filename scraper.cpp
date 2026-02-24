/*
 * Web Scraper - Iterative URL crawler with regex-based link extraction
 *
 * Prerequisites (Ubuntu/Debian):
 *   sudo apt-get install libcurl4-openssl-dev libzip-dev librocksdb-dev
 *
 * Build:
 *   g++ -std=c++23 -O2 -Wall -pthread scraper.cpp -lcurl -lzip -lrocksdb -o scraper
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
#include <sstream>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

#include <curl/curl.h>
#include <zip.h>

#include "url_state_manager.hpp"

namespace fs = std::filesystem;
using UrlSet = std::unordered_set<std::string>;

// ── Constants ─────────────────────────────────────────────────────────────────

static constexpr double MAX_EXECUTION_TIME = 18000.0; // 5 hours
static constexpr size_t MAX_URLS_PER_CHUNK = 500000;

static const fs::path   STATE_DIR{"state"};
static const fs::path   STATE_FILE = STATE_DIR / "scraper_state.dat";
static const fs::path   ROCKSDB_DIR = STATE_DIR / "url_state_db";
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

// ── ZIP helpers ───────────────────────────────────────────────────────────────

static void saveChunkToZip(const std::vector<std::string>& urls,
                            const std::string& zipPath) {
    // Build newline-separated content (no trailing newline)
    std::string content;
    content.reserve(urls.size() * 60);
    for (size_t i = 0; i < urls.size(); ++i) {
        if (i > 0) content += '\n';
        content += urls[i];
    }

    int    err = 0;
    zip_t* zf  = zip_open(zipPath.c_str(), ZIP_CREATE | ZIP_TRUNCATE, &err);
    if (!zf) throw std::runtime_error("saveChunkToZip: cannot open " + zipPath);

    // content must remain alive until zip_close (freep=0 → we manage the buffer)
    zip_source_t* src = zip_source_buffer(zf, content.data(), content.size(), 0);
    if (!src) {
        zip_close(zf);
        throw std::runtime_error("saveChunkToZip: cannot create source");
    }
    if (zip_file_add(zf, "urls.txt", src, ZIP_FL_OVERWRITE) < 0) {
        zip_source_free(src);
        zip_close(zf);
        throw std::runtime_error("saveChunkToZip: cannot add file in " + zipPath);
    }
    zip_close(zf); // flushes/compresses; content is still in scope
}

static std::vector<std::string> loadChunkFromZip(const std::string& zipPath) {
    int    err = 0;
    zip_t* zf  = zip_open(zipPath.c_str(), ZIP_RDONLY, &err);
    if (!zf) throw std::runtime_error("loadChunkFromZip: cannot open " + zipPath);

    zip_file_t* entry = zip_fopen(zf, "urls.txt", 0);
    if (!entry) {
        zip_close(zf);
        throw std::runtime_error("loadChunkFromZip: urls.txt not found in " + zipPath);
    }

    std::string content;
    char        buf[8192];
    zip_int64_t n;
    while ((n = zip_fread(entry, buf, sizeof(buf))) > 0)
        content.append(buf, static_cast<size_t>(n));
    zip_fclose(entry);
    zip_close(zf);

    std::vector<std::string> result;
    std::istringstream       ss(content);
    std::string              line;
    while (std::getline(ss, line))
        if (!line.empty()) result.push_back(std::move(line));
    return result;
}

// ── State persistence ─────────────────────────────────────────────────────────

static void cleanupOldChunks() {
    if (!fs::is_directory(STATE_DIR)) return;
    for (const auto& e : fs::directory_iterator(STATE_DIR)) {
        const std::string name = e.path().filename().string();
        if (e.path().extension() == ".zip" &&
            (name.rfind("scraper_state_visited_",  0) == 0 ||
             name.rfind("scraper_state_to_visit_", 0) == 0)) {
            std::error_code ec;
            fs::remove(e.path(), ec);
        }
    }
}

// State file format (state/scraper_state.dat):
//   One "key value" pair per line; multi-value keys (visited_chunk,
//   to_visit_chunk) may appear more than once.
//
//   start_url      <url>
//   pages_scraped  <int>
//   urls_found     <int>
//   elapsed_time   <double>
//   visited_chunk  <path>       (repeated)
//   to_visit_chunk <path>       (repeated)

// ── URL utilities ─────────────────────────────────────────────────────────────

static std::string resolveUrl(const std::string& base, const std::string& rel) {
    if (rel.empty()) return "";

    // Already absolute
    if (rel.size() >= 7 &&
        (rel.substr(0, 7) == "http://" || rel.substr(0, 8) == "https://"))
        return rel;

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

static UrlSet extractUrls(const std::string& html, const std::string& baseUrl) {
    UrlSet urls;

    // href and src attributes → resolve relative URLs
    for (const auto& re : {std::cref(RE_HREF), std::cref(RE_SRC)}) {
        auto begin = std::sregex_iterator(html.begin(), html.end(), re.get());
        for (auto it = begin; it != std::sregex_iterator(); ++it) {
            std::string resolved = resolveUrl(baseUrl, (*it)[1].str());
            if (resolved.size() >= 7 &&
                (resolved.substr(0, 7) == "http://" || resolved.substr(0, 8) == "https://"))
                urls.insert(std::move(resolved));
        }
    }

    // Bare absolute URLs embedded in the markup
    auto begin = std::sregex_iterator(html.begin(), html.end(), RE_URL);
    for (auto it = begin; it != std::sregex_iterator(); ++it)
        urls.insert((*it)[0].str());

    return urls;
}

// ── HTTP fetching ─────────────────────────────────────────────────────────────

static size_t curlWrite(char* ptr, size_t size, size_t nmemb, std::string* out) {
    out->append(ptr, size * nmemb);
    return size * nmemb;
}

static std::string fetchUrl(const std::string& url, long timeout = 10) {
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
        fs::remove_all(ROCKSDB_DIR, ec);
        fs::remove_all(ROCKSDB_DIR.string() + "_checkpoint", ec);
    }

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

            size_t importedVisited = 0, importedToVisit = 0;
            size_t chunkV = 0, chunkT = 0;

            std::cout << "Resuming from saved state: " << STATE_FILE << '\n'
                      << std::string(80, '-') << '\n';

            std::string line;
            while (std::getline(sf, line)) {
                if (line.empty()) continue;
                auto sp = line.find(' ');
                if (sp == std::string::npos) continue;
                std::string key = line.substr(0, sp);
                std::string val = line.substr(sp + 1);

                if      (key == "start_url")     actualStartUrl  = val;
                else if (key == "pages_scraped") pagesScraped    = std::stoi(val);
                else if (key == "urls_found")    urlsFound       = std::stoi(val);
                else if (key == "elapsed_time")  originalElapsed = std::stod(val);
                else if (key == "visited_chunk") {
                    if (!fs::exists(val)) continue;
                    std::cout << "  [visited chunk " << chunkV << "] Loading " << val << " ...\n";
                    auto chunk = loadChunkFromZip(val);
                    std::cout << "    " << chunk.size() << " URLs read from zip\n";
                    size_t n = urlMgr.bulkImport(chunk, UrlState::COMPLETED);
                    importedVisited += n;
                    std::cout << "    " << n << " URLs imported into RocksDB\n";
                    ++chunkV;
                }
                else if (key == "to_visit_chunk") {
                    if (!fs::exists(val)) continue;
                    std::cout << "  [to-visit chunk " << chunkT << "] Loading " << val << " ...\n";
                    auto chunk = loadChunkFromZip(val);
                    std::cout << "    " << chunk.size() << " URLs read from zip\n";
                    size_t n = urlMgr.bulkImport(chunk, UrlState::DISCOVERED);
                    importedToVisit += n;
                    std::cout << "    " << n << " URLs imported into RocksDB\n";
                    ++chunkT;
                }
            }

            visitedCount = importedVisited;
            maxPages     = pagesScraped + maxPages;

            double took = std::chrono::duration<double>(
                std::chrono::steady_clock::now() - tResume).count();
            std::cout << std::string(80, '-') << '\n'
                      << "  Previous start URL: "     << actualStartUrl  << '\n'
                      << "  Pages already scraped: "  << pagesScraped    << '\n'
                      << "  URLs imported to queue: " << importedToVisit << '\n'
                      << "  URLs visited: "           << importedVisited << '\n'
                      << "  State imported in "       << took            << "s\n"
                      << std::string(80, '-') << '\n'
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

    // Reset any interrupted CRAWLING URLs back to DISCOVERED for retry
    for (const auto& url : urlMgr.getUrlsByState(UrlState::CRAWLING))
        urlMgr.setState(url, UrlState::DISCOVERED);

    // Build in-memory work queue from all DISCOVERED URLs
    std::queue<std::string> toVisitQueue;
    {
        auto discovered = urlMgr.getUrlsByState(UrlState::DISCOVERED);
        for (auto& url : discovered)
            toVisitQueue.push(std::move(url));
    }

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
        }
    }

    double totalTime = originalElapsed + elapsed();

    // Export state to legacy ZIP format for backward compatibility and git storage
    size_t finalQueueSize = 0;
    {
        auto tSave = std::chrono::steady_clock::now();
        fs::create_directories(STATE_DIR);
        cleanupOldChunks();

        // Stream URLs from RocksDB into zip chunks one chunk at a time,
        // never holding more than MAX_URLS_PER_CHUNK URLs in memory at once.
        auto writeChunksStreaming = [&](UrlState state, const std::string& prefix,
                                        const std::string& label)
                -> std::pair<std::vector<std::string>, size_t> {
            std::vector<std::string> files;
            std::vector<std::string> batch;
            batch.reserve(MAX_URLS_PER_CHUNK);
            size_t chunkIdx = 0;
            size_t totalUrls = 0;

            auto flush = [&]() {
                if (batch.empty()) return;
                std::string path =
                    (STATE_DIR / (prefix + std::to_string(chunkIdx) + ".zip")).string();
                std::cout << "  [" << label << " chunk " << chunkIdx << "] Writing "
                          << batch.size() << " URLs -> " << path << " ...\n";
                saveChunkToZip(batch, path);
                std::cout << "    Done\n";
                totalUrls += batch.size();
                files.push_back(std::move(path));
                ++chunkIdx;
                batch.clear();
            };

            urlMgr.forEachUrlByState(state, [&](const std::string& url) {
                batch.push_back(url);
                if (batch.size() >= MAX_URLS_PER_CHUNK) flush();
            });
            flush();

            std::cout << "  Total: " << totalUrls << " URLs across "
                      << files.size() << " chunk(s)\n";
            return {std::move(files), totalUrls};
        };

        std::cout << "Saving visited URLs (COMPLETED) to zip chunks...\n";
        auto [visitedFiles, visitedTotal] =
            writeChunksStreaming(UrlState::COMPLETED, "scraper_state_visited_", "visited");
        visitedCount = visitedTotal;

        std::cout << "Saving to-visit URLs (DISCOVERED) to zip chunks...\n";
        auto [toVisitFiles, toVisitTotal] =
            writeChunksStreaming(UrlState::DISCOVERED, "scraper_state_to_visit_", "to-visit");
        finalQueueSize = toVisitTotal;

        std::ofstream f(STATE_FILE);
        f << "start_url "     << actualStartUrl << '\n'
          << "pages_scraped " << pagesScraped   << '\n'
          << "urls_found "    << urlsFound      << '\n'
          << "elapsed_time "  << std::fixed     << totalTime << '\n';
        for (const auto& p : visitedFiles)  f << "visited_chunk "  << p << '\n';
        for (const auto& p : toVisitFiles)  f << "to_visit_chunk " << p << '\n';

        double took = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - tSave).count();
        int totalChunks =
            static_cast<int>(visitedFiles.size() + toVisitFiles.size());
        std::cout << "  State saved to " << STATE_FILE
                  << " with " << totalChunks << " chunk files"
                  << " (took " << took << "s)\n";
    }

    std::cout << std::string(80, '-') << '\n'
              << "Scraping completed!\n"
              << "Total time: "             << totalTime       << " seconds\n"
              << "Pages scraped: "          << pagesScraped    << '\n'
              << "Total URLs found: "       << urlsFound       << '\n'
              << "Unique URLs visited: "    << visitedCount    << '\n'
              << "URLs remaining in queue: "<< finalQueueSize  << '\n';

    // UrlStateManager destructor seals (checkpoints) the RocksDB database
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
