/*
 * Tests for UrlStateManager (RocksDB-backed URL state tracking)
 *
 * Build:
 *   g++ -std=c++23 -O2 -pthread url_state_manager_test.cpp -lrocksdb -o url_state_manager_test
 *
 * Run:
 *   ./url_state_manager_test
 */

#include "url_state_manager.hpp"

#include <atomic>
#include <cassert>
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

static const std::string TEST_DB = "/tmp/url_state_manager_test_db";

// Clean up any leftover test data before/after each test.
static void cleanup() {
    std::error_code ec;
    fs::remove_all(TEST_DB, ec);
    fs::remove_all(TEST_DB + "_checkpoint", ec);
}

// ── Test helpers ─────────────────────────────────────────────────────────────

static int passed = 0;
static int failed = 0;

#define RUN_TEST(fn)                                                     \
    do {                                                                 \
        cleanup();                                                       \
        std::cout << "  Running " #fn "... ";                            \
        try {                                                            \
            fn();                                                        \
            std::cout << "PASSED\n";                                     \
            ++passed;                                                    \
        } catch (const std::exception& e) {                              \
            std::cout << "FAILED: " << e.what() << '\n';                 \
            ++failed;                                                    \
        }                                                                \
        cleanup();                                                       \
    } while (0)

// ── Tests ────────────────────────────────────────────────────────────────────

void test_create_new_db() {
    assert(!fs::exists(TEST_DB));
    UrlStateManager mgr(TEST_DB);
    assert(fs::is_directory(TEST_DB));
}

void test_open_existing_db() {
    // Create, close, reopen.
    {
        UrlStateManager mgr(TEST_DB);
        mgr.checkAndSet("http://example.com", UrlState::DISCOVERED);
    }
    assert(fs::is_directory(TEST_DB));
    {
        UrlStateManager mgr(TEST_DB);
        UrlState s{};
        assert(mgr.getState("http://example.com", s));
        assert(s == UrlState::DISCOVERED);
    }
}

void test_check_and_set() {
    UrlStateManager mgr(TEST_DB);
    bool added = mgr.checkAndSet("http://a.com", UrlState::DISCOVERED);
    assert(added);
    // Second insert with same key should fail.
    bool added2 = mgr.checkAndSet("http://a.com", UrlState::CRAWLING);
    assert(!added2);
    // Original state should be unchanged.
    UrlState s{};
    assert(mgr.getState("http://a.com", s));
    assert(s == UrlState::DISCOVERED);
}

void test_set_state() {
    UrlStateManager mgr(TEST_DB);
    mgr.checkAndSet("http://b.com", UrlState::DISCOVERED);
    mgr.setState("http://b.com", UrlState::COMPLETED);
    UrlState s{};
    assert(mgr.getState("http://b.com", s));
    assert(s == UrlState::COMPLETED);
}

void test_exists() {
    UrlStateManager mgr(TEST_DB);
    assert(!mgr.exists("http://no.com"));
    mgr.checkAndSet("http://yes.com", UrlState::DISCOVERED);
    assert(mgr.exists("http://yes.com"));
}

void test_get_urls_by_state() {
    UrlStateManager mgr(TEST_DB);
    mgr.checkAndSet("http://1.com", UrlState::DISCOVERED);
    mgr.checkAndSet("http://2.com", UrlState::CRAWLING);
    mgr.checkAndSet("http://3.com", UrlState::DISCOVERED);
    mgr.checkAndSet("http://4.com", UrlState::COMPLETED);

    auto discovered = mgr.getUrlsByState(UrlState::DISCOVERED);
    assert(discovered.size() == 2);
    auto crawling = mgr.getUrlsByState(UrlState::CRAWLING);
    assert(crawling.size() == 1);
    auto completed = mgr.getUrlsByState(UrlState::COMPLETED);
    assert(completed.size() == 1);
    auto failedUrls = mgr.getUrlsByState(UrlState::FAILED);
    assert(failedUrls.empty());
}

void test_all_states() {
    UrlStateManager mgr(TEST_DB);
    mgr.checkAndSet("http://d.com", UrlState::DISCOVERED);
    mgr.checkAndSet("http://c.com", UrlState::CRAWLING);
    mgr.checkAndSet("http://co.com", UrlState::COMPLETED);
    mgr.checkAndSet("http://f.com", UrlState::FAILED);

    UrlState s{};
    assert(mgr.getState("http://d.com", s));  assert(s == UrlState::DISCOVERED);
    assert(mgr.getState("http://c.com", s));  assert(s == UrlState::CRAWLING);
    assert(mgr.getState("http://co.com", s)); assert(s == UrlState::COMPLETED);
    assert(mgr.getState("http://f.com", s));  assert(s == UrlState::FAILED);
}

