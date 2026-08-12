/*
 * UrlStore - append-only, zip-chunked URL store
 *
 * Replaces the RocksDB-backed UrlStateManager.  All crawl state lives in the
 * repository as zipped text chunks:
 *
 *     urls/visited/<seq>.zip   every URL that has been crawled
 *     urls/queue/<seq>.zip     every URL that has been discovered
 *
 * Each zip holds a single `urls.txt` member, one URL per line.
 *
 * Chunks are IMMUTABLE.  Once a chunk is written it is never modified or
 * rewritten, and a run only ever adds new chunks.  That is what keeps the
 * repository cheap: git stores each blob exactly once, so history grows by the
 * genuinely-new data and nothing else.  Rewriting zipped chunks instead would
 * cost ~8.5x more, because git cannot delta already-compressed data.
 *
 * A URL is "pending" when it appears in queue/ but not in visited/.  Nothing
 * is ever deleted to represent progress; visited/ is the record of what has
 * been consumed.
 *
 * Memory: URLs are deduplicated by 64-bit hash, not by storing the strings, so
 * ~35M URLs cost roughly 600 MB rather than the ~6 GB the raw text would need.
 * At 35M entries the chance of any hash collision is about 3e-5; a collision
 * would silently skip one URL, which is an acceptable trade for a crawler.
 *
 * Prerequisites (Ubuntu/Debian):
 *   sudo apt-get install libzip-dev
 *
 * Build:
 *   g++ -std=c++23 -O2 scraper.cpp -lcurl -lzip -o scraper
 */

#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <zip.h>

namespace fs = std::filesystem;

// ── 64-bit hash set (open addressing) ────────────────────────────────────────

/// Minimal open-addressing set of 64-bit hashes.
/// Stores hashes rather than URLs so tens of millions of entries stay in RAM.
class HashSet64 {
public:
    HashSet64() { rehash(1 << 16); }

    /// Insert `h`; returns true when it was not already present.
    bool insert(std::uint64_t h) {
        if (h == kEmpty) h = kTombstoneSubstitute;
        if ((size_ + 1) * 10 >= cap_ * 7) rehash(cap_ * 2);   // keep load < 0.7
        std::size_t mask = cap_ - 1;
        std::size_t i = static_cast<std::size_t>(h) & mask;
        while (slots_[i] != kEmpty) {
            if (slots_[i] == h) return false;
            i = (i + 1) & mask;
        }
        slots_[i] = h;
        ++size_;
        return true;
    }

    [[nodiscard]] bool contains(std::uint64_t h) const {
        if (h == kEmpty) h = kTombstoneSubstitute;
        std::size_t mask = cap_ - 1;
        std::size_t i = static_cast<std::size_t>(h) & mask;
        while (slots_[i] != kEmpty) {
            if (slots_[i] == h) return true;
            i = (i + 1) & mask;
        }
        return false;
    }

    [[nodiscard]] std::size_t size() const { return size_; }

    /// Pre-size the table so a known bulk load does not rehash repeatedly.
    void reserve(std::size_t n) {
        std::size_t want = 1;
        while (want * 7 <= n * 10) want <<= 1;
        if (want > cap_) rehash(want);
    }

private:
    static constexpr std::uint64_t kEmpty               = 0;
    static constexpr std::uint64_t kTombstoneSubstitute = 0x9E3779B97F4A7C15ULL;

    void rehash(std::size_t newCap) {
        std::vector<std::uint64_t> old(newCap, kEmpty);
        old.swap(slots_);
        std::size_t oldCap = cap_;
        cap_ = newCap;
        std::size_t mask = cap_ - 1;
        for (std::size_t k = 0; k < oldCap; ++k) {
            std::uint64_t h = old[k];
            if (h == kEmpty) continue;
            std::size_t i = static_cast<std::size_t>(h) & mask;
            while (slots_[i] != kEmpty) i = (i + 1) & mask;
            slots_[i] = h;
        }
    }

    std::vector<std::uint64_t> slots_;
    std::size_t                cap_  = 0;
    std::size_t                size_ = 0;
};

