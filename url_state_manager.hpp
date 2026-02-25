/*
 * URL State Manager - RocksDB-backed URL state tracking for web crawlers
 *
 * Prerequisites (Ubuntu/Debian):
 *   sudo apt-get install librocksdb-dev
 *
 * Build (example test):
 *   g++ -std=c++23 -O2 -pthread url_state_manager_test.cpp -lrocksdb -o url_state_manager_test
 */

#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <rocksdb/db.h>
#include <rocksdb/filter_policy.h>
#include <rocksdb/options.h>
#include <rocksdb/table.h>
#include <rocksdb/utilities/checkpoint.h>
#include <rocksdb/write_batch.h>

namespace fs = std::filesystem;

// ── URL state enum ───────────────────────────────────────────────────────────

enum class UrlState : std::uint8_t {
    DISCOVERED = 0,
    CRAWLING   = 1,
    COMPLETED  = 2,
    FAILED     = 3
};

[[nodiscard]] inline std::string_view urlStateToString(UrlState s) {
    switch (s) {
        case UrlState::DISCOVERED: return "DISCOVERED";
        case UrlState::CRAWLING:   return "CRAWLING";
        case UrlState::COMPLETED:  return "COMPLETED";
        case UrlState::FAILED:     return "FAILED";
    }
    return "UNKNOWN";
}

// ── URL State Manager ────────────────────────────────────────────────────────

class UrlStateManager {
public:
    /// Open or create a RocksDB database at `dbPath`.
    /// If the directory already exists, it is opened; otherwise it is created.
    /// On startup the database integrity is verified.
    explicit UrlStateManager(const std::string& dbPath)
        : UrlStateManager(dbPath, /*autoVerify=*/true) {}

    /// Open or create a RocksDB database at `dbPath`, with optional
    /// startup integrity verification controlled by `autoVerify`.
    explicit UrlStateManager(const std::string& dbPath, bool autoVerify)
        : dbPath_(dbPath) {
        openDatabase();
        if (autoVerify) {
            verifyIntegrity();
        }
    }

    ~UrlStateManager() {
        try {
            seal();
        } catch (const std::exception& e) {
            std::cerr << "UrlStateManager: seal() failed in destructor: "
                      << e.what() << '\n';
        } catch (...) {
            std::cerr << "UrlStateManager: seal() failed in destructor (unknown error)\n";
        }
    }

    UrlStateManager(const UrlStateManager&)            = delete;
    UrlStateManager& operator=(const UrlStateManager&) = delete;

    // ── Core operations ──────────────────────────────────────────────────

    /// Thread-safe check-and-set: if the URL does not exist, insert it with
    /// the given state and return true.  If it already exists, return false.
    /// This is the only operation requiring a mutex (compound Get + Put).
    bool checkAndSet(const std::string& url, UrlState state) {
        std::lock_guard<std::mutex> lock(checkAndSetMu_);
        std::string existing;
        rocksdb::Status s = db_->Get(rocksdb::ReadOptions(), url, &existing);
        if (s.ok()) return false;           // already present
        if (!s.IsNotFound())
            throw std::runtime_error("checkAndSet Get failed: " + s.ToString());

        s = db_->Put(syncWriteOpts(), url, stateToValue(state));
        if (!s.ok())
            throw std::runtime_error("checkAndSet Put failed: " + s.ToString());
        return true;
    }

    /// Set (or overwrite) the state for a URL.
    /// RocksDB Put is natively thread-safe; no mutex needed.
    void setState(const std::string& url, UrlState state) {
        auto s = db_->Put(syncWriteOpts(), url, stateToValue(state));
        if (!s.ok())
            throw std::runtime_error("setState failed: " + s.ToString());
    }

    /// Retrieve the state for a URL.  Returns false if the URL is not found.
    /// RocksDB Get is natively thread-safe; no mutex needed.
    [[nodiscard]] bool getState(const std::string& url, UrlState& out) const {
        std::string value;
        auto s = db_->Get(rocksdb::ReadOptions(), url, &value);
        if (s.IsNotFound()) return false;
        if (!s.ok())
            throw std::runtime_error("getState failed: " + s.ToString());
        out = valueToState(value);
        return true;
    }

    /// Check whether a URL exists in the database.
    /// RocksDB Get is natively thread-safe; no mutex needed.
    [[nodiscard]] bool exists(const std::string& url) const {
        std::string value;
        auto s = db_->Get(rocksdb::ReadOptions(), url, &value);
        return s.ok();
    }

