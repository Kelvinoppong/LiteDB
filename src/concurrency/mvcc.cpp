#include "concurrency/mvcc.h"

#include <limits>

namespace litedb::concurrency {

static constexpr Timestamp kInfinity = std::numeric_limits<Timestamp>::max();

MvccTable::MvccTable() = default;

bool MvccTable::can_write_locked(const Transaction* txn, RowId row_id) const {
  auto it = heads_.find(row_id);
  if (it == heads_.end()) return false;

  auto old_head = it->second;
  return old_head->created_by == txn->id ||
         !(old_head->begin_ts == kInfinity ||
           old_head->begin_ts > txn->begin_ts);
}

RowId MvccTable::insert(Transaction* txn, std::string data) {
  std::lock_guard<std::mutex> lock(mutex_);
  RowId rid = next_row_id_.fetch_add(1);
  auto ver = std::make_shared<RowVersion>();
  ver->begin_ts = kInfinity;
  ver->end_ts = kInfinity;
  ver->created_by = txn->id;
  ver->deleted = false;
  ver->data = std::move(data);
  ver->prev = nullptr;
  heads_[rid] = ver;
  return rid;
}

bool MvccTable::update(Transaction* txn, RowId row_id, std::string new_data) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = heads_.find(row_id);
  if (it == heads_.end()) return false;

  // Write-write conflict: if the head was created by another active txn
  // or by a transaction newer than this snapshot, we cannot update it
  // (simple first-writer-wins).
  if (!can_write_locked(txn, row_id)) {
    return false;
  }
  auto old_head = it->second;

  auto new_ver = std::make_shared<RowVersion>();
  new_ver->begin_ts = kInfinity;
  new_ver->end_ts = kInfinity;
  new_ver->created_by = txn->id;
  new_ver->deleted = false;
  new_ver->data = std::move(new_data);
  new_ver->prev = old_head;

  it->second = new_ver;
  return true;
}

bool MvccTable::remove(Transaction* txn, RowId row_id) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = heads_.find(row_id);
  if (it == heads_.end()) return false;

  if (!can_write_locked(txn, row_id)) {
    return false;
  }
  auto old_head = it->second;

  auto del_ver = std::make_shared<RowVersion>();
  del_ver->begin_ts = kInfinity;
  del_ver->end_ts = kInfinity;
  del_ver->created_by = txn->id;
  del_ver->deleted = true;
  del_ver->data = "";
  del_ver->prev = old_head;

  it->second = del_ver;
  return true;
}

std::shared_ptr<RowVersion> MvccTable::visible_version(
    const Transaction* txn, RowId row_id) const {
  auto it = heads_.find(row_id);
  if (it == heads_.end()) return nullptr;

  auto ver = it->second;
  while (ver) {
    // A version is visible if:
    //   1) It was created by this same transaction, OR
    //   2) it has committed before this transaction's snapshot and has not
    //      ended before the snapshot.
    if (ver->created_by == txn->id) {
      return ver;
    }
    if (ver->begin_ts != kInfinity && ver->begin_ts <= txn->begin_ts &&
        ver->end_ts > txn->begin_ts) {
      return ver;
    }
    ver = ver->prev;
  }
  return nullptr;
}

std::optional<std::string> MvccTable::read(const Transaction* txn,
                                           RowId row_id) const {
  std::lock_guard<std::mutex> lock(mutex_);
  auto ver = visible_version(txn, row_id);
  if (!ver || ver->deleted) return std::nullopt;
  return ver->data;
}

std::vector<std::pair<RowId, std::string>> MvccTable::scan(
    const Transaction* txn) const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<std::pair<RowId, std::string>> results;
  for (const auto& [rid, _] : heads_) {
    auto ver = visible_version(txn, rid);
    if (ver && !ver->deleted) {
      results.emplace_back(rid, ver->data);
    }
  }
  return results;
}

bool MvccTable::can_write(const Transaction* txn, RowId row_id) const {
  std::lock_guard<std::mutex> lock(mutex_);
  return can_write_locked(txn, row_id);
}

void MvccTable::on_commit(Transaction* txn) {
  std::lock_guard<std::mutex> lock(mutex_);
  // Finalize: set begin_ts of versions created by this txn to commit_ts
  // and close the committed predecessor at the same timestamp.
  for (auto& [rid, head] : heads_) {
    auto ver = head;
    while (ver) {
      if (ver->created_by == txn->id) {
        ver->begin_ts = txn->commit_ts;
        if (ver->prev && ver->prev->created_by != txn->id &&
            ver->prev->end_ts == kInfinity) {
          ver->prev->end_ts = txn->commit_ts;
        }
      }
      ver = ver->prev;
    }
  }
}

void MvccTable::on_abort(Transaction* txn) {
  std::lock_guard<std::mutex> lock(mutex_);
  // Remove leading versions created by the aborted transaction and restore
  // the committed predecessor as the live head.
  std::vector<RowId> to_erase;
  for (auto& [rid, head] : heads_) {
    while (head && head->created_by == txn->id) {
      head = head->prev;
    }
    if (head) {
      head->end_ts = kInfinity;
      heads_[rid] = head;
    } else {
      to_erase.push_back(rid);
    }
  }
  for (RowId rid : to_erase) {
    heads_.erase(rid);
  }
}

}  // namespace litedb::concurrency
