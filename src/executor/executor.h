#pragma once

#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

#include "concurrency/mvcc.h"
#include "concurrency/transaction.h"
#include "parser/ast.h"
#include "storage/disk_manager.h"
#include "wal/wal_manager.h"

namespace litedb::executor {

using Value = std::variant<int64_t, std::string>;
using Row = std::vector<Value>;

struct TableSchema {
  std::string name;
  std::vector<parser::ColumnDef> columns;
  int primary_key_index = -1;
};

struct QueryResult {
  bool success = true;
  std::string error;
  std::vector<std::string> column_names;
  std::vector<Row> rows;
  std::string status_message;
};

// Catalog + executor. Manages table metadata, routes parsed statements
// to the underlying MVCC table storage, and serializes/deserializes rows.
class Executor {
 public:
  Executor();
  explicit Executor(std::string db_path);

  // Executes a parsed statement within the given transaction context.
  // If `txn` is nullptr, a one-shot auto-commit transaction is used.
  QueryResult execute(const parser::Statement& stmt,
                      concurrency::Transaction* txn = nullptr);

  concurrency::Transaction* begin_transaction();
  void commit_transaction(concurrency::Transaction* txn);
  void rollback_transaction(concurrency::Transaction* txn);

  concurrency::TransactionManager& txn_manager() { return txn_mgr_; }

 private:
  QueryResult exec_create(const parser::CreateTableStmt& stmt);
  QueryResult exec_insert(const parser::InsertStmt& stmt,
                          concurrency::Transaction* txn);
  QueryResult exec_select(const parser::SelectStmt& stmt,
                          concurrency::Transaction* txn);
  QueryResult exec_update(const parser::UpdateStmt& stmt,
                          concurrency::Transaction* txn);
  QueryResult exec_delete(const parser::DeleteStmt& stmt,
                          concurrency::Transaction* txn);

  // Row serialization.
  std::string serialize_row(const Row& row, const TableSchema& schema) const;
  Row deserialize_row(const std::string& data, const TableSchema& schema) const;

  // Expression evaluation.
  Value eval_expr(const parser::Expr* expr, const Row& row,
                  const TableSchema& schema) const;
  bool eval_where(const parser::Expr* expr, const Row& row,
                  const TableSchema& schema) const;
  void load_snapshot();
  void persist_snapshot();
  std::string serialize_snapshot();
  void deserialize_snapshot(const std::string& data);

  concurrency::TransactionManager txn_mgr_;
  std::unordered_map<std::string, TableSchema> schemas_;
  std::unordered_map<std::string, concurrency::MvccTable> tables_;
  mutable std::mutex mutex_;
  bool persistent_ = false;
  std::string db_path_;
  std::unique_ptr<storage::DiskManager> disk_;
  std::unique_ptr<wal::WalManager> wal_;
  wal::TxnId next_persist_txn_id_ = 1;
};

}  // namespace litedb::executor
