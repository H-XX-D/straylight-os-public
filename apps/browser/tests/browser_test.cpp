// apps/browser/tests/browser_test.cpp
// StrayLight Browser — unit tests (no GTK/EGL/GL required)
#include <gtest/gtest.h>
#include "../downloads.h"

// Note: Engine::create and TabManager::new_tab require an initialised GTK +
// EGL context.  We test the pure-logic classes (Downloads, URL normalisation)
// without spinning up a display.

using namespace straylight::browser;

// ---------------------------------------------------------------------------
// Downloads — add
// ---------------------------------------------------------------------------

TEST(Downloads, AddExtractsFilename) {
    Downloads dl;
    size_t idx = dl.add("https://example.com/archive.tar.gz", "/tmp");
    ASSERT_EQ(idx, 0u);
    EXPECT_EQ(dl.entries()[0].filename,  "archive.tar.gz");
    EXPECT_EQ(dl.entries()[0].dest_path, "/tmp/archive.tar.gz");
    EXPECT_EQ(dl.entries()[0].status,    DownloadStatus::Pending);
}

TEST(Downloads, AddStripsQueryString) {
    Downloads dl;
    dl.add("https://files.example.com/setup.exe?version=2.0&token=abc", "/downloads");
    EXPECT_EQ(dl.entries()[0].filename, "setup.exe");
}

TEST(Downloads, AddFallbackFilename) {
    Downloads dl;
    dl.add("https://example.com/", "/tmp");
    EXPECT_FALSE(dl.entries()[0].filename.empty());
}

// ---------------------------------------------------------------------------
// Downloads — progress & state transitions
// ---------------------------------------------------------------------------

TEST(Downloads, UpdateProgress) {
    Downloads dl;
    size_t idx = dl.add("https://example.com/file.bin", "/tmp");
    dl.update_progress(idx, 500, 1000);
    EXPECT_EQ(dl.entries()[idx].bytes_received, 500u);
    EXPECT_EQ(dl.entries()[idx].bytes_total,    1000u);
    EXPECT_EQ(dl.entries()[idx].status,         DownloadStatus::Active);
}

TEST(Downloads, MarkComplete) {
    Downloads dl;
    size_t idx = dl.add("https://example.com/a.zip", "/tmp");
    dl.update_progress(idx, 100, 100);
    dl.mark_complete(idx);
    EXPECT_EQ(dl.entries()[idx].status,         DownloadStatus::Complete);
    EXPECT_EQ(dl.entries()[idx].bytes_received, dl.entries()[idx].bytes_total);
}

TEST(Downloads, MarkFailed) {
    Downloads dl;
    size_t idx = dl.add("https://example.com/b.zip", "/tmp");
    dl.mark_failed(idx, "connection reset");
    EXPECT_EQ(dl.entries()[idx].status,    DownloadStatus::Failed);
    EXPECT_EQ(dl.entries()[idx].error_msg, "connection reset");
}

TEST(Downloads, Cancel) {
    Downloads dl;
    size_t idx = dl.add("https://example.com/big.iso", "/tmp");
    dl.update_progress(idx, 100, 10000);
    dl.cancel(idx);
    EXPECT_EQ(dl.entries()[idx].status, DownloadStatus::Cancelled);
}

TEST(Downloads, CancelPending) {
    Downloads dl;
    size_t idx = dl.add("https://example.com/pending.tar", "/tmp");
    dl.cancel(idx);
    EXPECT_EQ(dl.entries()[idx].status, DownloadStatus::Cancelled);
}

TEST(Downloads, OutOfRangeNoop) {
    Downloads dl;
    // These should not crash
    dl.update_progress(99, 0, 0);
    dl.mark_complete(99);
    dl.mark_failed(99, "nope");
    dl.cancel(99);
    EXPECT_TRUE(dl.entries().empty());
}

// ---------------------------------------------------------------------------
// Downloads — multiple entries
// ---------------------------------------------------------------------------

TEST(Downloads, MultipleEntries) {
    Downloads dl;
    dl.add("https://example.com/1.mp3", "/music");
    dl.add("https://example.com/2.mp3", "/music");
    dl.add("https://example.com/3.mp3", "/music");
    ASSERT_EQ(dl.entries().size(), 3u);
    dl.mark_complete(0);
    dl.mark_complete(2);
    EXPECT_EQ(dl.entries()[0].status, DownloadStatus::Complete);
    EXPECT_EQ(dl.entries()[1].status, DownloadStatus::Pending);
    EXPECT_EQ(dl.entries()[2].status, DownloadStatus::Complete);
}

// ---------------------------------------------------------------------------
// Downloads — JSON persistence round-trip (writes to /tmp)
// ---------------------------------------------------------------------------

TEST(Downloads, SaveAndLoadHistory) {
    Downloads dl;
    dl.add("https://example.com/test.bin", "/tmp");
    dl.update_progress(0, 256, 512);

    auto save_res = dl.save_history();
    EXPECT_TRUE(save_res.has_value()) << save_res.error().message();

    Downloads dl2;
    auto load_res = dl2.load_history();
    EXPECT_TRUE(load_res.has_value()) << load_res.error().message();

    ASSERT_GE(dl2.entries().size(), 1u);
    bool found = false;
    for (auto& e : dl2.entries()) {
        if (e.url == "https://example.com/test.bin") {
            EXPECT_EQ(e.bytes_received, 256u);
            found = true;
        }
    }
    EXPECT_TRUE(found);
}