    /// Return all URLs that have a given state.
    /// RocksDB iterators are thread-safe; no mutex needed.
    [[nodiscard]] std::vector<std::string> getUrlsByState(UrlState state) const {
        std::vector<std::string> result;
        std::string target = stateToValue(state);
        std::unique_ptr<rocksdb::Iterator> it(
            db_->NewIterator(rocksdb::ReadOptions()));
        for (it->SeekToFirst(); it->Valid(); it->Next()) {
            if (it->value().ToString() == target)
                result.emplace_back(it->key().ToString());
        }
        if (!it->status().ok()) {
            throw std::runtime_error("Iterator error: " + it->status().ToString());
        }
        return result;
    }

    /// Invoke fn(url) for every URL that has the given state.
    /// Uses a RocksDB iterator and never loads all URLs into memory at once.
    void forEachUrlByState(UrlState state,
                           const std::function<void(const std::string&)>& fn) const {
        std::string target = stateToValue(state);
        std::unique_ptr<rocksdb::Iterator> it(
            db_->NewIterator(rocksdb::ReadOptions()));
        for (it->SeekToFirst(); it->Valid(); it->Next()) {
            if (it->value().ToString() == target)
                fn(it->key().ToString());
        }
        if (!it->status().ok())
            throw std::runtime_error("forEachUrlByState iterator error: " +
                                     it->status().ToString());
    }

    /// Count URLs that have a given state without loading them into memory.
    [[nodiscard]] size_t countByState(UrlState state) const {
        size_t n = 0;
        forEachUrlByState(state, [&](const std::string&) { ++n; });
        return n;
    }

    /// Invoke fn(url) for every URL in the database, regardless of state.
    void forEachUrl(const std::function<void(const std::string&)>& fn) const {
        std::unique_ptr<rocksdb::Iterator> it(
            db_->NewIterator(rocksdb::ReadOptions()));
        for (it->SeekToFirst(); it->Valid(); it->Next())
            fn(it->key().ToString());
        if (!it->status().ok())
            throw std::runtime_error("forEachUrl iterator error: " +
                                     it->status().ToString());
    }

    /// Bulk-delete a list of URLs using a single WriteBatch.
    /// Returns the number of URLs submitted for deletion.
    size_t bulkDelete(const std::vector<std::string>& urls) {
        if (urls.empty()) return 0;
        rocksdb::WriteBatch batch;
        for (const auto& url : urls)
            batch.Delete(url);
        auto s = db_->Write(syncWriteOpts(), &batch);
        if (!s.ok())
            throw std::runtime_error("bulkDelete failed: " + s.ToString());
        return urls.size();
    }

    /// Bulk import URLs with a given state using WriteBatch for efficiency.
    /// Only imports URLs that don't already exist (idempotent).
    /// Returns the number of URLs actually imported.
    size_t bulkImport(const std::vector<std::string>& urls, UrlState state) {
        std::lock_guard<std::mutex> lock(checkAndSetMu_);
        rocksdb::WriteBatch batch;
        std::string val = stateToValue(state);
        size_t count = 0;
        for (const auto& url : urls) {
            std::string existing;
            auto s = db_->Get(rocksdb::ReadOptions(), url, &existing);
            if (s.IsNotFound()) {
                batch.Put(url, val);
                ++count;
            }
        }
        if (count > 0) {
            auto s = db_->Write(syncWriteOpts(), &batch);
            if (!s.ok())
                throw std::runtime_error("bulkImport failed: " + s.ToString());
        }
        return count;
    }

    /// Fast bulk import for initial database population (e.g. after a cache miss).
    /// Skips per-key existence checks and WAL writes; last write wins on duplicates.
    /// Call flushAll() once after all chunks are imported to persist data to SST files.
    /// Returns the number of URLs written.
    size_t bulkImportFast(const std::vector<std::string>& urls, UrlState state) {
        if (urls.empty()) return 0;
        rocksdb::WriteBatch batch;
        std::string val = stateToValue(state);
        for (const auto& url : urls)
            batch.Put(url, val);
        rocksdb::WriteOptions wo;
        wo.disableWAL = true;
        auto s = db_->Write(wo, &batch);
        if (!s.ok())
            throw std::runtime_error("bulkImportFast failed: " + s.ToString());
        return urls.size();
    }

    /// Flush all memtable data to SST files.
    /// Call this once after all bulkImportFast() calls to ensure durability.
    void flushAll() {
        rocksdb::FlushOptions fo;
        fo.wait = true;
        auto s = db_->Flush(fo);
        if (!s.ok())
            throw std::runtime_error("flushAll failed: " + s.ToString());
    }

