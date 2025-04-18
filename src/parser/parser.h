#pragma once

#include <string>
#include <vector>

#include "parser/ast.h"
#include "parser/lexer.h"

namespace litedb::parser {

// Recursive-descent SQL parser. Supports:
//   CREATE TABLE name (col type, ...)
//   INSERT INTO name VALUES (val, ...), ...
//   SELECT cols FROM name [WHERE expr]
//   UPDATE name SET col=val, ... [WHERE expr]
//   DELETE FROM name [WHERE expr]
//   BEGIN / COMMIT / ROLLBACK
class Parser {
 public:
  explicit Parser(std::vector<Token> tokens);

  Statement parse();

 private:
  Token& current();
  Token& peek_token();
  Token consume(TokenType expected);
  bool match(TokenType type);
  bool check(TokenType type) const;

  Statement parse_create();
  Statement parse_insert();
  Statement parse_select();
  Statement parse_update();
  Statement parse_delete();

  ExprPtr parse_expr();
  ExprPtr parse_or_expr();
  ExprPtr parse_and_expr();
  ExprPtr parse_comparison();
  ExprPtr parse_additive();
  ExprPtr parse_primary();

  std::vector<Token> tokens_;
  std::size_t pos_ = 0;
};

// Convenience: lex + parse in one call.
Statement parse_sql(const std::string& sql);

}  // namespace litedb::parser
