#pragma once

#include <array>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string_view>

namespace litedb::storage {

using PageId = uint32_t;
using Lsn = uint64_t;

inline constexpr std::size_t kPageSize = 4096;
inline constexpr PageId kInvalidPageId = static_cast<PageId>(-1);

// On-disk header laid out at the start of every page. Fixed size so the
// slot directory has a known starting offset.
#pragma pack(push, 1)
struct PageHeader {
  PageId page_id;
  Lsn lsn;
  uint16_t slot_count;
  uint16_t free_space_offset;  // tuples grow downward from page end; this
                               // is the offset of the lowest tuple byte.
  uint32_t reserved;
};
#pragma pack(pop)

static_assert(sizeof(PageHeader) == 20, "PageHeader must be 20 bytes");

#pragma pack(push, 1)
struct Slot {
  uint16_t offset;  // byte offset within the page where tuple starts
  uint16_t length;  // tuple length in bytes; 0 means tombstone
};
#pragma pack(pop)

static_assert(sizeof(Slot) == 4, "Slot must be 4 bytes");

inline constexpr std::size_t kPageHeaderSize = sizeof(PageHeader);
inline constexpr std::size_t kSlotSize = sizeof(Slot);

// SlotId is the index into the slot directory of a page.
using SlotId = uint16_t;

// A 4 KB slotted page. The page owns a fixed buffer and provides typed
// access into the header, slot directory, and tuple area. Pages are the
// unit transferred between the buffer pool and disk.
class Page {
 public:
  Page();

  // Initializes a brand-new page in-place. Wipes the buffer, sets the
  // page id, zeroes the slot count, and points free_space_offset at the
  // page end (= empty tuple area).
  void init(PageId page_id);

  PageId page_id() const { return header().page_id; }
  Lsn lsn() const { return header().lsn; }
  void set_lsn(Lsn lsn) { mutable_header().lsn = lsn; }

  uint16_t slot_count() const { return header().slot_count; }
  uint16_t free_space_offset() const { return header().free_space_offset; }

  // Bytes available for a brand new tuple, after accounting for the slot
  // that would also have to be inserted.
  std::size_t free_space() const;

  // Inserts a tuple and returns its slot id, or std::nullopt if the page
  // does not have enough free space.
  std::optional<SlotId> insert_tuple(std::string_view tuple);

  // Reads a tuple back out by slot id. Returns nullopt if the slot is
  // out of range or has been tombstoned (length == 0).
  std::optional<std::string_view> get_tuple(SlotId slot_id) const;

  // Marks the slot as a tombstone; does not reclaim space (compaction is
  // a later concern).
  bool delete_tuple(SlotId slot_id);

  // Raw access for the disk manager / buffer pool.
  std::byte* data() { return buffer_.data(); }
  const std::byte* data() const { return buffer_.data(); }
  static constexpr std::size_t size() { return kPageSize; }

 private:
  PageHeader& mutable_header() {
    return *reinterpret_cast<PageHeader*>(buffer_.data());
  }
  const PageHeader& header() const {
    return *reinterpret_cast<const PageHeader*>(buffer_.data());
  }

  Slot& mutable_slot(SlotId slot_id) {
    auto* slots = reinterpret_cast<Slot*>(buffer_.data() + kPageHeaderSize);
    return slots[slot_id];
  }
  const Slot& slot(SlotId slot_id) const {
    const auto* slots =
        reinterpret_cast<const Slot*>(buffer_.data() + kPageHeaderSize);
    return slots[slot_id];
  }

  std::array<std::byte, kPageSize> buffer_{};
};

}  // namespace litedb::storage
