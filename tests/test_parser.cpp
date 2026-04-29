#include <gtest/gtest.h>

#include <filesystem>
#include <stdexcept>
#include <string>

#include <unistd.h>

#include "executor/executor.h"
#include "parser/parser.h"

using namespace litedb::parser;
using namespace litedb::executor;
namespace fs = std::filesystem;

namespace {

std::string TempDbPath(const char* tag) {
  return (fs::temp_directory_path() /
          ("litedb_exec_" + std::string(tag) + "_" +
           std::to_string(::getpid()) + ".db"))
      .string();
}

struct ScopedDb {
  std::string path;
  ~ScopedDb() {
    std::error_code ec;
    fs::remove(path, ec);
    fs::remove(path + ".wal", ec);
  }
};

}  // namespace

TEST(Parser, CreateTable) {
  auto stmt = parse_sql("CREATE TABLE users (id INT PRIMARY KEY, name TEXT)");
  auto* ct = std::get_if<CreateTableStmt>(&stmt);
  ASSERT_NE(ct, nullptr);
  EXPECT_EQ(ct->table_name, "users");
  ASSERT_EQ(ct->columns.size(), 2u);
  EXPECT_EQ(ct->columns[0].name, "id");
  EXPECT_EQ(ct->columns[0].type, ColumnType::Int);
  EXPECT_TRUE(ct->columns[0].is_primary_key);
  EXPECT_EQ(ct->columns[1].name, "name");
  EXPECT_EQ(ct->columns[1].type, ColumnType::Text);
}

TEST(Parser, InsertValues) {
  auto stmt = parse_sql("INSERT INTO users VALUES (1, 'alice'), (2, 'bob')");
  auto* ins = std::get_if<InsertStmt>(&stmt);
  ASSERT_NE(ins, nullptr);
  EXPECT_EQ(ins->table_name, "users");
  EXPECT_EQ(ins->rows.size(), 2u);
}

TEST(Parser, SelectWhere) {
  auto stmt = parse_sql("SELECT name FROM users WHERE id > 5");
  auto* sel = std::get_if<SelectStmt>(&stmt);
  ASSERT_NE(sel, nullptr);
  EXPECT_EQ(sel->table_name, "users");
  EXPECT_EQ(sel->columns.size(), 1u);
  EXPECT_EQ(sel->columns[0], "name");
  EXPECT_NE(sel->where, nullptr);
}

TEST(Parser, Update) {
  auto stmt = parse_sql("UPDATE users SET name = 'bob' WHERE id = 1");
  auto* upd = std::get_if<UpdateStmt>(&stmt);
  ASSERT_NE(upd, nullptr);
  EXPECT_EQ(upd->table_name, "users");
  EXPECT_EQ(upd->assignments.size(), 1u);
  EXPECT_NE(upd->where, nullptr);
}

TEST(Parser, Delete) {
  auto stmt = parse_sql("DELETE FROM users WHERE id = 1");
  auto* del = std::get_if<DeleteStmt>(&stmt);
  ASSERT_NE(del, nullptr);
  EXPECT_EQ(del->table_name, "users");
  EXPECT_NE(del->where, nullptr);
}

TEST(Parser, ArithmeticEscapesAndTrailingTokens) {
  auto stmt = parse_sql("INSERT INTO users VALUES (1 + 2, 'it''s ok')");
  auto* ins = std::get_if<InsertStmt>(&stmt);
  ASSERT_NE(ins, nullptr);
  ASSERT_EQ(ins->rows.size(), 1u);
  ASSERT_EQ(ins->rows[0].size(), 2u);
  EXPECT_EQ(ins->rows[0][0]->kind, Expr::Kind::BinaryOp);
  EXPECT_EQ(ins->rows[0][0]->op, BinaryOp::Plus);
  EXPECT_EQ(ins->rows[0][1]->kind, Expr::Kind::StrLiteral);
  EXPECT_EQ(ins->rows[0][1]->str_val, "it's ok");

  EXPECT_THROW(parse_sql("SELECT * FROM users; SELECT * FROM users"),
               std::runtime_error);
  EXPECT_THROW(parse_sql("INSERT INTO users VALUES ('unterminated)"),
               std::runtime_error);
}

