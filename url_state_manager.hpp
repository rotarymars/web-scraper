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
#include <iostream>
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
#include <rocksdb/utilities/backup_engine.h>

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
        : dbPath_(dbPath) {
        openDatabase();
        verifyIntegrity();
    }

    ~UrlStateManager() {
        seal();
        delete db_;
    }

    UrlStateManager(const UrlStateManager&)            = delete;
    UrlStateManager& operator=(const UrlStateManager&) = delete;

    // ── Core operations ──────────────────────────────────────────────────

    /// Thread-safe check-and-set: if the URL does not exist, insert it with
    /// the given state and return true.  If it already exists, return false.
    bool checkAndSet(const std::string& url, UrlState state) {
        std::lock_guard<std::mutex> lock(mu_);
        std::string existing;
        rocksdb::Status s = db_->Get(rocksdb::ReadOptions(), url, &existing);
        if (s.ok()) return false;           // already present
        if (!s.IsNotFound())
            throw std::runtime_error("checkAndSet Get failed: " + s.ToString());

        s = db_->Put(rocksdb::WriteOptions(), url, stateToValue(state));
        if (!s.ok())
            throw std::runtime_error("checkAndSet Put failed: " + s.ToString());
        return true;
    }

    /// Set (or overwrite) the state for a URL.
    void setState(const std::string& url, UrlState state) {
        std::lock_guard<std::mutex> lock(mu_);
        auto s = db_->Put(rocksdb::WriteOptions(), url, stateToValue(state));
        if (!s.ok())
            throw std::runtime_error("setState failed: " + s.ToString());
    }

    /// Retrieve the state for a URL.  Returns false if the URL is not found.
    [[nodiscard]] bool getState(const std::string& url, UrlState& out) const {
        std::lock_guard<std::mutex> lock(mu_);
        std::string value;
        auto s = db_->Get(rocksdb::ReadOptions(), url, &value);
        if (s.IsNotFound()) return false;
        if (!s.ok())
            throw std::runtime_error("getState failed: " + s.ToString());
        out = valueToState(value);
        return true;
    }

    /// Check whether a URL exists in the database.
    [[nodiscard]] bool exists(const std::string& url) const {
        std::lock_guard<std::mutex> lock(mu_);
        std::string value;
        auto s = db_->Get(rocksdb::ReadOptions(), url, &value);
        return s.ok();
    }

    /// Return all URLs that have a given state.
    [[nodiscard]] std::vector<std::string> getUrlsByState(UrlState state) const {
        std::lock_guard<std::mutex> lock(mu_);
        std::vector<std::string> result;
        std::string target = stateToValue(state);
        std::unique_ptr<rocksdb::Iterator> it(
            db_->NewIterator(rocksdb::ReadOptions()));
        for (it->SeekToFirst(); it->Valid(); it->Next()) {
            if (it->value().ToString() == target)
                result.emplace_back(it->key().ToString());
        }
        return result;
    }

    // ── Checkpoint / Backup ──────────────────────────────────────────────

    /// Seal the database by creating a RocksDB Checkpoint.
    /// The checkpoint directory is `<dbPath>_checkpoint`.
    void seal() {
        std::lock_guard<std::mutex> lock(mu_);
        if (!db_) return;

        std::string cpDir = dbPath_ + "_checkpoint";

        // Remove any previous checkpoint directory
        std::error_code ec;
        fs::remove_all(cpDir, ec);

        rocksdb::Checkpoint* rawCp = nullptr;
        auto s = rocksdb::Checkpoint::Create(db_, &rawCp);
        if (!s.ok())
            throw std::runtime_error("Checkpoint::Create failed: " + s.ToString());
        std::unique_ptr<rocksdb::Checkpoint> cp(rawCp);

        s = cp->CreateCheckpoint(cpDir);
        if (!s.ok())
            throw std::runtime_error("CreateCheckpoint failed: " + s.ToString());

        std::cout << "  Database sealed (checkpoint at " << cpDir << ")\n";
    }

    /// Verify database integrity using VerifyChecksum.
    void verifyIntegrity() {
        std::lock_guard<std::mutex> lock(mu_);
        auto s = db_->VerifyChecksum();
        if (!s.ok())
            throw std::runtime_error("Integrity check failed: " + s.ToString());
        std::cout << "  Database integrity verified.\n";
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

        bool dirExists = fs::is_directory(dbPath_);
        std::cout << (dirExists ? "  Opening existing" : "  Creating new")
                  << " database at " << dbPath_ << '\n';

        rocksdb::DB* raw = nullptr;
        auto s = rocksdb::DB::Open(opts, dbPath_, &raw);
        if (!s.ok())
            throw std::runtime_error("DB::Open failed: " + s.ToString());
        db_ = raw;
    }

    static std::string stateToValue(UrlState s) {
        return std::string(1, static_cast<char>(s));
    }

    static UrlState valueToState(const std::string& v) {
        if (v.empty())
            throw std::runtime_error("valueToState: empty value");
        auto raw = static_cast<std::uint8_t>(v[0]);
        if (raw > static_cast<std::uint8_t>(UrlState::FAILED))
            throw std::runtime_error("valueToState: invalid state value");
        return static_cast<UrlState>(raw);
    }

    std::string        dbPath_;
    rocksdb::DB*       db_ = nullptr;
    mutable std::mutex mu_;
};
