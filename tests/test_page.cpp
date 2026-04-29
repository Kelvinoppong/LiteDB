#include <gtest/gtest.h>

#include <string>

#include "storage/page.h"

using litedb::storage::kPageSize;
using litedb::storage::Page;
using litedb::storage::SlotId;

TEST(Page, FreshPageIsEmpty) {
  Page p;
  p.init(/*page_id=*/7);
  EXPECT_EQ(p.page_id(), 7u);
  EXPECT_EQ(p.lsn(), 0u);
  EXPECT_EQ(p.slot_count(), 0u);
  EXPECT_EQ(p.free_space_offset(), kPageSize);
  EXPECT_GT(p.free_space(), kPageSize - 64);  // header + slot overhead is small
}

TEST(Page, InsertAndReadTuples) {
  Page p;
  p.init(0);

  auto a = p.insert_tuple("hello");
  auto b = p.insert_tuple("world");
  auto c = p.insert_tuple("");

  ASSERT_TRUE(a);
  ASSERT_TRUE(b);
  ASSERT_TRUE(c);
  EXPECT_EQ(*a, 0);
  EXPECT_EQ(*b, 1);
  EXPECT_EQ(*c, 2);

  EXPECT_EQ(p.get_tuple(*a).value(), "hello");
  EXPECT_EQ(p.get_tuple(*b).value(), "world");
  // Zero-length tuples are indistinguishable from tombstones in this
  // simple slot format and intentionally read back as nullopt.
  EXPECT_FALSE(p.get_tuple(*c).has_value());
}

TEST(Page, RejectsOversizedTuple) {
  Page p;
  p.init(0);
  std::string huge(kPageSize, 'x');
  EXPECT_FALSE(p.insert_tuple(huge).has_value());
}

TEST(Page, FillsUpAndRefuses) {
  Page p;
  p.init(0);
  std::string payload(64, 'a');
  std::size_t inserted = 0;
  while (p.insert_tuple(payload).has_value()) {
    ++inserted;
  }
  // Must have inserted enough tuples to exhaust the page, but not more
  // than the theoretical maximum.
  EXPECT_GT(inserted, 0u);
  EXPECT_LT(inserted * (payload.size() + 4) + 20, kPageSize + 64);
}

TEST(Page, DeleteTombstones) {
  Page p;
  p.init(0);
  auto id = p.insert_tuple("data");
  ASSERT_TRUE(id);
  ASSERT_TRUE(p.delete_tuple(*id));
  EXPECT_FALSE(p.get_tuple(*id).has_value());
  // Double-delete should be a no-op.
  EXPECT_FALSE(p.delete_tuple(*id));
}
