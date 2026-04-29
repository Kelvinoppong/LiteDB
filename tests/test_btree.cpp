#include <gtest/gtest.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <numeric>
#include <random>
#include <string>
#include <vector>

#include "storage/btree.h"
#include "storage/buffer_pool.h"
#include "storage/disk_manager.h"

namespace fs = std::filesystem;
using namespace litedb::storage;

namespace {

std::string TempPath(const char* tag) {
  return (fs::temp_directory_path() /
          ("litedb_btree_" + std::string(tag) + "_" +
           std::to_string(::getpid()) + ".dat"))
      .string();
}

struct ScopedFile {
  std::string path;
  ~ScopedFile() {
    std::error_code ec;
    fs::remove(path, ec);
  }
};

}  // namespace

TEST(BTree, InsertAndSearch) {
  ScopedFile sf{TempPath("basic")};
  DiskManager dm(sf.path);
  BufferPool pool(dm, 256);
  BTree tree(pool);
  tree.create();

  tree.insert(10, 100);
  tree.insert(20, 200);
  tree.insert(5, 50);

  EXPECT_EQ(tree.search(10).value(), 100u);
  EXPECT_EQ(tree.search(20).value(), 200u);
  EXPECT_EQ(tree.search(5).value(), 50u);
  EXPECT_FALSE(tree.search(99).has_value());
}

TEST(BTree, OverwriteDuplicate) {
  ScopedFile sf{TempPath("dup")};
  DiskManager dm(sf.path);
  BufferPool pool(dm, 256);
  BTree tree(pool);
  tree.create();

  tree.insert(42, 1);
  tree.insert(42, 2);
  EXPECT_EQ(tree.search(42).value(), 2u);
}

TEST(BTree, RangeScan) {
  ScopedFile sf{TempPath("range")};
  DiskManager dm(sf.path);
  BufferPool pool(dm, 256);
  BTree tree(pool);
  tree.create();

  for (int64_t i = 0; i < 200; ++i) {
    tree.insert(i, static_cast<uint64_t>(i * 10));
  }

  auto result = tree.range_scan(50, 59);
  ASSERT_EQ(result.size(), 10u);
  for (std::size_t i = 0; i < result.size(); ++i) {
    EXPECT_EQ(result[i].first, static_cast<int64_t>(50 + i));
    EXPECT_EQ(result[i].second, static_cast<uint64_t>((50 + i) * 10));
  }
}

TEST(BTree, Insert100kRandomLookup10k) {
  ScopedFile sf{TempPath("100k")};
  DiskManager dm(sf.path);
  BufferPool pool(dm, 2048);
  BTree tree(pool);
  tree.create();

  constexpr int N = 100000;
  std::vector<int64_t> keys(N);
  std::iota(keys.begin(), keys.end(), 0);
  std::mt19937 rng(42);
  std::shuffle(keys.begin(), keys.end(), rng);

  for (int64_t k : keys) {
    tree.insert(k, static_cast<uint64_t>(k + 1));
  }

  // 10k random lookups.
  std::shuffle(keys.begin(), keys.end(), rng);
  for (int i = 0; i < 10000; ++i) {
    auto val = tree.search(keys[i]);
    ASSERT_TRUE(val.has_value()) << "key=" << keys[i];
    EXPECT_EQ(*val, static_cast<uint64_t>(keys[i] + 1));
  }

  // Range scan [500, 600].
  auto range = tree.range_scan(500, 600);
  ASSERT_EQ(range.size(), 101u);
  for (std::size_t i = 0; i < range.size(); ++i) {
    EXPECT_EQ(range[i].first, static_cast<int64_t>(500 + i));
  }
}