/// FNV-1a over the URL bytes, finalised with a splitmix64 avalanche so that
/// similar URLs (which share long prefixes) land far apart in the table.
[[nodiscard]] inline std::uint64_t hashUrl(std::string_view s) {
    std::uint64_t h = 1469598103934665603ULL;
    for (unsigned char c : s) {
        h ^= c;
        h *= 1099511628211ULL;
    }
    h ^= h >> 30; h *= 0xBF58476D1CE4E5B9ULL;
    h ^= h >> 27; h *= 0x94D049BB133111EBULL;
    h ^= h >> 31;
    return h;
}

// ── Zip chunk I/O ────────────────────────────────────────────────────────────

/// Every chunk stores its URLs in a single member with this name.
inline constexpr const char* kMemberName = "urls.txt";

/// Read every line of the `urls.txt` member of a zip chunk.
inline void readZipChunk(const fs::path& path,
                         const std::function<void(std::string&&)>& fn) {
    int err = 0;
    zip_t* z = zip_open(path.c_str(), ZIP_RDONLY, &err);
    if (!z) throw std::runtime_error("cannot open zip: " + path.string());

    zip_file_t* f = zip_fopen(z, kMemberName, 0);
    if (!f) {
        zip_close(z);
        throw std::runtime_error("zip has no " + std::string(kMemberName) + ": " + path.string());
    }

    std::string buf;
    buf.resize(1 << 20);
    std::string line;
    zip_int64_t n;
    while ((n = zip_fread(f, buf.data(), buf.size())) > 0) {
        for (zip_int64_t i = 0; i < n; ++i) {
            char c = buf[static_cast<std::size_t>(i)];
            if (c == '\n') {
                // No CR stripping: URLs are exact byte keys and a few genuinely
                // end with CR.  Removing it would silently merge them onto the
                // URL without it.  Chunks are always written with plain \n.
                if (!line.empty()) fn(std::move(line));
                line.clear();
            } else {
                line.push_back(c);
            }
        }
    }
    if (!line.empty()) fn(std::move(line));

    zip_fclose(f);
    zip_close(z);
}

/// Write `urls` as a new zip chunk containing a single `urls.txt` member.
inline void writeZipChunk(const fs::path& path, const std::vector<std::string>& urls) {
    std::string blob;
    std::size_t total = 0;
    for (const auto& u : urls) total += u.size() + 1;
    blob.reserve(total);
    for (const auto& u : urls) { blob += u; blob.push_back('\n'); }

    fs::create_directories(path.parent_path());
    int err = 0;
    zip_t* z = zip_open(path.c_str(), ZIP_CREATE | ZIP_TRUNCATE, &err);
    if (!z) throw std::runtime_error("cannot create zip: " + path.string());

    // The source must outlive zip_close(), so hand libzip a copy it owns.
    char* owned = static_cast<char*>(std::malloc(blob.size()));
    if (!owned) { zip_close(z); throw std::runtime_error("out of memory writing chunk"); }
    std::memcpy(owned, blob.data(), blob.size());

    zip_source_t* src = zip_source_buffer(z, owned, blob.size(), 1 /* libzip frees */);
    if (!src) { std::free(owned); zip_close(z); throw std::runtime_error("zip_source_buffer failed"); }

    if (zip_file_add(z, kMemberName, src, ZIP_FL_OVERWRITE) < 0) {
        zip_source_free(src);
        zip_close(z);
        throw std::runtime_error("zip_file_add failed: " + path.string());
    }
    zip_set_file_compression(z, 0, ZIP_CM_DEFLATE, 9);

    if (zip_close(z) < 0)
        throw std::runtime_error("zip_close failed: " + path.string());
}

// ── UrlStore ─────────────────────────────────────────────────────────────────

class UrlStore {
public:
    /// Open the store rooted at `root` (typically "urls") and load every
    /// committed chunk into the in-memory dedup tables.
    explicit UrlStore(fs::path root, std::size_t chunkTargetBytes = 90u * 1024 * 1024)
        : root_(std::move(root)), chunkTarget_(chunkTargetBytes) {
        visitedDir_ = root_ / "visited";
        queueDir_   = root_ / "queue";
        load();
    }

    UrlStore(const UrlStore&)            = delete;
    UrlStore& operator=(const UrlStore&) = delete;

    /// Record `url` as discovered.  Returns true when it was not already known,
    /// in which case it is queued for a future run (or this one).
    bool addDiscovered(const std::string& url) {
        std::uint64_t h = hashUrl(url);
        if (!known_.insert(h)) return false;
        newQueue_.push_back(url);
        newQueueBytes_ += url.size() + 1;
        return true;
    }

