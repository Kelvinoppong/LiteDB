#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace litedb::parser {

enum class TokenType {
  // Keywords
  Select,
  Insert,
  Into,
  Values,
  From,
  Where,
  Create,
  Table,
  Update,
  Set,
  Delete,
  And,
  Or,
  Not,
  Int,
  Text,
  PrimaryKey,
  Begin,
  Commit,
  Rollback,

  // Literals / identifiers
  Identifier,
  IntegerLiteral,
  StringLiteral,

  // Operators
  Star,       // *
  Comma,      // ,
  Semicolon,  // ;
  LParen,     // (
  RParen,     // )
  Eq,         // =
  Neq,        // != or <>
  Lt,         // <
  Gt,         // >
  LtEq,       // <=
  GtEq,       // >=
  Plus,
  Minus,

  // Control
  Eof,
  Invalid,
};

struct Token {
  TokenType type;
  std::string text;
  int64_t int_value = 0;
};

class Lexer {
 public:
  explicit Lexer(std::string input);
  std::vector<Token> tokenize();

 private:
  Token next_token();
  char peek() const;
  char advance();
  void skip_whitespace();
  Token read_identifier_or_keyword();
  Token read_number();
  Token read_string();
  Token make_token(TokenType type, std::string text);

  std::string input_;
  std::size_t pos_ = 0;
};

}  // namespace litedb::parser
