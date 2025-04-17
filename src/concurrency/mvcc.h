#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "concurrency/transaction.h"

namespace litedb::concurrency {

using RowId = uint64_t;

// A single version of a row. Versions form a singly-linked list (newest
// first) per RowId.
struct RowVersion {
  Timestamp begin_ts;
  Timestamp end_ts;  // UINT64_MAX means "latest / still live"
  TxnId created_by;
  bool deleted;
  std::string data;
  std::shared_ptr<RowVersion> prev;  // older version
};

// MVCC table: manages version chains for a set of rows. Not a full table
// implementation (Phase 5 adds schema), but sufficient to prove correct
// snapshot-isolation concurrency.
class MvccTable {
 public:
  MvccTable();

  // Inserts a new row. Returns its RowId.
  RowId insert(Transaction* txn, std::string data);

  // Updates a row (creates a new version).
  bool update(Transaction* txn, RowId row_id, std::string new_data);

  // Deletes a row (tombstone version).
  bool remove(Transaction* txn, RowId row_id);

  // Reads the visible version of a row for `txn`. Returns nullopt if
  // the row doesn't exist or is not visible.
  std::optional<std::string> read(const Transaction* txn, RowId row_id) const;

  // Scan all visible rows for a transaction.
  std::vector<std::pair<RowId, std::string>> scan(const Transaction* txn) const;

  // Called after a transaction commits to finalize version timestamps.
  void on_commit(Transaction* txn);

  // Called after a transaction aborts to remove uncommitted versions.
  void on_abort(Transaction* txn);

 private:
  std::shared_ptr<RowVersion> visible_version(const Transaction* txn,
                                              RowId row_id) const;

  mutable std::mutex mutex_;
  std::unordered_map<RowId, std::shared_ptr<RowVersion>> heads_;
  std::atomic<RowId> next_row_id_{1};
};

}  // namespace litedb::concurrency
