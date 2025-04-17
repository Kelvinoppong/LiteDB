#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace litedb::concurrency {

using TxnId = uint64_t;
using Timestamp = uint64_t;

inline constexpr TxnId kInvalidTxnId = 0;
inline constexpr Timestamp kInvalidTimestamp = 0;

enum class TxnState : uint8_t {
  Active,
  Committed,
  Aborted,
};

struct Transaction {
  TxnId id;
  Timestamp begin_ts;
  Timestamp commit_ts;
  TxnState state;
};

// Global transaction manager. Allocates monotonically increasing txn ids
// and timestamps and tracks active/committed state for MVCC visibility.
class TransactionManager {
 public:
  TransactionManager();

  Transaction* begin();
  void commit(Transaction* txn);
  void abort(Transaction* txn);

  // Returns the minimum begin_ts among all currently active transactions.
  // Useful for garbage collection of old MVCC versions.
  Timestamp oldest_active_ts() const;

  // Snapshot: returns a copy of all active txn ids at call time.
  std::vector<TxnId> active_txn_ids() const;

 private:
  mutable std::mutex mutex_;
  std::atomic<TxnId> next_txn_id_{1};
  std::atomic<Timestamp> next_timestamp_{1};
  std::unordered_map<TxnId, std::unique_ptr<Transaction>> txns_;
};

}  // namespace litedb::concurrency
