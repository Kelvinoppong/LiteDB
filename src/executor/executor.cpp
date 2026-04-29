#include "executor/executor.h"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <unordered_set>

#include "storage/page.h"
#include "wal/recovery.h"

namespace litedb::executor {

namespace {

constexpr std::string_view kSnapshotMagic = "LDBSNAP1";
constexpr std::size_t kSnapshotChunkSize =
    storage::kPageSize - storage::kPageHeaderSize - storage::kSlotSize;

const char* column_type_name(parser::ColumnType type) {
  switch (type) {
    case parser::ColumnType::Int:
      return "INT";
    case parser::ColumnType::Text:
      return "TEXT";
  }
  return "UNKNOWN";
}

bool value_matches_column(const Value& value, const parser::ColumnDef& column) {
  if (column.type == parser::ColumnType::Int) {
    return std::holds_alternative<int64_t>(value);
  }
  return std::holds_alternative<std::string>(value);
}

void append_u8(std::string& out, uint8_t value) {
  out.push_back(static_cast<char>(value));
}

void append_u32(std::string& out, uint32_t value) {
  out.append(reinterpret_cast<const char*>(&value), sizeof(value));
}

void append_u64(std::string& out, uint64_t value) {
  out.append(reinterpret_cast<const char*>(&value), sizeof(value));
}

void append_string(std::string& out, std::string_view value) {
  if (value.size() > std::numeric_limits<uint32_t>::max()) {
    throw std::runtime_error("snapshot string too large");
  }
  append_u32(out, static_cast<uint32_t>(value.size()));
  out.append(value.data(), value.size());
}

uint8_t read_u8(const std::string& in, std::size_t& pos) {
  if (pos + 1 > in.size()) throw std::runtime_error("corrupt snapshot");
  return static_cast<uint8_t>(in[pos++]);
}

uint32_t read_u32(const std::string& in, std::size_t& pos) {
  if (pos + sizeof(uint32_t) > in.size()) {
    throw std::runtime_error("corrupt snapshot");
  }
  uint32_t value = 0;
  std::memcpy(&value, in.data() + pos, sizeof(value));
  pos += sizeof(value);
  return value;
}

uint64_t read_u64(const std::string& in, std::size_t& pos) {
  if (pos + sizeof(uint64_t) > in.size()) {
    throw std::runtime_error("corrupt snapshot");
  }
  uint64_t value = 0;
  std::memcpy(&value, in.data() + pos, sizeof(value));
  pos += sizeof(value);
  return value;
}

std::string read_string(const std::string& in, std::size_t& pos) {
  uint32_t size = read_u32(in, pos);
  if (pos + size > in.size()) throw std::runtime_error("corrupt snapshot");
  std::string value = in.substr(pos, size);
  pos += size;
  return value;
}

}  // namespace

Executor::Executor() = default;

Executor::Executor(std::string db_path)
    : persistent_(true), db_path_(std::move(db_path)) {
  disk_ = std::make_unique<storage::DiskManager>(db_path_);
  wal_ = std::make_unique<wal::WalManager>(db_path_ + ".wal");
  wal::Recovery recovery(*wal_, *disk_);
  recovery.run();
  next_persist_txn_id_ = wal_->next_lsn();
  load_snapshot();
}

concurrency::Transaction* Executor::begin_transaction() {
  return txn_mgr_.begin();
}

void Executor::commit_transaction(concurrency::Transaction* txn) {
  if (txn == nullptr) {
    throw std::runtime_error("commit requires an active transaction");
  }
  std::lock_guard<std::mutex> lock(mutex_);
  txn_mgr_.commit(txn);
  for (auto& [name, tbl] : tables_) tbl.on_commit(txn);
  persist_snapshot();
}

void Executor::rollback_transaction(concurrency::Transaction* txn) {
  if (txn == nullptr) {
    throw std::runtime_error("rollback requires an active transaction");
  }
  std::lock_guard<std::mutex> lock(mutex_);
  txn_mgr_.abort(txn);
  for (auto& [name, tbl] : tables_) tbl.on_abort(txn);
}

// Row serialization: simple length-prefixed fields.
// Format per value: 1 byte type tag (0=int, 1=string) + payload.
// Int: 8 bytes. String: 4-byte length + char data.

std::string Executor::serialize_row(const Row& row,
                                    const TableSchema& /*schema*/) const {
  std::string buf;
  for (const auto& val : row) {
    if (std::holds_alternative<int64_t>(val)) {
      buf += '\x00';
      int64_t v = std::get<int64_t>(val);
      buf.append(reinterpret_cast<const char*>(&v), sizeof(v));
    } else {
      buf += '\x01';
      const auto& s = std::get<std::string>(val);
      uint32_t len = static_cast<uint32_t>(s.size());
      buf.append(reinterpret_cast<const char*>(&len), sizeof(len));
      buf.append(s);
    }
  }
  return buf;
}

Row Executor::deserialize_row(const std::string& data,
                              const TableSchema& /*schema*/) const {
  Row row;
  std::size_t pos = 0;
  while (pos < data.size()) {
    char tag = data[pos++];
    if (tag == '\x00') {
      int64_t v;
      std::memcpy(&v, data.data() + pos, sizeof(v));
      pos += sizeof(v);
      row.emplace_back(v);
    } else {
      uint32_t len;
      std::memcpy(&len, data.data() + pos, sizeof(len));
      pos += sizeof(len);
      row.emplace_back(data.substr(pos, len));
      pos += len;
    }
  }
  return row;
}

Value Executor::eval_expr(const parser::Expr* expr, const Row& row,
                          const TableSchema& schema) const {
  if (!expr) return int64_t{0};
  switch (expr->kind) {
    case parser::Expr::Kind::IntLiteral:
      return expr->int_val;
    case parser::Expr::Kind::StrLiteral:
      return expr->str_val;
    case parser::Expr::Kind::ColumnRef: {
      for (std::size_t i = 0; i < schema.columns.size(); ++i) {
        if (schema.columns[i].name == expr->str_val) {
          if (i >= row.size()) {
            throw std::runtime_error("column reference not available: " +
                                     expr->str_val);
          }
          return row[i];
        }
      }
      throw std::runtime_error("unknown column: " + expr->str_val);
    }
    case parser::Expr::Kind::BinaryOp: {
      auto lv = eval_expr(expr->left.get(), row, schema);
      auto rv = eval_expr(expr->right.get(), row, schema);
      // Arithmetic.
      if (expr->op == parser::BinaryOp::Plus) {
        return std::get<int64_t>(lv) + std::get<int64_t>(rv);
      }
      if (expr->op == parser::BinaryOp::Minus) {
        return std::get<int64_t>(lv) - std::get<int64_t>(rv);
      }
      // Logical.
      if (expr->op == parser::BinaryOp::And) {
        return int64_t{(std::get<int64_t>(lv) != 0 &&
                        std::get<int64_t>(rv) != 0)
                           ? 1
                           : 0};
      }
      if (expr->op == parser::BinaryOp::Or) {
        return int64_t{(std::get<int64_t>(lv) != 0 ||
                        std::get<int64_t>(rv) != 0)
                           ? 1
                           : 0};
      }
      // Comparison — works for both int and string via variant ordering.
      int cmp = 0;
      if (std::holds_alternative<int64_t>(lv) &&
          std::holds_alternative<int64_t>(rv)) {
        int64_t a = std::get<int64_t>(lv), b = std::get<int64_t>(rv);
        cmp = (a < b) ? -1 : (a > b) ? 1 : 0;
      } else {
        auto a = std::holds_alternative<std::string>(lv)
                     ? std::get<std::string>(lv)
                     : std::to_string(std::get<int64_t>(lv));
        auto b = std::holds_alternative<std::string>(rv)
                     ? std::get<std::string>(rv)
                     : std::to_string(std::get<int64_t>(rv));
        cmp = a.compare(b);
      }
      bool result = false;
      switch (expr->op) {
        case parser::BinaryOp::Eq: result = cmp == 0; break;
        case parser::BinaryOp::Neq: result = cmp != 0; break;
        case parser::BinaryOp::Lt: result = cmp < 0; break;
        case parser::BinaryOp::Gt: result = cmp > 0; break;
        case parser::BinaryOp::LtEq: result = cmp <= 0; break;
        case parser::BinaryOp::GtEq: result = cmp >= 0; break;
        default: break;
      }
      return int64_t{result ? 1 : 0};
    }
  }
  return int64_t{0};
}

bool Executor::eval_where(const parser::Expr* expr, const Row& row,
                          const TableSchema& schema) const {
  if (!expr) return true;
  Value v = eval_expr(expr, row, schema);
  if (std::holds_alternative<int64_t>(v)) {
    return std::get<int64_t>(v) != 0;
  }
  return !std::get<std::string>(v).empty();
}

QueryResult Executor::execute(const parser::Statement& stmt,
                              concurrency::Transaction* txn) {
  std::lock_guard<std::mutex> lock(mutex_);

  // Auto-commit wrapper.
  bool is_txn_control = std::holds_alternative<parser::BeginStmt>(stmt) ||
                        std::holds_alternative<parser::CommitStmt>(stmt) ||
                        std::holds_alternative<parser::RollbackStmt>(stmt);
  bool is_mutating = std::holds_alternative<parser::CreateTableStmt>(stmt) ||
                     std::holds_alternative<parser::InsertStmt>(stmt) ||
                     std::holds_alternative<parser::UpdateStmt>(stmt) ||
                     std::holds_alternative<parser::DeleteStmt>(stmt);
  bool auto_commit = (txn == nullptr && !is_txn_control);
  concurrency::Transaction* active_txn = txn;
  if (auto_commit) {
    active_txn = txn_mgr_.begin();
  }

  QueryResult result;
  try {
    if (auto* s = std::get_if<parser::CreateTableStmt>(&stmt)) {
      if (txn != nullptr) {
        result.success = false;
        result.error = "CREATE TABLE is not supported inside a transaction";
      } else {
        result = exec_create(*s);
      }
    } else if (auto* s = std::get_if<parser::InsertStmt>(&stmt)) {
      result = exec_insert(*s, active_txn);
    } else if (auto* s = std::get_if<parser::SelectStmt>(&stmt)) {
      result = exec_select(*s, active_txn);
    } else if (auto* s = std::get_if<parser::UpdateStmt>(&stmt)) {
      result = exec_update(*s, active_txn);
    } else if (auto* s = std::get_if<parser::DeleteStmt>(&stmt)) {
      result = exec_delete(*s, active_txn);
    } else if (std::holds_alternative<parser::BeginStmt>(stmt)) {
      result.status_message = "BEGIN";
    } else if (std::holds_alternative<parser::CommitStmt>(stmt)) {
      result.status_message = "COMMIT";
    } else if (std::holds_alternative<parser::RollbackStmt>(stmt)) {
      result.status_message = "ROLLBACK";
    }
  } catch (const std::exception& e) {
    result.success = false;
    result.error = e.what();
    if (auto_commit) {
      txn_mgr_.abort(active_txn);
      for (auto& [name, tbl] : tables_) tbl.on_abort(active_txn);
    }
    return result;
  }

  if (auto_commit) {
    if (result.success) {
      txn_mgr_.commit(active_txn);
      for (auto& [name, tbl] : tables_) tbl.on_commit(active_txn);
      if (is_mutating) {
        persist_snapshot();
      }
    } else {
      txn_mgr_.abort(active_txn);
      for (auto& [name, tbl] : tables_) tbl.on_abort(active_txn);
    }
  }
  return result;
}

void Executor::load_snapshot() {
  if (!persistent_ || disk_->num_pages() == 0) return;

  storage::Page meta_page;
  disk_->read_page(0, meta_page.data());
  auto meta_tuple = meta_page.get_tuple(0);
  if (!meta_tuple.has_value()) return;

  std::string meta(meta_tuple->data(), meta_tuple->size());
  std::size_t pos = 0;
  std::string magic = read_string(meta, pos);
  if (magic != kSnapshotMagic) {
    return;
  }
  uint64_t snapshot_size = read_u64(meta, pos);
  uint32_t chunk_count = read_u32(meta, pos);

  if (disk_->num_pages() < chunk_count + 1) {
    throw std::runtime_error("database snapshot is missing chunk pages");
  }

  std::string snapshot;
  snapshot.reserve(static_cast<std::size_t>(snapshot_size));
  for (uint32_t i = 0; i < chunk_count; ++i) {
    storage::Page chunk_page;
    disk_->read_page(i + 1, chunk_page.data());
    auto chunk = chunk_page.get_tuple(0);
    if (!chunk.has_value()) {
      throw std::runtime_error("database snapshot chunk is missing");
    }
    snapshot.append(chunk->data(), chunk->size());
  }
  if (snapshot.size() < snapshot_size) {
    throw std::runtime_error("database snapshot is truncated");
  }
  snapshot.resize(static_cast<std::size_t>(snapshot_size));
  deserialize_snapshot(snapshot);
}

void Executor::persist_snapshot() {
  if (!persistent_) return;

  std::string snapshot = serialize_snapshot();
  const uint32_t chunk_count =
      static_cast<uint32_t>((snapshot.size() + kSnapshotChunkSize - 1) /
                            kSnapshotChunkSize);

  while (disk_->num_pages() < chunk_count + 1) {
    disk_->allocate_page();
  }

  std::string meta;
  append_string(meta, kSnapshotMagic);
  append_u64(meta, snapshot.size());
  append_u32(meta, chunk_count);

  std::vector<storage::Page> pages;
  pages.resize(static_cast<std::size_t>(chunk_count) + 1);

  pages[0].init(0);
  if (!pages[0].insert_tuple(meta).has_value()) {
    throw std::runtime_error("snapshot metadata does not fit in a page");
  }

  for (uint32_t i = 0; i < chunk_count; ++i) {
    const std::size_t offset = static_cast<std::size_t>(i) * kSnapshotChunkSize;
    const std::size_t len =
        std::min(kSnapshotChunkSize, snapshot.size() - offset);
    pages[i + 1].init(i + 1);
    if (!pages[i + 1]
             .insert_tuple(std::string_view(snapshot.data() + offset, len))
             .has_value()) {
      throw std::runtime_error("snapshot chunk does not fit in a page");
    }
  }

  wal::TxnId txn_id = next_persist_txn_id_++;
  wal_->log_begin(txn_id);
  for (auto& page : pages) {
    page.set_lsn(wal_->next_lsn());
    wal_->log_write(txn_id, page.page_id(), page.data(), storage::kPageSize);
  }
  wal_->log_commit(txn_id);

  for (const auto& page : pages) {
    disk_->write_page(page.page_id(), page.data());
  }
  disk_->flush();
}

std::string Executor::serialize_snapshot() {
  std::string out;
  append_u32(out, static_cast<uint32_t>(schemas_.size()));

  auto* read_txn = txn_mgr_.begin();
  for (const auto& [table_name, schema] : schemas_) {
    append_string(out, table_name);
    append_u32(out, static_cast<uint32_t>(schema.columns.size()));
    for (const auto& col : schema.columns) {
      append_string(out, col.name);
      append_u8(out, static_cast<uint8_t>(col.type));
      append_u8(out, col.is_primary_key ? 1 : 0);
    }

    const auto& table = tables_.at(table_name);
    auto rows = table.scan(read_txn);
    append_u32(out, static_cast<uint32_t>(rows.size()));
    for (const auto& [rid, data] : rows) {
      append_string(out, data);
    }
  }
  txn_mgr_.commit(read_txn);
  return out;
}

void Executor::deserialize_snapshot(const std::string& data) {
  schemas_.clear();
  tables_.clear();

  std::size_t pos = 0;
  uint32_t table_count = read_u32(data, pos);
  auto* load_txn = txn_mgr_.begin();
  for (uint32_t t = 0; t < table_count; ++t) {
    TableSchema schema;
    schema.name = read_string(data, pos);

    uint32_t column_count = read_u32(data, pos);
    schema.columns.reserve(column_count);
    for (uint32_t c = 0; c < column_count; ++c) {
      parser::ColumnDef col;
      col.name = read_string(data, pos);
      col.type = static_cast<parser::ColumnType>(read_u8(data, pos));
      col.is_primary_key = read_u8(data, pos) != 0;
      if (col.is_primary_key) {
        schema.primary_key_index = static_cast<int>(c);
      }
      schema.columns.push_back(std::move(col));
    }

    uint32_t row_count = read_u32(data, pos);
    std::string table_name = schema.name;
    schemas_[table_name] = std::move(schema);
    auto [it, _] = tables_.try_emplace(table_name);
    for (uint32_t r = 0; r < row_count; ++r) {
      it->second.insert(load_txn, read_string(data, pos));
    }
  }
  txn_mgr_.commit(load_txn);
  for (auto& [name, table] : tables_) {
    table.on_commit(load_txn);
  }
}

QueryResult Executor::exec_create(const parser::CreateTableStmt& stmt) {
  QueryResult r;
  if (schemas_.count(stmt.table_name)) {
    r.success = false;
    r.error = "table already exists: " + stmt.table_name;
    return r;
  }
  if (stmt.columns.empty()) {
    r.success = false;
    r.error = "table must have at least one column";
    return r;
  }

  std::unordered_set<std::string> seen_columns;
  bool has_primary_key = false;
  for (const auto& col : stmt.columns) {
    if (!seen_columns.insert(col.name).second) {
      r.success = false;
      r.error = "duplicate column: " + col.name;
      return r;
    }
    if (col.is_primary_key) {
      if (has_primary_key) {
        r.success = false;
        r.error = "multiple primary keys are not supported";
        return r;
      }
      has_primary_key = true;
    }
  }

  TableSchema schema;
  schema.name = stmt.table_name;
  schema.columns = stmt.columns;
  for (std::size_t i = 0; i < stmt.columns.size(); ++i) {
    if (stmt.columns[i].is_primary_key) {
      schema.primary_key_index = static_cast<int>(i);
      break;
    }
  }
  schemas_[stmt.table_name] = std::move(schema);
  tables_.try_emplace(stmt.table_name);
  r.status_message = "CREATE TABLE";
  return r;
}

QueryResult Executor::exec_insert(const parser::InsertStmt& stmt,
                                  concurrency::Transaction* txn) {
  QueryResult r;
  auto it = schemas_.find(stmt.table_name);
  if (it == schemas_.end()) {
    r.success = false;
    r.error = "unknown table: " + stmt.table_name;
    return r;
  }
  const auto& schema = it->second;
  auto& table = tables_.at(stmt.table_name);

  int count = 0;
  for (const auto& row_exprs : stmt.rows) {
    if (row_exprs.size() != schema.columns.size()) {
      r.success = false;
      r.error = "INSERT has " + std::to_string(row_exprs.size()) +
                " values but table " + stmt.table_name + " has " +
                std::to_string(schema.columns.size()) + " columns";
      return r;
    }

    Row row;
    row.reserve(row_exprs.size());
    for (std::size_t i = 0; i < row_exprs.size(); ++i) {
      Value value = eval_expr(row_exprs[i].get(), {}, schema);
      if (!value_matches_column(value, schema.columns[i])) {
        r.success = false;
        r.error = "column " + schema.columns[i].name + " expects " +
                  column_type_name(schema.columns[i].type);
        return r;
      }
      row.push_back(std::move(value));
    }
    std::string data = serialize_row(row, schema);
    table.insert(txn, std::move(data));
    ++count;
  }
  r.status_message = "INSERT 0 " + std::to_string(count);
  return r;
}

QueryResult Executor::exec_select(const parser::SelectStmt& stmt,
                                  concurrency::Transaction* txn) {
  QueryResult r;
  auto it = schemas_.find(stmt.table_name);
  if (it == schemas_.end()) {
    r.success = false;
    r.error = "unknown table: " + stmt.table_name;
    return r;
  }
  const auto& schema = it->second;
  auto& table = tables_.at(stmt.table_name);

  // Determine output columns.
  std::vector<std::size_t> col_indices;
  if (stmt.columns.empty()) {
    for (std::size_t i = 0; i < schema.columns.size(); ++i) {
      col_indices.push_back(i);
      r.column_names.push_back(schema.columns[i].name);
    }
  } else {
    for (const auto& name : stmt.columns) {
      bool found = false;
      for (std::size_t i = 0; i < schema.columns.size(); ++i) {
        if (schema.columns[i].name == name) {
          col_indices.push_back(i);
          r.column_names.push_back(name);
          found = true;
          break;
        }
      }
      if (!found) {
        r.success = false;
        r.error = "unknown column: " + name;
        return r;
      }
    }
  }

  auto all_rows = table.scan(txn);
  for (const auto& [rid, data] : all_rows) {
    Row full_row = deserialize_row(data, schema);
    if (!eval_where(stmt.where.get(), full_row, schema)) continue;
    Row projected;
    for (auto idx : col_indices) {
      projected.push_back(full_row[idx]);
    }
    r.rows.push_back(std::move(projected));
  }
  r.status_message = "SELECT " + std::to_string(r.rows.size());
  return r;
}

QueryResult Executor::exec_update(const parser::UpdateStmt& stmt,
                                  concurrency::Transaction* txn) {
  QueryResult r;
  auto it = schemas_.find(stmt.table_name);
  if (it == schemas_.end()) {
    r.success = false;
    r.error = "unknown table: " + stmt.table_name;
    return r;
  }
  const auto& schema = it->second;
  auto& table = tables_.at(stmt.table_name);

  std::vector<std::pair<std::size_t, const parser::Expr*>> assignments;
  assignments.reserve(stmt.assignments.size());
  for (const auto& [col_name, expr] : stmt.assignments) {
    auto col_it =
        std::find_if(schema.columns.begin(), schema.columns.end(),
                     [&](const parser::ColumnDef& col) {
                       return col.name == col_name;
                     });
    if (col_it == schema.columns.end()) {
      r.success = false;
      r.error = "unknown column: " + col_name;
      return r;
    }
    assignments.emplace_back(
        static_cast<std::size_t>(col_it - schema.columns.begin()), expr.get());
  }

  auto all_rows = table.scan(txn);
  int count = 0;
  for (const auto& [rid, data] : all_rows) {
    Row row = deserialize_row(data, schema);
    if (!eval_where(stmt.where.get(), row, schema)) continue;
    for (const auto& [col_idx, expr] : assignments) {
      Value value = eval_expr(expr, row, schema);
      if (!value_matches_column(value, schema.columns[col_idx])) {
        r.success = false;
        r.error = "column " + schema.columns[col_idx].name + " expects " +
                  column_type_name(schema.columns[col_idx].type);
        return r;
      }
      row[col_idx] = std::move(value);
    }
    if (!table.update(txn, rid, serialize_row(row, schema))) {
      r.success = false;
      r.error = "could not update row due to write conflict";
      return r;
    }
    ++count;
  }
  r.status_message = "UPDATE " + std::to_string(count);
  return r;
}

QueryResult Executor::exec_delete(const parser::DeleteStmt& stmt,
                                  concurrency::Transaction* txn) {
  QueryResult r;
  auto it = schemas_.find(stmt.table_name);
  if (it == schemas_.end()) {
    r.success = false;
    r.error = "unknown table: " + stmt.table_name;
    return r;
  }
  const auto& schema = it->second;
  auto& table = tables_.at(stmt.table_name);

  auto all_rows = table.scan(txn);
  int count = 0;
  for (const auto& [rid, data] : all_rows) {
    Row row = deserialize_row(data, schema);
    if (!eval_where(stmt.where.get(), row, schema)) continue;
    if (!table.remove(txn, rid)) {
      r.success = false;
      r.error = "could not delete row due to write conflict";
      return r;
    }
    ++count;
  }
  r.status_message = "DELETE " + std::to_string(count);
  return r;
}

}  // namespace litedb::executor
