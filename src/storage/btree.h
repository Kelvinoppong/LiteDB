#pragma once

#include <cstdint>
#include <optional>
#include <vector>

#include "storage/buffer_pool.h"
#include "storage/page.h"

namespace litedb::storage {

using BTreeKey = int64_t;
using BTreeValue = uint64_t;

// Node type tags stored in the first byte of a B+ tree page's tuple area.
enum class BTreeNodeType : uint8_t {
  Internal = 1,
  Leaf = 2,
};

// Max keys per node. With 4 KB pages and 16-byte entries (8 key + 8 value/child),
// we can fit ~200 entries. We use a conservative order to leave room for
// metadata and simplify splits.
inline constexpr int kBTreeOrder = 128;
inline constexpr int kBTreeMinKeys = kBTreeOrder / 2;

// On-disk layout at the start of a B+ tree page (after the generic PageHeader).
// Stored as the first "tuple" in the slotted page.
#pragma pack(push, 1)
struct BTreeNodeHeader {
  BTreeNodeType type;
  uint16_t num_keys;
  PageId parent_page_id;
  PageId next_leaf;  // leaf linked-list pointer; kInvalidPageId if internal
  PageId prev_leaf;
};
#pragma pack(pop)

// In-memory representation of a B+ tree node for easy manipulation.
// Serialized to/from a slotted page for persistence.
struct BTreeNode {
  BTreeNodeType type;
  PageId self_page_id;
  PageId parent_page_id;
  PageId next_leaf;
  PageId prev_leaf;
  std::vector<BTreeKey> keys;
  // For leaf: values[i] corresponds to keys[i].
  // For internal: children[i] is the child pointer; children.size() == keys.size() + 1.
  std::vector<BTreeValue> values;
  std::vector<PageId> children;
};

// A B+ tree index that stores int64 keys mapped to uint64 values.
// Built on top of the buffer pool; each node occupies one page.
class BTree {
 public:
  explicit BTree(BufferPool& pool);

  // Creates a new empty tree and returns the root page id.
  PageId create();

  // Opens an existing tree rooted at `root_page_id`.
  void open(PageId root_page_id);

  // Inserts a key-value pair. Duplicate keys overwrite.
  void insert(BTreeKey key, BTreeValue value);

  // Point lookup. Returns nullopt if the key does not exist.
  std::optional<BTreeValue> search(BTreeKey key) const;

  // Range scan: returns all (key, value) pairs where lo <= key <= hi,
  // ordered by key.
  std::vector<std::pair<BTreeKey, BTreeValue>> range_scan(BTreeKey lo,
                                                          BTreeKey hi) const;

  PageId root_page_id() const { return root_page_id_; }

 private:
  BTreeNode read_node(PageId page_id) const;
  void write_node(const BTreeNode& node);
  PageId allocate_node(BTreeNodeType type);

  // Finds the leaf page that would contain `key`.
  PageId find_leaf(BTreeKey key) const;

  // Inserts into a leaf, splitting upward as needed.
  void insert_into_leaf(PageId leaf_page_id, BTreeKey key, BTreeValue value);
  void split_leaf(PageId leaf_page_id, BTreeNode& leaf);
  void insert_into_internal(PageId node_page_id, BTreeKey key,
                            PageId right_child);
  void split_internal(PageId node_page_id, BTreeNode& node);

  BufferPool& pool_;
  PageId root_page_id_ = kInvalidPageId;
};

}  // namespace litedb::storage