// End-to-end executor test.
TEST(Executor, CreateInsertSelect) {
  Executor exec;

  auto r1 = exec.execute(parse_sql("CREATE TABLE t (id INT, val TEXT)"));
  EXPECT_TRUE(r1.success);

  auto r2 = exec.execute(
      parse_sql("INSERT INTO t VALUES (1, 'hello'), (2, 'world'), (3, 'foo')"));
  EXPECT_TRUE(r2.success);
  EXPECT_EQ(r2.status_message, "INSERT 0 3");

  auto r3 = exec.execute(parse_sql("SELECT val FROM t WHERE id >= 2"));
  EXPECT_TRUE(r3.success);
  EXPECT_EQ(r3.rows.size(), 2u);

  auto r4 = exec.execute(parse_sql("UPDATE t SET val = 'updated' WHERE id = 1"));
  EXPECT_TRUE(r4.success);
  EXPECT_EQ(r4.status_message, "UPDATE 1");

  auto r5 = exec.execute(parse_sql("SELECT * FROM t WHERE id = 1"));
  EXPECT_TRUE(r5.success);
  ASSERT_EQ(r5.rows.size(), 1u);
  EXPECT_EQ(std::get<std::string>(r5.rows[0][1]), "updated");

  auto r6 = exec.execute(parse_sql("DELETE FROM t WHERE id = 3"));
  EXPECT_TRUE(r6.success);
  EXPECT_EQ(r6.status_message, "DELETE 1");

  auto r7 = exec.execute(parse_sql("SELECT * FROM t"));
  EXPECT_TRUE(r7.success);
  EXPECT_EQ(r7.rows.size(), 2u);
}

TEST(Executor, ValidatesRowsColumnsAndTypes) {
  Executor exec;

  ASSERT_TRUE(
      exec.execute(parse_sql("CREATE TABLE t (id INT, val TEXT)")).success);

  auto wrong_count = exec.execute(parse_sql("INSERT INTO t VALUES (1)"));
  EXPECT_FALSE(wrong_count.success);

  auto wrong_type =
      exec.execute(parse_sql("INSERT INTO t VALUES ('not-int', 'ok')"));
  EXPECT_FALSE(wrong_type.success);

  auto valid = exec.execute(parse_sql("INSERT INTO t VALUES (1 + 2, 'ok')"));
  EXPECT_TRUE(valid.success) << valid.error;

  auto unknown_col =
      exec.execute(parse_sql("UPDATE t SET missing = 'x' WHERE id = 3"));
  EXPECT_FALSE(unknown_col.success);

  auto update_wrong_type =
      exec.execute(parse_sql("UPDATE t SET id = 'bad' WHERE id = 3"));
  EXPECT_FALSE(update_wrong_type.success);

  auto selected = exec.execute(parse_sql("SELECT * FROM t"));
  ASSERT_TRUE(selected.success);
  ASSERT_EQ(selected.rows.size(), 1u);
  EXPECT_EQ(std::get<int64_t>(selected.rows[0][0]), 3);
  EXPECT_EQ(std::get<std::string>(selected.rows[0][1]), "ok");
}

TEST(Executor, ExplicitTransactionVisibility) {
  Executor exec;

  ASSERT_TRUE(
      exec.execute(parse_sql("CREATE TABLE t (id INT, val TEXT)")).success);

  auto* writer = exec.begin_transaction();
  auto insert =
      exec.execute(parse_sql("INSERT INTO t VALUES (1, 'pending')"), writer);
  ASSERT_TRUE(insert.success) << insert.error;

  auto before_commit = exec.execute(parse_sql("SELECT * FROM t"));
  ASSERT_TRUE(before_commit.success);
  EXPECT_TRUE(before_commit.rows.empty());

  auto same_txn = exec.execute(parse_sql("SELECT * FROM t"), writer);
  ASSERT_TRUE(same_txn.success);
  ASSERT_EQ(same_txn.rows.size(), 1u);
  EXPECT_EQ(std::get<std::string>(same_txn.rows[0][1]), "pending");

  exec.commit_transaction(writer);

  auto after_commit = exec.execute(parse_sql("SELECT * FROM t"));
  ASSERT_TRUE(after_commit.success);
  ASSERT_EQ(after_commit.rows.size(), 1u);

  auto* updater = exec.begin_transaction();
  auto update =
      exec.execute(parse_sql("UPDATE t SET val = 'new' WHERE id = 1"), updater);
  ASSERT_TRUE(update.success) << update.error;

  auto outside_update = exec.execute(parse_sql("SELECT val FROM t WHERE id = 1"));
  ASSERT_TRUE(outside_update.success);
  ASSERT_EQ(outside_update.rows.size(), 1u);
  EXPECT_EQ(std::get<std::string>(outside_update.rows[0][0]), "pending");

  exec.rollback_transaction(updater);

  auto after_rollback = exec.execute(parse_sql("SELECT val FROM t WHERE id = 1"));
  ASSERT_TRUE(after_rollback.success);
  ASSERT_EQ(after_rollback.rows.size(), 1u);
  EXPECT_EQ(std::get<std::string>(after_rollback.rows[0][0]), "pending");
}

