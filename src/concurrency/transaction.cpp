#include "concurrency/transaction.h"

#include <limits>
#include <stdexcept>

namespace litedb::concurrency {

TransactionManager::TransactionManager() = default;

Transaction* TransactionManager::begin() {
  std::lock_guard<std::mutex> lock(mutex_);
  TxnId id = next_txn_id_.fetch_add(1);
  Timestamp ts = next_timestamp_.fetch_add(1);
  auto txn =
      std::make_unique<Transaction>(Transaction{id, ts, kInvalidTimestamp,
                                                TxnState::Active});
  Transaction* ptr = txn.get();
  txns_.emplace(id, std::move(txn));
  return ptr;
}

void TransactionManager::commit(Transaction* txn) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (txn->state != TxnState::Active) {
    throw std::runtime_error("commit on non-active transaction");
  }
  txn->commit_ts = next_timestamp_.fetch_add(1);
  txn->state = TxnState::Committed;
}

void TransactionManager::abort(Transaction* txn) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (txn->state != TxnState::Active) {
    throw std::runtime_error("abort on non-active transaction");
  }
  txn->state = TxnState::Aborted;
}

Timestamp TransactionManager::oldest_active_ts() const {
  std::lock_guard<std::mutex> lock(mutex_);
  Timestamp min_ts = std::numeric_limits<Timestamp>::max();
  for (const auto& [id, txn] : txns_) {
    if (txn->state == TxnState::Active) {
      min_ts = std::min(min_ts, txn->begin_ts);
    }
  }
  return min_ts;
}

std::vector<TxnId> TransactionManager::active_txn_ids() const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<TxnId> result;
  for (const auto& [id, txn] : txns_) {
    if (txn->state == TxnState::Active) {
      result.push_back(id);
    }
  }
  return result;
}

}  // namespace litedb::concurrency
