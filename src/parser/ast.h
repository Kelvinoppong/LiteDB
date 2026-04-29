#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <variant>
#include <vector>

namespace litedb::parser {

// Column types supported by the engine.
enum class ColumnType { Int, Text };

struct ColumnDef {
  std::string name;
  ColumnType type;
  bool is_primary_key = false;
};

// Expressions used in WHERE clauses and value lists.
struct Expr;
using ExprPtr = std::unique_ptr<Expr>;

enum class BinaryOp { Eq, Neq, Lt, Gt, LtEq, GtEq, And, Or, Plus, Minus };

struct Expr {
  enum class Kind { IntLiteral, StrLiteral, ColumnRef, BinaryOp };
  Kind kind;
  int64_t int_val = 0;
  std::string str_val;
  BinaryOp op = BinaryOp::Eq;
  ExprPtr left;
  ExprPtr right;
};

// Statements.
struct CreateTableStmt {
  std::string table_name;
  std::vector<ColumnDef> columns;
};

struct InsertStmt {
  std::string table_name;
  std::vector<std::vector<ExprPtr>> rows;  // each row is a list of exprs
};

struct SelectStmt {
  std::string table_name;
  std::vector<std::string> columns;  // empty = SELECT *
  ExprPtr where;
};

struct UpdateStmt {
  std::string table_name;
  std::vector<std::pair<std::string, ExprPtr>> assignments;
  ExprPtr where;
};

struct DeleteStmt {
  std::string table_name;
  ExprPtr where;
};

struct BeginStmt {};
struct CommitStmt {};
struct RollbackStmt {};

using Statement = std::variant<CreateTableStmt, InsertStmt, SelectStmt,
                               UpdateStmt, DeleteStmt, BeginStmt,
                               CommitStmt, RollbackStmt>;

}  // namespace litedb::parser