    /// Record `url` as crawled.
    void markVisited(const std::string& url) {
        std::uint64_t h = hashUrl(url);
        known_.insert(h);
        if (!visited_.insert(h)) return;
        newVisited_.push_back(url);
        newVisitedBytes_ += url.size() + 1;
    }

    [[nodiscard]] bool isVisited(const std::string& url) const {
        return visited_.contains(hashUrl(url));
    }
    [[nodiscard]] bool isKnown(const std::string& url) const {
        return known_.contains(hashUrl(url));
    }

    /// Stream every pending URL (present in queue/ but not yet crawled),
    /// oldest chunk first.  `fn` returns false to stop early.
    /// Chunks are read one at a time so the full frontier never sits in RAM.
    void forEachPending(const std::function<bool(const std::string&)>& fn) const {
        for (const auto& chunk : queueChunks_) {
            bool keepGoing = true;
            readZipChunk(chunk, [&](std::string&& url) {
                if (!keepGoing) return;
                if (visited_.contains(hashUrl(url))) return;
                keepGoing = fn(url);
            });
            if (!keepGoing) return;
        }
    }

    /// Write everything accumulated this run as brand-new chunks.
    /// Existing chunks are never touched.
    void flush() {
        writePending(newVisited_, visitedDir_, "visited");
        writePending(newQueue_,   queueDir_,   "queue");
        newVisited_.clear();      newVisitedBytes_ = 0;
        newQueue_.clear();        newQueueBytes_   = 0;
    }

    [[nodiscard]] std::size_t visitedCount()  const { return visited_.size(); }
    [[nodiscard]] std::size_t knownCount()    const { return known_.size(); }
    /// Discovered but not yet crawled.
    [[nodiscard]] std::size_t pendingCount()  const { return known_.size() - visited_.size(); }
    [[nodiscard]] std::size_t newQueuedThisRun()  const { return newQueue_.size(); }
    /// URLs discovered this run and not yet written.  Used to prime the work
    /// queue, since forEachPending only sees chunks already on disk.
    [[nodiscard]] const std::vector<std::string>& newlyQueued() const { return newQueue_; }
    [[nodiscard]] std::size_t newVisitedThisRun() const { return newVisited_.size(); }

private:
    void load() {
        queueChunks_   = sortedChunks(queueDir_);
        auto visChunks = sortedChunks(visitedDir_);

        for (const auto& c : visChunks)
            readZipChunk(c, [&](std::string&& url) {
                std::uint64_t h = hashUrl(url);
                visited_.insert(h);
                known_.insert(h);
            });
        for (const auto& c : queueChunks_)
            readZipChunk(c, [&](std::string&& url) { known_.insert(hashUrl(url)); });

        nextVisitedSeq_ = visChunks.size();
        nextQueueSeq_   = queueChunks_.size();
    }

    /// Chunk files in `dir`, ordered by their numeric name so replay is stable.
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

    /// Split `urls` across as many new chunks as the size target requires.
    /// Chunk names are zero-padded so lexical order matches creation order.
    void writePending(const std::vector<std::string>& urls,
                      const fs::path& dir, const char* label) {
        if (urls.empty()) return;
        std::size_t& seq = (std::strcmp(label, "visited") == 0) ? nextVisitedSeq_ : nextQueueSeq_;

        std::vector<std::string> batch;
        std::size_t bytes = 0;
        auto emit = [&] {
            if (batch.empty()) return;
            char name[32];
            std::snprintf(name, sizeof(name), "%06zu.zip", seq++);
            writeZipChunk(dir / name, batch);
            batch.clear();
            bytes = 0;
        };
        for (const auto& u : urls) {
            batch.push_back(u);
            bytes += u.size() + 1;
            if (bytes >= chunkTarget_) emit();
        }
        emit();
    }

    fs::path    root_, visitedDir_, queueDir_;
    std::size_t chunkTarget_;

    HashSet64   known_;      // every URL ever seen (visited ∪ queued)
    HashSet64   visited_;    // crawled only

    std::vector<fs::path>    queueChunks_;
    std::vector<std::string> newVisited_, newQueue_;
    std::size_t newVisitedBytes_ = 0, newQueueBytes_ = 0;
    std::size_t nextVisitedSeq_  = 0, nextQueueSeq_  = 0;
};