    // ── Checkpoint / Backup ──────────────────────────────────────────────

    /// Seal the database by creating a RocksDB Checkpoint.
    /// The checkpoint directory is `<dbPath>_checkpoint`.
    /// Idempotent: subsequent calls after the first are no-ops.
    void seal() {
        std::lock_guard<std::mutex> lock(sealMu_);
        if (!db_ || sealed_) return;

        std::string cpDir = dbPath_ + "_checkpoint";

        // Remove any previous checkpoint directory
        std::error_code ec;
        fs::remove_all(cpDir, ec);

        rocksdb::Checkpoint* rawCp = nullptr;
        auto s = rocksdb::Checkpoint::Create(db_.get(), &rawCp);
        if (!s.ok())
            throw std::runtime_error("Checkpoint::Create failed: " + s.ToString());
        std::unique_ptr<rocksdb::Checkpoint> cp(rawCp);

        s = cp->CreateCheckpoint(cpDir);
        if (!s.ok())
            throw std::runtime_error("CreateCheckpoint failed: " + s.ToString());

        sealed_ = true;
    }

    /// Verify database integrity using VerifyChecksum.
    /// RocksDB VerifyChecksum is thread-safe; no mutex needed.
    void verifyIntegrity() {
        auto s = db_->VerifyChecksum();
        if (!s.ok())
            throw std::runtime_error("Integrity check failed: " + s.ToString());
    }

    /// Return the underlying database path.
    [[nodiscard]] const std::string& path() const { return dbPath_; }

private:
    // ── Helpers ──────────────────────────────────────────────────────────

    void openDatabase() {
        rocksdb::BlockBasedTableOptions tableOpts;
        tableOpts.filter_policy.reset(rocksdb::NewBloomFilterPolicy(10));
        tableOpts.whole_key_filtering          = true;
        tableOpts.cache_index_and_filter_blocks = true;

        rocksdb::Options opts;
        opts.table_factory.reset(
            rocksdb::NewBlockBasedTableFactory(tableOpts));
        opts.create_if_missing = true;
        // GitHub's hard per-file limit is 100 MB.  Keep SST files at 40% of
        // that to stay safely under the limit on all levels.
        static constexpr std::uint64_t GITHUB_MAX_FILE_BYTES = 100ULL * 1024 * 1024;
        static constexpr std::uint64_t SST_TARGET_BYTES      = GITHUB_MAX_FILE_BYTES * 40 / 100;
        opts.target_file_size_base       = SST_TARGET_BYTES;
        opts.target_file_size_multiplier = 1; // same limit on every LSM level

        rocksdb::DB* raw = nullptr;
        auto s = rocksdb::DB::Open(opts, dbPath_, &raw);
        if (!s.ok())
            throw std::runtime_error("DB::Open failed: " + s.ToString());
        db_.reset(raw);
    }

    static std::string stateToValue(UrlState s) {
        return std::string(1, static_cast<char>(s));
    }

    /// Convert a stored byte back to UrlState.
    /// Requires the enum to be sequential from 0 (DISCOVERED) to 3 (FAILED).
    static UrlState valueToState(const std::string& v) {
        static_assert(static_cast<std::uint8_t>(UrlState::DISCOVERED) == 0);
        static_assert(static_cast<std::uint8_t>(UrlState::CRAWLING)   == 1);
        static_assert(static_cast<std::uint8_t>(UrlState::COMPLETED)  == 2);
        static_assert(static_cast<std::uint8_t>(UrlState::FAILED)     == 3);

        if (v.empty())
            throw std::runtime_error("valueToState: empty value");
        auto raw = static_cast<std::uint8_t>(v[0]);
        if (raw > static_cast<std::uint8_t>(UrlState::FAILED))
            throw std::runtime_error("valueToState: invalid state value");
        return static_cast<UrlState>(raw);
    }

    /// Return WriteOptions with sync=true for WAL durability.
    static rocksdb::WriteOptions syncWriteOpts() {
        rocksdb::WriteOptions wo;
        wo.sync = true;
        return wo;
    }

    std::string                    dbPath_;
    std::unique_ptr<rocksdb::DB>   db_;
    std::mutex                     checkAndSetMu_;  // only for atomic check-and-set
    std::mutex                     sealMu_;         // only for seal() filesystem operations
    bool                           sealed_ = false;
};
