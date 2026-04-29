#include "storage/disk_manager.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <cerrno>
#include <stdexcept>
#include <system_error>
#include <utility>

namespace litedb::storage {

namespace {

void pwrite_full(int fd, const void* buf, std::size_t len, off_t offset) {
  const auto* p = static_cast<const char*>(buf);
  std::size_t remaining = len;
  while (remaining > 0) {
    ssize_t n = ::pwrite(fd, p, remaining, offset);
    if (n < 0) {
      if (errno == EINTR) continue;
      throw std::system_error(errno, std::generic_category(), "pwrite");
    }
    if (n == 0) {
      throw std::runtime_error("pwrite returned 0");
    }
    p += n;
    offset += n;
    remaining -= static_cast<std::size_t>(n);
  }
}

void pread_full(int fd, void* buf, std::size_t len, off_t offset) {
  auto* p = static_cast<char*>(buf);
  std::size_t remaining = len;
  while (remaining > 0) {
    ssize_t n = ::pread(fd, p, remaining, offset);
    if (n < 0) {
      if (errno == EINTR) continue;
      throw std::system_error(errno, std::generic_category(), "pread");
    }
    if (n == 0) {
      throw std::runtime_error("pread hit EOF before requested length");
    }
    p += n;
    offset += n;
    remaining -= static_cast<std::size_t>(n);
  }
}

}  // namespace

DiskManager::DiskManager(std::string path) : path_(std::move(path)) {
  fd_ = ::open(path_.c_str(), O_RDWR | O_CREAT, 0644);
  if (fd_ < 0) {
    throw std::system_error(errno, std::generic_category(),
                            "open " + path_);
  }
  struct stat st{};
  if (::fstat(fd_, &st) < 0) {
    int saved = errno;
    ::close(fd_);
    fd_ = -1;
    throw std::system_error(saved, std::generic_category(), "fstat");
  }
  next_page_id_.store(static_cast<PageId>(st.st_size / kPageSize));
}

DiskManager::~DiskManager() {
  if (fd_ >= 0) {
    ::close(fd_);
  }
}

void DiskManager::read_page(PageId page_id, std::byte* out) {
  std::lock_guard<std::mutex> lock(io_mutex_);
  const off_t offset = static_cast<off_t>(page_id) * kPageSize;
  pread_full(fd_, out, kPageSize, offset);
}

void DiskManager::write_page(PageId page_id, const std::byte* in) {
  std::lock_guard<std::mutex> lock(io_mutex_);
  const off_t offset = static_cast<off_t>(page_id) * kPageSize;
  pwrite_full(fd_, in, kPageSize, offset);
  if (page_id >= next_page_id_.load()) {
    next_page_id_.store(page_id + 1);
  }
}

PageId DiskManager::allocate_page() {
  return next_page_id_.fetch_add(1);
}

void DiskManager::flush() {
  std::lock_guard<std::mutex> lock(io_mutex_);
  if (::fsync(fd_) < 0) {
    throw std::system_error(errno, std::generic_category(), "fsync");
  }
}

}  // namespace litedb::storage
