#include <gtest/gtest.h>

#include <cstring>
#include <filesystem>
#include <string>

#include "storage/disk_manager.h"
#include "storage/page.h"
#include "wal/recovery.h"
#include "wal/wal_manager.h"

namespace fs = std::filesystem;
using namespace litedb::storage;
using namespace litedb::wal;

namespace {

std::string TempBase(const char* tag) {
  return (fs::temp_directory_path() /
          ("litedb_wal_" + std::string(tag) + "_" +
           std::to_string(::getpid())))
      .string();
}

struct ScopedFiles {
  std::string db_path;
  std::string wal_path;
  ~ScopedFiles() {
    std::error_code ec;
    fs::remove(db_path, ec);
    fs::remove(wal_path, ec);
  }
};

}  // namespace

TEST(Wal, WriteAndReadBack) {
  ScopedFiles sf{TempBase("basic") + ".db", TempBase("basic") + ".wal"};
  WalManager wal(sf.wal_path);

  wal.log_begin(1);
  Page p;
  p.init(0);
  p.insert_tuple("wal-test-payload");
  wal.log_write(1, 0, p.data(), kPageSize);
  wal.log_commit(1);

  auto records = wal.read_all();
  ASSERT_EQ(records.size(), 3u);
  EXPECT_EQ(records[0].header.type, LogRecordType::Begin);
  EXPECT_EQ(records[1].header.type, LogRecordType::Write);
  EXPECT_EQ(records[2].header.type, LogRecordType::Commit);
}

TEST(Wal, RecoveryReplaysCommitted) {
  ScopedFiles sf{TempBase("recovery") + ".db", TempBase("recovery") + ".wal"};

  // Write a page via WAL but do NOT write to disk directly.
  {
    WalManager wal(sf.wal_path);
    DiskManager dm(sf.db_path);

    // Allocate page 0 on disk (just so file is big enough for reads).
    PageId pid = dm.allocate_page();
    Page blank;
    blank.init(pid);
    dm.write_page(pid, blank.data());
    dm.flush();

    // WAL logs for committed transaction 1.
    wal.log_begin(1);
    Page modified;
    modified.init(0);
    modified.set_lsn(1);
    modified.insert_tuple("survived-crash");
    wal.log_write(1, 0, modified.data(), kPageSize);
    wal.log_commit(1);

    // WAL logs for uncommitted transaction 2 — should NOT be replayed.
    wal.log_begin(2);
    Page bad;
    bad.init(0);
    bad.set_lsn(2);
    bad.insert_tuple("should-not-appear");
    wal.log_write(2, 0, bad.data(), kPageSize);
    // No commit for txn 2!
  }

  // Recovery.
  {
    WalManager wal(sf.wal_path);
    DiskManager dm(sf.db_path);
    Recovery recovery(wal, dm);
    std::size_t replayed = recovery.run();
    EXPECT_EQ(replayed, 1u);

    // Verify the recovered page.
    Page p;
    dm.read_page(0, p.data());
    EXPECT_EQ(p.lsn(), 1u);
    auto t = p.get_tuple(0);
    ASSERT_TRUE(t.has_value());
    EXPECT_EQ(*t, "survived-crash");
  }
}

TEST(Wal, RecoverySkipsAlreadyApplied) {
  ScopedFiles sf{TempBase("skip") + ".db", TempBase("skip") + ".wal"};

  {
    WalManager wal(sf.wal_path);
    DiskManager dm(sf.db_path);

    // Write page 0 with LSN=5 on disk (simulates previous flush).
    PageId pid = dm.allocate_page();
    Page already_flushed;
    already_flushed.init(pid);
    already_flushed.set_lsn(5);
    already_flushed.insert_tuple("original-data");
    dm.write_page(pid, already_flushed.data());
    dm.flush();

    // WAL has a write for page 0 with LSN=3 (already applied).
    wal.log_begin(1);
    Page older;
    older.init(0);
    older.set_lsn(3);
    older.insert_tuple("stale-redo");
    wal.log_write(1, 0, older.data(), kPageSize);
    wal.log_commit(1);
  }

  {
    WalManager wal(sf.wal_path);
    DiskManager dm(sf.db_path);
    Recovery recovery(wal, dm);
    std::size_t replayed = recovery.run();
    EXPECT_EQ(replayed, 0u);

    Page p;
    dm.read_page(0, p.data());
    EXPECT_EQ(p.lsn(), 5u);
    EXPECT_EQ(p.get_tuple(0).value(), "original-data");
  }
}