void test_seal_and_checkpoint() {
    {
        UrlStateManager mgr(TEST_DB);
        mgr.checkAndSet("http://seal.com", UrlState::COMPLETED);
        // First explicit seal should create the checkpoint.
        mgr.seal();
        // Second explicit seal should be safe/idempotent and not create a duplicate checkpoint.
        mgr.seal();
        assert(fs::is_directory(TEST_DB + "_checkpoint"));
    }
}

void test_verify_integrity() {
    UrlStateManager mgr(TEST_DB);
    mgr.checkAndSet("http://int.com", UrlState::DISCOVERED);
    // Should not throw.
    mgr.verifyIntegrity();
}

void test_thread_safety() {
    UrlStateManager mgr(TEST_DB);
    constexpr int numThreads = 8;
    constexpr int urlsPerThread = 100;

    std::vector<std::thread> threads;
    threads.reserve(numThreads);

    for (int t = 0; t < numThreads; ++t) {
        threads.emplace_back([&mgr, t] {
            for (int i = 0; i < urlsPerThread; ++i) {
                std::string url = "http://thread" + std::to_string(t)
                                + "/page" + std::to_string(i);
                mgr.checkAndSet(url, UrlState::DISCOVERED);
                mgr.setState(url, UrlState::CRAWLING);
                mgr.setState(url, UrlState::COMPLETED);
            }
        });
    }
    for (auto& t : threads) t.join();

    // Every URL should be COMPLETED.
    auto completed = mgr.getUrlsByState(UrlState::COMPLETED);
    assert(static_cast<int>(completed.size()) == numThreads * urlsPerThread);
}

void test_concurrent_check_and_set() {
    UrlStateManager mgr(TEST_DB);
    constexpr int numThreads = 8;
    std::atomic<int> wins{0};

    // All threads try to insert the same URL; exactly one should succeed.
    std::vector<std::thread> threads;
    threads.reserve(numThreads);
    for (int t = 0; t < numThreads; ++t) {
        threads.emplace_back([&] {
            if (mgr.checkAndSet("http://race.com", UrlState::DISCOVERED))
                ++wins;
        });
    }
    for (auto& t : threads) t.join();
    assert(wins.load() == 1);
}

void test_url_state_to_string() {
    assert(urlStateToString(UrlState::DISCOVERED) == "DISCOVERED");
    assert(urlStateToString(UrlState::CRAWLING)   == "CRAWLING");
    assert(urlStateToString(UrlState::COMPLETED)  == "COMPLETED");
    assert(urlStateToString(UrlState::FAILED)     == "FAILED");
}

void test_bulk_import_fast() {
    UrlStateManager mgr(TEST_DB);
    std::vector<std::string> urls;
    for (int i = 0; i < 1000; ++i)
        urls.push_back("http://fast-import-" + std::to_string(i) + ".com");

    size_t n = mgr.bulkImportFast(urls, UrlState::COMPLETED);
    assert(n == 1000);
    mgr.flushAll();   // ensure no-WAL data is persisted to SST

    // Verify a sample of entries
    UrlState s{};
    assert(mgr.getState("http://fast-import-0.com",   s)); assert(s == UrlState::COMPLETED);
    assert(mgr.getState("http://fast-import-999.com", s)); assert(s == UrlState::COMPLETED);

    // Verify total count via iteration
    auto completed = mgr.getUrlsByState(UrlState::COMPLETED);
    assert(completed.size() == 1000);
}

void test_bulk_import_fast_last_write_wins() {
    UrlStateManager mgr(TEST_DB);
    // Import same URL as COMPLETED then re-import as DISCOVERED; DISCOVERED wins.
    std::vector<std::string> urls = {"http://overwrite-test.com"};
    mgr.bulkImportFast(urls, UrlState::COMPLETED);
    mgr.bulkImportFast(urls, UrlState::DISCOVERED);
    mgr.flushAll();

    UrlState s{};
    assert(mgr.getState("http://overwrite-test.com", s));
    assert(s == UrlState::DISCOVERED);
}

// ── Main ─────────────────────────────────────────────────────────────────────

int main() {
    std::cout << "=== UrlStateManager Tests ===\n";

    RUN_TEST(test_create_new_db);
    RUN_TEST(test_open_existing_db);
    RUN_TEST(test_check_and_set);
    RUN_TEST(test_set_state);
    RUN_TEST(test_exists);
    RUN_TEST(test_get_urls_by_state);
    RUN_TEST(test_all_states);
    RUN_TEST(test_seal_and_checkpoint);
    RUN_TEST(test_verify_integrity);
    RUN_TEST(test_thread_safety);
    RUN_TEST(test_concurrent_check_and_set);
    RUN_TEST(test_url_state_to_string);
    RUN_TEST(test_bulk_import_fast);
    RUN_TEST(test_bulk_import_fast_last_write_wins);

    std::cout << "\n=== Results: " << passed << " passed, "
              << failed << " failed ===\n";
    return failed > 0 ? 1 : 0;
}
