#pragma once

#include <string>

#include "storage/disk_manager.h"
#include "wal/wal_manager.h"

namespace litedb::wal {

// REDO-only crash recovery: replays committed write records whose LSN
// exceeds the on-disk page LSN. Uncommitted (no Commit record) writes
// are simply skipped — since we haven't flushed those pages, the on-disk
// state is consistent.
class Recovery {
 public:
  Recovery(WalManager& wal, storage::DiskManager& disk);

  // Runs recovery. Returns the number of pages replayed.
  std::size_t run();

 private:
  WalManager& wal_;
  storage::DiskManager& disk_;
};

}  // namespace litedb::wal
