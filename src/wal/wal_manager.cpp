#include "wal/wal_manager.h"

#include <cstring>
#include <fstream>
#include <stdexcept>

namespace litedb::wal {

WalManager::WalManager(std::string path) : path_(std::move(path)) {
  // Open for reading to determine the current end (next_lsn_).
  {
    std::ifstream in(path_, std::ios::binary);
    if (in.good()) {
      while (true) {
        LogRecordHeader hdr{};
        in.read(reinterpret_cast<char*>(&hdr), sizeof(hdr));
        if (!in.good()) break;
        in.seekg(hdr.data_length, std::ios::cur);
        if (!in.good()) break;
        if (hdr.lsn >= next_lsn_) {
          next_lsn_ = hdr.lsn + 1;
        }
      }
    }
  }
  wal_file_.open(path_, std::ios::binary | std::ios::app);
  if (!wal_file_.is_open()) {
    throw std::runtime_error("failed to open WAL file: " + path_);
  }
}

WalManager::~WalManager() {
  if (wal_file_.is_open()) {
    wal_file_.flush();
    wal_file_.close();
  }
}

Lsn WalManager::append_record(TxnId txn_id, LogRecordType type,
                               const std::byte* data, std::size_t len) {
  std::lock_guard<std::mutex> lock(mutex_);
  LogRecordHeader hdr{};
  hdr.lsn = next_lsn_++;
  hdr.txn_id = txn_id;
  hdr.type = type;
  hdr.data_length = static_cast<uint32_t>(len);
  wal_file_.write(reinterpret_cast<const char*>(&hdr), sizeof(hdr));
  if (len > 0 && data != nullptr) {
    wal_file_.write(reinterpret_cast<const char*>(data),
                    static_cast<std::streamsize>(len));
  }
  return hdr.lsn;
}

Lsn WalManager::log_begin(TxnId txn_id) {
  return append_record(txn_id, LogRecordType::Begin, nullptr, 0);
}

Lsn WalManager::log_commit(TxnId txn_id) {
  Lsn lsn = append_record(txn_id, LogRecordType::Commit, nullptr, 0);
  flush(lsn);
  return lsn;
}

Lsn WalManager::log_abort(TxnId txn_id) {
  return append_record(txn_id, LogRecordType::Abort, nullptr, 0);
}

Lsn WalManager::log_write(TxnId txn_id, PageId page_id,
                           const std::byte* after_image, std::size_t len) {
  WritePayloadHeader ph{};
  ph.page_id = page_id;
  std::vector<std::byte> payload(sizeof(ph) + len);
  std::memcpy(payload.data(), &ph, sizeof(ph));
  std::memcpy(payload.data() + sizeof(ph), after_image, len);
  return append_record(txn_id, LogRecordType::Write, payload.data(),
                       payload.size());
}

Lsn WalManager::log_checkpoint() {
  Lsn lsn = append_record(0, LogRecordType::Checkpoint, nullptr, 0);
  flush(lsn);
  return lsn;
}

void WalManager::flush(Lsn /*up_to_lsn*/) {
  std::lock_guard<std::mutex> lock(mutex_);
  wal_file_.flush();
}

std::vector<LogRecord> WalManager::read_all() const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<LogRecord> records;
  std::ifstream in(path_, std::ios::binary);
  if (!in.good()) return records;

  while (true) {
    LogRecord rec{};
    in.read(reinterpret_cast<char*>(&rec.header), sizeof(rec.header));
    if (!in.good()) break;
    if (rec.header.data_length > 0) {
      rec.data.resize(rec.header.data_length);
      in.read(reinterpret_cast<char*>(rec.data.data()),
              rec.header.data_length);
      if (!in.good()) break;
    }
    records.push_back(std::move(rec));
  }
  return records;
}

}  // namespace litedb::wal
