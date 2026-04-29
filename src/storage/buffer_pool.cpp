#include "storage/buffer_pool.h"

#include <stdexcept>

namespace litedb::storage {

BufferPool::BufferPool(DiskManager& disk, std::size_t pool_size)
    : disk_(disk), frames_(pool_size) {
  if (pool_size == 0) {
    throw std::invalid_argument("BufferPool size must be > 0");
  }
  for (FrameId i = 0; i < pool_size; ++i) {
    free_list_.push_back(i);
  }
}

BufferPool::~BufferPool() {
  // Best-effort flush. Any I/O errors here would surface earlier in
  // explicit flush_all() calls; ignore them on destruction so that a
  // failure in one DB doesn't take down the process.
  try {
    flush_all();
  } catch (...) {
    // swallow
  }
}

void BufferPool::touch_lru(FrameId frame_id) {
  remove_from_lru(frame_id);
  lru_list_.push_back(frame_id);
  lru_index_[frame_id] = std::prev(lru_list_.end());
}

void BufferPool::remove_from_lru(FrameId frame_id) {
  auto it = lru_index_.find(frame_id);
  if (it != lru_index_.end()) {
    lru_list_.erase(it->second);
    lru_index_.erase(it);
  }
}

FrameId BufferPool::acquire_frame() {
  if (!free_list_.empty()) {
    FrameId frame_id = free_list_.front();
    free_list_.pop_front();
    return frame_id;
  }

  if (lru_list_.empty()) {
    throw std::runtime_error(
        "BufferPool exhausted: all frames are pinned");
  }

  FrameId victim = lru_list_.front();
  remove_from_lru(victim);

  Frame& f = frames_[victim];
  if (f.page_id != kInvalidPageId) {
    if (f.dirty) {
      disk_.write_page(f.page_id, f.page.data());
      f.dirty = false;
    }
    page_table_.erase(f.page_id);
    f.page_id = kInvalidPageId;
  }
  return victim;
}

Page& BufferPool::fetch_page(PageId page_id) {
  std::lock_guard<std::mutex> lock(mutex_);

  if (auto it = page_table_.find(page_id); it != page_table_.end()) {
    FrameId frame_id = it->second;
    Frame& f = frames_[frame_id];
    if (f.pin_count == 0) {
      remove_from_lru(frame_id);
    }
    ++f.pin_count;
    return f.page;
  }

  FrameId frame_id = acquire_frame();
  Frame& f = frames_[frame_id];
  disk_.read_page(page_id, f.page.data());
  f.page_id = page_id;
  f.pin_count = 1;
  f.dirty = false;
  page_table_[page_id] = frame_id;
  return f.page;
}

Page& BufferPool::new_page(PageId* out_page_id) {
  std::lock_guard<std::mutex> lock(mutex_);

  PageId page_id = disk_.allocate_page();
  FrameId frame_id = acquire_frame();
  Frame& f = frames_[frame_id];
  f.page.init(page_id);
  f.page_id = page_id;
  f.pin_count = 1;
  f.dirty = true;  // a freshly initialized page must be persisted.
  page_table_[page_id] = frame_id;

  // Reserve disk space immediately so subsequent disk reads against this
  // page id (e.g. from another BufferPool view in tests) succeed.
  disk_.write_page(page_id, f.page.data());
  f.dirty = false;

  if (out_page_id) {
    *out_page_id = page_id;
  }
  return f.page;
}

void BufferPool::unpin_page(PageId page_id, bool is_dirty) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = page_table_.find(page_id);
  if (it == page_table_.end()) {
    return;
  }
  FrameId frame_id = it->second;
  Frame& f = frames_[frame_id];
  if (f.pin_count <= 0) {
    throw std::runtime_error("unpin_page on already-unpinned page");
  }
  --f.pin_count;
  if (is_dirty) {
    f.dirty = true;
  }
  if (f.pin_count == 0) {
    touch_lru(frame_id);
  }
}

void BufferPool::flush_page(PageId page_id) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = page_table_.find(page_id);
  if (it == page_table_.end()) {
    return;
  }
  Frame& f = frames_[it->second];
  if (f.dirty) {
    disk_.write_page(f.page_id, f.page.data());
    f.dirty = false;
  }
}

void BufferPool::flush_all() {
  std::lock_guard<std::mutex> lock(mutex_);
  for (auto& [page_id, frame_id] : page_table_) {
    Frame& f = frames_[frame_id];
    if (f.dirty) {
      disk_.write_page(f.page_id, f.page.data());
      f.dirty = false;
    }
  }
  disk_.flush();
}

}  // namespace litedb::storage
