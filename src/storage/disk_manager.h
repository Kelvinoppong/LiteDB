#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>

#include "storage/page.h"

namespace litedb::storage {

// DiskManager owns a single database file and provides page-granularity
// I/O. Pages are addressed by PageId, mapped to byte offset
// page_id * kPageSize. Allocation simply hands out the next page id and
// extends the file when written.
class DiskManager {
 public:
  // Opens (or creates) the database file at `path`. Throws std::system_error
  // on failure.
  explicit DiskManager(std::string path);
  ~DiskManager();

  DiskManager(const DiskManager&) = delete;
  DiskManager& operator=(const DiskManager&) = delete;

  // Reads kPageSize bytes for `page_id` into `out`. Throws if the page
  // is past end-of-file or I/O fails.
  void read_page(PageId page_id, std::byte* out);

  // Writes kPageSize bytes for `page_id` from `in`. The file is extended
  // automatically. fsync is left to the caller (see flush()).
  void write_page(PageId page_id, const std::byte* in);

  // Allocates the next page id. Does not actually touch the file; the
  // caller is expected to write the page shortly after.
  PageId allocate_page();

  // Total number of pages ever allocated.
  PageId num_pages() const { return next_page_id_.load(); }

  // fsync the file to durable storage.
  void flush();

  const std::string& path() const { return path_; }

 private:
  std::string path_;
  int fd_ = -1;
  std::atomic<PageId> next_page_id_{0};
  std::mutex io_mutex_;
};

}  // namespace litedb::storage
