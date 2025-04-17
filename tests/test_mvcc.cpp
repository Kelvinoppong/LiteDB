#include <gtest/gtest.h>

#include <atomic>
#include <string>
#include <thread>
#include <vector>

#include "concurrency/mvcc.h"
#include "concurrency/transaction.h"

using namespace litedb::concurrency;

TEST(Mvcc, BasicInsertReadCommit) {
  TransactionManager tm;
  MvccTable table;

  auto* txn = tm.begin();
  RowId r1 = table.insert(txn, "hello");
  EXPECT_EQ(table.read(txn, r1).value(), "hello");
  tm.commit(txn);
  table.on_commit(txn);

  auto* txn2 = tm.begin();
  EXPECT_EQ(table.read(txn2, r1).value(), "hello");
  tm.commit(txn2);
}

TEST(Mvcc, SnapshotIsolation) {
  TransactionManager tm;
  MvccTable table;

  auto* writer = tm.begin();
  RowId r1 = table.insert(writer, "v1");
  tm.commit(writer);
  table.on_commit(writer);

  // Reader starts BEFORE the update commits.
  auto* reader = tm.begin();

  auto* updater = tm.begin();
  table.update(updater, r1, "v2");
  tm.commit(updater);
  table.on_commit(updater);

  // Reader should still see "v1" (snapshot isolation).
  EXPECT_EQ(table.read(reader, r1).value(), "v1");
  tm.commit(reader);

  // A new transaction sees "v2".
  auto* post = tm.begin();
  EXPECT_EQ(table.read(post, r1).value(), "v2");
  tm.commit(post);
}

TEST(Mvcc, UncommittedInsertIsInvisibleToOtherTransactions) {
  TransactionManager tm;
  MvccTable table;

  auto* writer = tm.begin();
  RowId r1 = table.insert(writer, "pending");

  auto* reader = tm.begin();
  EXPECT_FALSE(table.read(reader, r1).has_value());

  tm.commit(writer);
  table.on_commit(writer);

  // The reader keeps its original snapshot even after the writer commits.
  EXPECT_FALSE(table.read(reader, r1).has_value());
  tm.commit(reader);

  auto* fresh = tm.begin();
  EXPECT_EQ(table.read(fresh, r1).value(), "pending");
  tm.commit(fresh);
}

TEST(Mvcc, UncommittedUpdateDoesNotHideCommittedVersion) {
  TransactionManager tm;
  MvccTable table;

  auto* setup = tm.begin();
  RowId r1 = table.insert(setup, "old");
  tm.commit(setup);
  table.on_commit(setup);

  auto* writer = tm.begin();
  ASSERT_TRUE(table.update(writer, r1, "new"));

  auto* reader = tm.begin();
  EXPECT_EQ(table.read(reader, r1).value(), "old");

  tm.commit(writer);
  table.on_commit(writer);
  EXPECT_EQ(table.read(reader, r1).value(), "old");
  tm.commit(reader);
}

TEST(Mvcc, StaleWriterCannotOverwriteNewerCommittedVersion) {
  TransactionManager tm;
  MvccTable table;

  auto* setup = tm.begin();
  RowId r1 = table.insert(setup, "v1");
  tm.commit(setup);
  table.on_commit(setup);

  auto* stale = tm.begin();

  auto* newer = tm.begin();
  ASSERT_TRUE(table.update(newer, r1, "v2"));
  tm.commit(newer);
  table.on_commit(newer);

  EXPECT_FALSE(table.update(stale, r1, "stale-write"));
  tm.abort(stale);
  table.on_abort(stale);

  auto* reader = tm.begin();
  EXPECT_EQ(table.read(reader, r1).value(), "v2");
  tm.commit(reader);
}

TEST(Mvcc, DeleteMakesRowInvisible) {
  TransactionManager tm;
  MvccTable table;

  auto* txn1 = tm.begin();
  RowId r = table.insert(txn1, "data");
  tm.commit(txn1);
  table.on_commit(txn1);

  auto* txn2 = tm.begin();
  table.remove(txn2, r);
  tm.commit(txn2);
  table.on_commit(txn2);

  auto* txn3 = tm.begin();
  EXPECT_FALSE(table.read(txn3, r).has_value());
  tm.commit(txn3);
}

TEST(Mvcc, AbortRollsBackInsert) {
  TransactionManager tm;
  MvccTable table;

  auto* txn = tm.begin();
  RowId r = table.insert(txn, "ghost");
  tm.abort(txn);
  table.on_abort(txn);

  auto* reader = tm.begin();
  EXPECT_FALSE(table.read(reader, r).has_value());
  tm.commit(reader);
}

TEST(Mvcc, ConcurrentReadersNoBlocking) {
  TransactionManager tm;
  MvccTable table;

  // Pre-populate.
  auto* setup = tm.begin();
  for (int i = 0; i < 100; ++i) {
    table.insert(setup, "row-" + std::to_string(i));
  }
  tm.commit(setup);
  table.on_commit(setup);

  // One writer thread doing updates.
  std::atomic<bool> done{false};
  std::thread writer([&] {
    for (int i = 0; i < 200; ++i) {
      auto* w = tm.begin();
      RowId target = static_cast<RowId>((i % 100) + 1);
      table.update(w, target, "updated-" + std::to_string(i));
      tm.commit(w);
      table.on_commit(w);
    }
    done.store(true);
  });

  // 12 reader threads scanning concurrently.
  std::atomic<int> read_count{0};
  std::vector<std::thread> readers;
  for (int t = 0; t < 12; ++t) {
    readers.emplace_back([&] {
      while (!done.load()) {
        auto* r = tm.begin();
        auto rows = table.scan(r);
        // Every scan should see exactly 100 rows (snapshot isolation).
        EXPECT_EQ(rows.size(), 100u);
        read_count.fetch_add(1);
        tm.commit(r);
      }
    });
  }

  writer.join();
  for (auto& t : readers) t.join();

  // Sanity: must have completed many concurrent reads.
  EXPECT_GT(read_count.load(), 50);
}
