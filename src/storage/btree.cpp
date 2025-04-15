#include "storage/btree.h"

#include <algorithm>
#include <cassert>
#include <cstring>
#include <stdexcept>

namespace litedb::storage {

namespace {

// Serialization format for a B+ tree node inside a page:
//   [BTreeNodeHeader][keys...][values or children...]
// Keys are int64_t, values are uint64_t, children are PageId (uint32_t).

std::vector<std::byte> serialize_node(const BTreeNode& node) {
  BTreeNodeHeader hdr{};
  hdr.type = node.type;
  hdr.num_keys = static_cast<uint16_t>(node.keys.size());
  hdr.parent_page_id = node.parent_page_id;
  hdr.next_leaf = node.next_leaf;
  hdr.prev_leaf = node.prev_leaf;

  std::size_t size = sizeof(hdr);
  size += node.keys.size() * sizeof(BTreeKey);
  if (node.type == BTreeNodeType::Leaf) {
    size += node.values.size() * sizeof(BTreeValue);
  } else {
    size += node.children.size() * sizeof(PageId);
  }

  std::vector<std::byte> buf(size);
  auto* p = buf.data();
  std::memcpy(p, &hdr, sizeof(hdr));
  p += sizeof(hdr);

  std::memcpy(p, node.keys.data(), node.keys.size() * sizeof(BTreeKey));
  p += node.keys.size() * sizeof(BTreeKey);

  if (node.type == BTreeNodeType::Leaf) {
    std::memcpy(p, node.values.data(),
                node.values.size() * sizeof(BTreeValue));
  } else {
    std::memcpy(p, node.children.data(),
                node.children.size() * sizeof(PageId));
  }
  return buf;
}

BTreeNode deserialize_node(const std::byte* data, std::size_t len,
                           PageId self_page_id) {
  if (len < sizeof(BTreeNodeHeader)) {
    throw std::runtime_error("corrupt btree node: too short");
  }
  BTreeNodeHeader hdr{};
  std::memcpy(&hdr, data, sizeof(hdr));

  BTreeNode node{};
  node.type = hdr.type;
  node.self_page_id = self_page_id;
  node.parent_page_id = hdr.parent_page_id;
  node.next_leaf = hdr.next_leaf;
  node.prev_leaf = hdr.prev_leaf;

  const auto* p = data + sizeof(hdr);
  node.keys.resize(hdr.num_keys);
  std::memcpy(node.keys.data(), p, hdr.num_keys * sizeof(BTreeKey));
  p += hdr.num_keys * sizeof(BTreeKey);

  if (node.type == BTreeNodeType::Leaf) {
    node.values.resize(hdr.num_keys);
    std::memcpy(node.values.data(), p, hdr.num_keys * sizeof(BTreeValue));
  } else {
    std::size_t num_children = static_cast<std::size_t>(hdr.num_keys) + 1;
    node.children.resize(num_children);
    std::memcpy(node.children.data(), p, num_children * sizeof(PageId));
  }
  return node;
}

}  // namespace

BTree::BTree(BufferPool& pool) : pool_(pool) {}

PageId BTree::create() {
  root_page_id_ = allocate_node(BTreeNodeType::Leaf);
  return root_page_id_;
}

void BTree::open(PageId root_page_id) {
  root_page_id_ = root_page_id;
}

PageId BTree::allocate_node(BTreeNodeType type) {
  PageId pid = 0;
  Page& p = pool_.new_page(&pid);
  BTreeNode node{};
  node.type = type;
  node.self_page_id = pid;
  node.parent_page_id = kInvalidPageId;
  node.next_leaf = kInvalidPageId;
  node.prev_leaf = kInvalidPageId;
  write_node(node);
  pool_.unpin_page(pid, true);
  return pid;
}

BTreeNode BTree::read_node(PageId page_id) const {
  Page& p = const_cast<BufferPool&>(pool_).fetch_page(page_id);
  auto tuple = p.get_tuple(0);
  if (!tuple) {
    const_cast<BufferPool&>(pool_).unpin_page(page_id, false);
    throw std::runtime_error("corrupt btree page: no tuple at slot 0");
  }
  auto node = deserialize_node(
      reinterpret_cast<const std::byte*>(tuple->data()), tuple->size(),
      page_id);
  const_cast<BufferPool&>(pool_).unpin_page(page_id, false);
  return node;
}

void BTree::write_node(const BTreeNode& node) {
  Page& p = pool_.fetch_page(node.self_page_id);
  p.init(node.self_page_id);
  auto buf = serialize_node(node);
  std::string_view sv(reinterpret_cast<const char*>(buf.data()), buf.size());
  auto slot = p.insert_tuple(sv);
  if (!slot) {
    pool_.unpin_page(node.self_page_id, false);
    throw std::runtime_error("btree node too large for page");
  }
  pool_.unpin_page(node.self_page_id, true);
}

PageId BTree::find_leaf(BTreeKey key) const {
  PageId cur = root_page_id_;
  while (true) {
    BTreeNode node = read_node(cur);
    if (node.type == BTreeNodeType::Leaf) {
      return cur;
    }
    // Binary search for the child pointer.
    auto it = std::upper_bound(node.keys.begin(), node.keys.end(), key);
    std::size_t idx = static_cast<std::size_t>(it - node.keys.begin());
    cur = node.children[idx];
  }
}

std::optional<BTreeValue> BTree::search(BTreeKey key) const {
  if (root_page_id_ == kInvalidPageId) return std::nullopt;
  PageId leaf_id = find_leaf(key);
  BTreeNode leaf = read_node(leaf_id);
  auto it = std::lower_bound(leaf.keys.begin(), leaf.keys.end(), key);
  if (it != leaf.keys.end() && *it == key) {
    std::size_t idx = static_cast<std::size_t>(it - leaf.keys.begin());
    return leaf.values[idx];
  }
  return std::nullopt;
}

std::vector<std::pair<BTreeKey, BTreeValue>> BTree::range_scan(
    BTreeKey lo, BTreeKey hi) const {
  std::vector<std::pair<BTreeKey, BTreeValue>> result;
  if (root_page_id_ == kInvalidPageId) return result;

  PageId leaf_id = find_leaf(lo);
  while (leaf_id != kInvalidPageId) {
    BTreeNode leaf = read_node(leaf_id);
    for (std::size_t i = 0; i < leaf.keys.size(); ++i) {
      if (leaf.keys[i] > hi) return result;
      if (leaf.keys[i] >= lo) {
        result.emplace_back(leaf.keys[i], leaf.values[i]);
      }
    }
    leaf_id = leaf.next_leaf;
  }
  return result;
}

void BTree::insert(BTreeKey key, BTreeValue value) {
  if (root_page_id_ == kInvalidPageId) {
    create();
  }
  PageId leaf_id = find_leaf(key);
  insert_into_leaf(leaf_id, key, value);
}

void BTree::insert_into_leaf(PageId leaf_page_id, BTreeKey key,
                             BTreeValue value) {
  BTreeNode leaf = read_node(leaf_page_id);
  assert(leaf.type == BTreeNodeType::Leaf);

  auto it = std::lower_bound(leaf.keys.begin(), leaf.keys.end(), key);
  std::size_t idx = static_cast<std::size_t>(it - leaf.keys.begin());

  if (it != leaf.keys.end() && *it == key) {
    leaf.values[idx] = value;
    write_node(leaf);
    return;
  }

  leaf.keys.insert(leaf.keys.begin() + static_cast<long>(idx), key);
  leaf.values.insert(leaf.values.begin() + static_cast<long>(idx), value);
  write_node(leaf);

  if (static_cast<int>(leaf.keys.size()) > kBTreeOrder) {
    split_leaf(leaf_page_id, leaf);
  }
}

void BTree::split_leaf(PageId leaf_page_id, BTreeNode& leaf) {
  std::size_t mid = leaf.keys.size() / 2;

  // Allocate new right sibling.
  PageId new_pid = 0;
  Page& new_page = pool_.new_page(&new_pid);
  pool_.unpin_page(new_pid, false);

  BTreeNode right{};
  right.type = BTreeNodeType::Leaf;
  right.self_page_id = new_pid;
  right.parent_page_id = leaf.parent_page_id;
  right.next_leaf = leaf.next_leaf;
  right.prev_leaf = leaf_page_id;
  right.keys.assign(leaf.keys.begin() + static_cast<long>(mid),
                    leaf.keys.end());
  right.values.assign(leaf.values.begin() + static_cast<long>(mid),
                      leaf.values.end());

  // Update old next sibling's prev pointer.
  if (leaf.next_leaf != kInvalidPageId) {
    BTreeNode old_next = read_node(leaf.next_leaf);
    old_next.prev_leaf = new_pid;
    write_node(old_next);
  }

  leaf.keys.resize(mid);
  leaf.values.resize(mid);
  leaf.next_leaf = new_pid;
  write_node(leaf);
  write_node(right);

  BTreeKey promote_key = right.keys[0];

  if (leaf.parent_page_id == kInvalidPageId) {
    // Create new root.
    PageId new_root_pid = 0;
    Page& root_page = pool_.new_page(&new_root_pid);
    pool_.unpin_page(new_root_pid, false);

    BTreeNode new_root{};
    new_root.type = BTreeNodeType::Internal;
    new_root.self_page_id = new_root_pid;
    new_root.parent_page_id = kInvalidPageId;
    new_root.next_leaf = kInvalidPageId;
    new_root.prev_leaf = kInvalidPageId;
    new_root.keys.push_back(promote_key);
    new_root.children.push_back(leaf_page_id);
    new_root.children.push_back(new_pid);
    write_node(new_root);

    leaf.parent_page_id = new_root_pid;
    write_node(leaf);
    right.parent_page_id = new_root_pid;
    write_node(right);

    root_page_id_ = new_root_pid;
  } else {
    right.parent_page_id = leaf.parent_page_id;
    write_node(right);
    insert_into_internal(leaf.parent_page_id, promote_key, new_pid);
  }
}

void BTree::insert_into_internal(PageId node_page_id, BTreeKey key,
                                 PageId right_child) {
  BTreeNode node = read_node(node_page_id);
  assert(node.type == BTreeNodeType::Internal);

  auto it = std::upper_bound(node.keys.begin(), node.keys.end(), key);
  std::size_t idx = static_cast<std::size_t>(it - node.keys.begin());

  node.keys.insert(node.keys.begin() + static_cast<long>(idx), key);
  node.children.insert(
      node.children.begin() + static_cast<long>(idx) + 1, right_child);
  write_node(node);

  if (static_cast<int>(node.keys.size()) > kBTreeOrder) {
    split_internal(node_page_id, node);
  }
}

void BTree::split_internal(PageId node_page_id, BTreeNode& node) {
  std::size_t mid = node.keys.size() / 2;
  BTreeKey promote_key = node.keys[mid];

  PageId new_pid = 0;
  Page& new_page = pool_.new_page(&new_pid);
  pool_.unpin_page(new_pid, false);

  BTreeNode right{};
  right.type = BTreeNodeType::Internal;
  right.self_page_id = new_pid;
  right.parent_page_id = node.parent_page_id;
  right.next_leaf = kInvalidPageId;
  right.prev_leaf = kInvalidPageId;
  right.keys.assign(node.keys.begin() + static_cast<long>(mid) + 1,
                    node.keys.end());
  right.children.assign(node.children.begin() + static_cast<long>(mid) + 1,
                        node.children.end());

  // Update children's parent pointers.
  for (PageId child_pid : right.children) {
    BTreeNode child = read_node(child_pid);
    child.parent_page_id = new_pid;
    write_node(child);
  }

  node.keys.resize(mid);
  node.children.resize(mid + 1);
  write_node(node);
  write_node(right);

  if (node.parent_page_id == kInvalidPageId) {
    PageId new_root_pid = 0;
    Page& root_page = pool_.new_page(&new_root_pid);
    pool_.unpin_page(new_root_pid, false);

    BTreeNode new_root{};
    new_root.type = BTreeNodeType::Internal;
    new_root.self_page_id = new_root_pid;
    new_root.parent_page_id = kInvalidPageId;
    new_root.next_leaf = kInvalidPageId;
    new_root.prev_leaf = kInvalidPageId;
    new_root.keys.push_back(promote_key);
    new_root.children.push_back(node_page_id);
    new_root.children.push_back(new_pid);
    write_node(new_root);

    node.parent_page_id = new_root_pid;
    write_node(node);
    right.parent_page_id = new_root_pid;
    write_node(right);

    root_page_id_ = new_root_pid;
  } else {
    insert_into_internal(node.parent_page_id, promote_key, new_pid);
  }
}

}  // namespace litedb::storage
