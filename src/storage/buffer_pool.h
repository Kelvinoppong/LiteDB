#pragma once

#include <cstddef>
#include <list>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "storage/disk_manager.h"
#include "storage/page.h"

namespace litedb::storage {

// FrameId identifies a slot in the buffer pool's frame array.
using FrameId = std::size_t;

// One buffer pool frame: holds at most one Page plus its bookkeeping.
struct Frame {
  Page page;
  PageId page_id = kInvalidPageId;
  int pin_count = 0;
  bool dirty = false;
};

// BufferPool is a fixed-size cache of pages on top of a DiskManager. It
// uses a classic page table + LRU replacer:
//
//   - page_table_ maps PageId -> FrameId for resident pages.
//   - free_list_ holds frame ids that have never been used.
//   - lru_list_ holds frame ids whose pin_count == 0 (eviction candidates),
//     with most-recently-unpinned at the back.
//
// The deliverable for Phase 1 is correctness: pages are durable across a
// reopen, pinned pages are never evicted, and dirty pages are flushed on
// shutdown.
class BufferPool {
 public:
  BufferPool(DiskManager& disk, std::size_t pool_size);
  ~BufferPool();

  BufferPool(const BufferPool&) = delete;
  BufferPool& operator=(const BufferPool&) = delete;

  // Returns a pinned page for `page_id`, fetching it from disk if needed.
  // Throws std::runtime_error if every frame is pinned and there is no
  // candidate to evict. Caller must unpin_page() when done.
  Page& fetch_page(PageId page_id);

  // Allocates a fresh page and returns it pinned and freshly initialized.
  // The new page id is written through `out_page_id`.
  Page& new_page(PageId* out_page_id);

  // Decrements the pin count for `page_id`. If `is_dirty` is true the
  // dirty flag is set (and stays sticky until the page is flushed).
  void unpin_page(PageId page_id, bool is_dirty);

  // Writes a single page to disk if dirty. Does not unpin.
  void flush_page(PageId page_id);

  // Flushes every dirty resident page. Safe to call on shutdown.
  void flush_all();

  std::size_t pool_size() const { return frames_.size(); }

 private:
  // Locates a frame for a brand new resident page: either reuses one from
  // the free list or evicts the LRU candidate. Returns the chosen frame
  // id; the frame's existing tenant (if any) has already been flushed and
  // its page_table_ entry erased.
  FrameId acquire_frame();

  void touch_lru(FrameId frame_id);
  void remove_from_lru(FrameId frame_id);

  DiskManager& disk_;
  std::vector<Frame> frames_;
  std::unordered_map<PageId, FrameId> page_table_;

  std::list<FrameId> free_list_;
  std::list<FrameId> lru_list_;
  std::unordered_map<FrameId, std::list<FrameId>::iterator> lru_index_;

  std::mutex mutex_;
};

}  // namespace litedb::storage
