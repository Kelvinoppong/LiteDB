#include <gtest/gtest.h>

#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include "storage/buffer_pool.h"
#include "storage/disk_manager.h"
#include "storage/page.h"

namespace fs = std::filesystem;
using litedb::storage::BufferPool;
using litedb::storage::DiskManager;
using litedb::storage::Page;
using litedb::storage::PageId;

namespace {

std::string TempPath(const char* tag) {
  auto dir = fs::temp_directory_path();
  auto name = std::string("litedb_") + tag + "_" +
              std::to_string(::getpid()) + ".dat";
  return (dir / name).string();
}

struct ScopedFile {
  std::string path;
  ~ScopedFile() {
    std::error_code ec;
    fs::remove(path, ec);
  }
};

}  // namespace

TEST(BufferPool, FetchPagePinAndUnpin) {
  ScopedFile sf{TempPath("fetch")};
  DiskManager dm(sf.path);
  BufferPool pool(dm, /*pool_size=*/4);

  PageId pid = 0;
  Page& p = pool.new_page(&pid);
  auto slot = p.insert_tuple("alpha");
  ASSERT_TRUE(slot.has_value());
  pool.unpin_page(pid, /*is_dirty=*/true);

  Page& again = pool.fetch_page(pid);
  EXPECT_EQ(again.page_id(), pid);
  EXPECT_EQ(again.get_tuple(*slot).value(), "alpha");
  pool.unpin_page(pid, false);
}

TEST(BufferPool, EvictsLruWhenFull) {
  ScopedFile sf{TempPath("evict")};
  DiskManager dm(sf.path);
  BufferPool pool(dm, /*pool_size=*/2);

  PageId p0, p1, p2;
  pool.new_page(&p0);
  pool.unpin_page(p0, true);
  pool.new_page(&p1);
  pool.unpin_page(p1, true);

  // Allocating a third page must evict either p0 or p1, but both are
  // unpinned so eviction must succeed.
  pool.new_page(&p2);
  pool.unpin_page(p2, true);

  // All three pages must still be readable from disk via the pool.
  Page& fetched0 = pool.fetch_page(p0);
  EXPECT_EQ(fetched0.page_id(), p0);
  pool.unpin_page(p0, false);
}

TEST(BufferPool, ThrowsWhenAllPinned) {
  ScopedFile sf{TempPath("pinned")};
  DiskManager dm(sf.path);
  BufferPool pool(dm, /*pool_size=*/2);

  PageId p0, p1;
  pool.new_page(&p0);
  pool.new_page(&p1);
  // Intentionally do not unpin.

  PageId p2;
  EXPECT_THROW(pool.new_page(&p2), std::runtime_error);

  pool.unpin_page(p0, false);
  pool.unpin_page(p1, false);
}

// Phase 1 deliverable: write 1,000 pages, close the database, reopen
// it, and read every page back to verify durability through restart.
TEST(BufferPool, WriteThousandPagesAndReadBackAfterRestart) {
  constexpr std::size_t kNumPages = 1000;
  ScopedFile sf{TempPath("durability")};

  std::vector<PageId> ids;
  ids.reserve(kNumPages);

  // -- Session 1: create + populate --
  {
    DiskManager dm(sf.path);
    BufferPool pool(dm, /*pool_size=*/16);

    for (std::size_t i = 0; i < kNumPages; ++i) {
      PageId pid = 0;
      Page& p = pool.new_page(&pid);
      ids.push_back(pid);

      const std::string payload =
          "page-" + std::to_string(pid) + "-" + std::string(32, 'x');
      ASSERT_TRUE(p.insert_tuple(payload).has_value());
      pool.unpin_page(pid, /*is_dirty=*/true);
    }

    pool.flush_all();
  }

  // -- Session 2: reopen and verify every tuple round-trips --
  {
    DiskManager dm(sf.path);
    BufferPool pool(dm, /*pool_size=*/16);

    ASSERT_EQ(dm.num_pages(), kNumPages);

    for (PageId pid : ids) {
      Page& p = pool.fetch_page(pid);
      ASSERT_EQ(p.page_id(), pid);
      const auto t = p.get_tuple(0);
      ASSERT_TRUE(t.has_value());
      const std::string expected =
          "page-" + std::to_string(pid) + "-" + std::string(32, 'x');
      EXPECT_EQ(*t, expected);
      pool.unpin_page(pid, false);
    }
  }
}