TEST(Executor, PersistentDatabaseSurvivesRestart) {
  ScopedDb db{TempDbPath("restart")};

  {
    Executor exec(db.path);
    ASSERT_TRUE(
        exec.execute(parse_sql("CREATE TABLE t (id INT, val TEXT)")).success);
    ASSERT_TRUE(exec.execute(parse_sql("INSERT INTO t VALUES (1, 'one')"))
                    .success);

    auto* committed = exec.begin_transaction();
    ASSERT_TRUE(exec.execute(parse_sql("INSERT INTO t VALUES (2, 'two')"),
                             committed)
                    .success);
    exec.commit_transaction(committed);

    auto* rolled_back = exec.begin_transaction();
    ASSERT_TRUE(exec.execute(parse_sql("INSERT INTO t VALUES (3, 'ghost')"),
                             rolled_back)
                    .success);
    exec.rollback_transaction(rolled_back);

    ASSERT_TRUE(exec.execute(parse_sql("UPDATE t SET val = 'uno' WHERE id = 1"))
                    .success);
  }

  {
    Executor exec(db.path);
    auto one = exec.execute(parse_sql("SELECT val FROM t WHERE id = 1"));
    ASSERT_TRUE(one.success) << one.error;
    ASSERT_EQ(one.rows.size(), 1u);
    EXPECT_EQ(std::get<std::string>(one.rows[0][0]), "uno");

    auto two = exec.execute(parse_sql("SELECT val FROM t WHERE id = 2"));
    ASSERT_TRUE(two.success) << two.error;
    ASSERT_EQ(two.rows.size(), 1u);
    EXPECT_EQ(std::get<std::string>(two.rows[0][0]), "two");

    auto ghost = exec.execute(parse_sql("SELECT * FROM t WHERE id = 3"));
    ASSERT_TRUE(ghost.success) << ghost.error;
    EXPECT_TRUE(ghost.rows.empty());
  }
}

TEST(Executor, PersistentDatabaseCanRecoverFromWalOnly) {
  ScopedDb db{TempDbPath("wal_restart")};

  {
    Executor exec(db.path);
    ASSERT_TRUE(
        exec.execute(parse_sql("CREATE TABLE t (id INT, val TEXT)")).success);
    ASSERT_TRUE(exec.execute(parse_sql("INSERT INTO t VALUES (7, 'seven')"))
                    .success);
  }

  std::error_code ec;
  ASSERT_TRUE(fs::remove(db.path, ec));

  {
    Executor exec(db.path);
    auto result = exec.execute(parse_sql("SELECT val FROM t WHERE id = 7"));
    ASSERT_TRUE(result.success) << result.error;
    ASSERT_EQ(result.rows.size(), 1u);
    EXPECT_EQ(std::get<std::string>(result.rows[0][0]), "seven");
  }
}

TEST(Executor, PersistentCreateTableIsAutocommitOnly) {
  ScopedDb db{TempDbPath("ddl_txn")};
  Executor exec(db.path);

  auto* txn = exec.begin_transaction();
  auto result = exec.execute(parse_sql("CREATE TABLE t (id INT)"), txn);
  EXPECT_FALSE(result.success);
  exec.rollback_transaction(txn);
}
