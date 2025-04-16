#include "wal/recovery.h"

#include <cstring>
#include <unordered_set>

#include "storage/page.h"

namespace litedb::wal {

Recovery::Recovery(WalManager& wal, storage::DiskManager& disk)
    : wal_(wal), disk_(disk) {}

std::size_t Recovery::run() {
  auto records = wal_.read_all();

  // First pass: determine which transactions committed.
  std::unordered_set<TxnId> committed;
  for (const auto& rec : records) {
    if (rec.header.type == LogRecordType::Commit) {
      committed.insert(rec.header.txn_id);
    }
  }

  // Second pass: redo committed writes where the page LSN < record LSN.
  std::size_t replayed = 0;
  for (const auto& rec : records) {
    if (rec.header.type != LogRecordType::Write) continue;
    if (committed.find(rec.header.txn_id) == committed.end()) continue;

    if (rec.data.size() < sizeof(WritePayloadHeader)) continue;
    WritePayloadHeader ph{};
    std::memcpy(&ph, rec.data.data(), sizeof(ph));

    const std::byte* after_image = rec.data.data() + sizeof(ph);
    std::size_t image_len = rec.data.size() - sizeof(ph);
    if (image_len < storage::kPageSize) continue;

    // Read the on-disk page to check its LSN.
    storage::Page disk_page;
    if (ph.page_id < disk_.num_pages()) {
      disk_.read_page(ph.page_id, disk_page.data());
      if (disk_page.lsn() >= rec.header.lsn) {
        continue;  // already applied
      }
    }

    // Apply the after-image.
    disk_.write_page(ph.page_id, after_image);
    ++replayed;
  }

  disk_.flush();
  return replayed;
}

}  // namespace litedb::wal
