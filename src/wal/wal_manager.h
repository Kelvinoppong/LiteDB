#pragma once

#include <cstdint>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

#include "storage/page.h"

namespace litedb::wal {

using Lsn = litedb::storage::Lsn;
using PageId = litedb::storage::PageId;
using TxnId = uint64_t;

enum class LogRecordType : uint8_t {
  Begin = 1,
  Commit = 2,
  Abort = 3,
  Write = 4,   // page-level redo image
  Checkpoint = 5,
};

#pragma pack(push, 1)
struct LogRecordHeader {
  Lsn lsn;
  TxnId txn_id;
  LogRecordType type;
  uint32_t data_length;
};
#pragma pack(pop)

// Redo-only write record payload: page_id + after_image.
#pragma pack(push, 1)
struct WritePayloadHeader {
  PageId page_id;
};
#pragma pack(pop)

struct LogRecord {
  LogRecordHeader header;
  std::vector<std::byte> data;
};

// Append-only WAL file. All writes are appended to the end. The WAL
// provides durability: committed transactions are guaranteed recoverable.
//
// This is a REDO-only implementation: we log the after-image of each page
// write. On recovery, we replay all committed transactions whose LSN is
// greater than the page's on-disk LSN.
class WalManager {
 public:
  explicit WalManager(std::string path);
  ~WalManager();

  WalManager(const WalManager&) = delete;
  WalManager& operator=(const WalManager&) = delete;

  Lsn next_lsn() const { return next_lsn_; }

  // Transaction lifecycle.
  Lsn log_begin(TxnId txn_id);
  Lsn log_commit(TxnId txn_id);
  Lsn log_abort(TxnId txn_id);

  // Logs a page write (after-image). Returns the LSN of this record.
  Lsn log_write(TxnId txn_id, PageId page_id, const std::byte* after_image,
                std::size_t len);

  // Logs a checkpoint marker.
  Lsn log_checkpoint();

  // Flushes the WAL to durable storage up to at least `up_to_lsn`.
  void flush(Lsn up_to_lsn = UINT64_MAX);

  // Reads all log records from the WAL file (for recovery).
  std::vector<LogRecord> read_all() const;

  const std::string& path() const { return path_; }

 private:
  Lsn append_record(TxnId txn_id, LogRecordType type,
                    const std::byte* data, std::size_t len);

  std::string path_;
  std::ofstream wal_file_;
  Lsn next_lsn_ = 1;
  mutable std::mutex mutex_;
};

}  // namespace litedb::wal
