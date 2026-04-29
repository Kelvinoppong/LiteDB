#include "storage/page.h"

#include <algorithm>

namespace litedb::storage {

Page::Page() {
  buffer_.fill(std::byte{0});
}

void Page::init(PageId page_id) {
  buffer_.fill(std::byte{0});
  auto& h = mutable_header();
  h.page_id = page_id;
  h.lsn = 0;
  h.slot_count = 0;
  h.free_space_offset = static_cast<uint16_t>(kPageSize);
  h.reserved = 0;
}

std::size_t Page::free_space() const {
  const auto& h = header();
  const std::size_t slot_dir_end = kPageHeaderSize + h.slot_count * kSlotSize;
  if (h.free_space_offset <= slot_dir_end) {
    return 0;
  }
  const std::size_t raw_free = h.free_space_offset - slot_dir_end;
  // Inserting a new tuple requires both the tuple bytes and one slot.
  return raw_free > kSlotSize ? raw_free - kSlotSize : 0;
}

std::optional<SlotId> Page::insert_tuple(std::string_view tuple) {
  if (tuple.size() > kPageSize) {
    return std::nullopt;
  }
  if (tuple.size() > free_space()) {
    return std::nullopt;
  }

  auto& h = mutable_header();
  const auto new_offset =
      static_cast<uint16_t>(h.free_space_offset - tuple.size());
  std::memcpy(buffer_.data() + new_offset, tuple.data(), tuple.size());

  const SlotId slot_id = h.slot_count;
  Slot& s = mutable_slot(slot_id);
  s.offset = new_offset;
  s.length = static_cast<uint16_t>(tuple.size());

  h.free_space_offset = new_offset;
  h.slot_count = static_cast<uint16_t>(h.slot_count + 1);
  return slot_id;
}

std::optional<std::string_view> Page::get_tuple(SlotId slot_id) const {
  if (slot_id >= header().slot_count) {
    return std::nullopt;
  }
  const Slot& s = slot(slot_id);
  if (s.length == 0) {
    return std::nullopt;
  }
  const auto* start =
      reinterpret_cast<const char*>(buffer_.data() + s.offset);
  return std::string_view{start, s.length};
}

bool Page::delete_tuple(SlotId slot_id) {
  if (slot_id >= header().slot_count) {
    return false;
  }
  Slot& s = mutable_slot(slot_id);
  if (s.length == 0) {
    return false;
  }
  s.length = 0;
  return true;
}

}  // namespace litedb::storage
