#include "parser/parser.h"

#include <stdexcept>
#include <utility>

namespace litedb::parser {

Parser::Parser(std::vector<Token> tokens) : tokens_(std::move(tokens)) {}

Token& Parser::current() { return tokens_[pos_]; }

Token& Parser::peek_token() {
  return pos_ + 1 < tokens_.size() ? tokens_[pos_ + 1] : tokens_.back();
}

bool Parser::check(TokenType type) const {
  return pos_ < tokens_.size() && tokens_[pos_].type == type;
}

bool Parser::match(TokenType type) {
  if (check(type)) {
    ++pos_;
    return true;
  }
  return false;
}

Token Parser::consume(TokenType expected) {
  if (!check(expected)) {
    throw std::runtime_error("parse error: expected token " +
                             std::to_string(static_cast<int>(expected)) +
                             " but got " +
                             std::to_string(static_cast<int>(current().type)) +
                             " ('" + current().text + "')");
  }
  return tokens_[pos_++];
}

Statement Parser::parse() {
  Statement stmt;
  if (check(TokenType::Create)) {
    stmt = parse_create();
  } else if (check(TokenType::Insert)) {
    stmt = parse_insert();
  } else if (check(TokenType::Select)) {
    stmt = parse_select();
  } else if (check(TokenType::Update)) {
    stmt = parse_update();
  } else if (check(TokenType::Delete)) {
    stmt = parse_delete();
  } else if (match(TokenType::Begin)) {
    match(TokenType::Semicolon);
    stmt = BeginStmt{};
  } else if (match(TokenType::Commit)) {
    match(TokenType::Semicolon);
    stmt = CommitStmt{};
  } else if (match(TokenType::Rollback)) {
    match(TokenType::Semicolon);
    stmt = RollbackStmt{};
  } else {
    throw std::runtime_error("parse error: unexpected token '" +
                             current().text + "'");
  }

  if (!check(TokenType::Eof)) {
    throw std::runtime_error("parse error: unexpected trailing token '" +
                             current().text + "'");
  }
  return stmt;
}

Statement Parser::parse_create() {
  consume(TokenType::Create);
  consume(TokenType::Table);
  CreateTableStmt stmt;
  stmt.table_name = consume(TokenType::Identifier).text;
  consume(TokenType::LParen);

  while (!check(TokenType::RParen)) {
    ColumnDef col;
    col.name = consume(TokenType::Identifier).text;
    if (match(TokenType::Int)) {
      col.type = ColumnType::Int;
    } else if (match(TokenType::Text)) {
      col.type = ColumnType::Text;
    } else {
      throw std::runtime_error("parse error: expected column type");
    }
    if (match(TokenType::PrimaryKey)) {
      col.is_primary_key = true;
    }
    stmt.columns.push_back(std::move(col));
    if (!match(TokenType::Comma)) break;
  }
  consume(TokenType::RParen);
  match(TokenType::Semicolon);
  return stmt;
}

Statement Parser::parse_insert() {
  consume(TokenType::Insert);
  consume(TokenType::Into);
  InsertStmt stmt;
  stmt.table_name = consume(TokenType::Identifier).text;
  consume(TokenType::Values);

  while (true) {
    consume(TokenType::LParen);
    std::vector<ExprPtr> row;
    while (!check(TokenType::RParen)) {
      row.push_back(parse_expr());
      if (!match(TokenType::Comma)) break;
    }
    consume(TokenType::RParen);
    stmt.rows.push_back(std::move(row));
    if (!match(TokenType::Comma)) break;
  }
  match(TokenType::Semicolon);
  return stmt;
}

Statement Parser::parse_select() {
  consume(TokenType::Select);
  SelectStmt stmt;

  if (match(TokenType::Star)) {
    // all columns
  } else {
    do {
      stmt.columns.push_back(consume(TokenType::Identifier).text);
    } while (match(TokenType::Comma));
  }

  consume(TokenType::From);
  stmt.table_name = consume(TokenType::Identifier).text;

  if (match(TokenType::Where)) {
    stmt.where = parse_expr();
  }
  match(TokenType::Semicolon);
  return stmt;
}

Statement Parser::parse_update() {
  consume(TokenType::Update);
  UpdateStmt stmt;
  stmt.table_name = consume(TokenType::Identifier).text;
  consume(TokenType::Set);

  do {
    std::string col = consume(TokenType::Identifier).text;
    consume(TokenType::Eq);
    auto val = parse_expr();
    stmt.assignments.emplace_back(std::move(col), std::move(val));
  } while (match(TokenType::Comma));

  if (match(TokenType::Where)) {
    stmt.where = parse_expr();
  }
  match(TokenType::Semicolon);
  return stmt;
}

Statement Parser::parse_delete() {
  consume(TokenType::Delete);
  consume(TokenType::From);
  DeleteStmt stmt;
  stmt.table_name = consume(TokenType::Identifier).text;
  if (match(TokenType::Where)) {
    stmt.where = parse_expr();
  }
  match(TokenType::Semicolon);
  return stmt;
}

ExprPtr Parser::parse_expr() {
  return parse_or_expr();
}

ExprPtr Parser::parse_or_expr() {
  auto left = parse_and_expr();
  while (match(TokenType::Or)) {
    auto right = parse_and_expr();
    auto node = std::make_unique<Expr>();
    node->kind = Expr::Kind::BinaryOp;
    node->op = BinaryOp::Or;
    node->left = std::move(left);
    node->right = std::move(right);
    left = std::move(node);
  }
  return left;
}

ExprPtr Parser::parse_and_expr() {
  auto left = parse_comparison();
  while (match(TokenType::And)) {
    auto right = parse_comparison();
    auto node = std::make_unique<Expr>();
    node->kind = Expr::Kind::BinaryOp;
    node->op = BinaryOp::And;
    node->left = std::move(left);
    node->right = std::move(right);
    left = std::move(node);
  }
  return left;
}

ExprPtr Parser::parse_comparison() {
  auto left = parse_additive();
  BinaryOp op;
  bool found = true;
  if (check(TokenType::Eq)) op = BinaryOp::Eq;
  else if (check(TokenType::Neq)) op = BinaryOp::Neq;
  else if (check(TokenType::Lt)) op = BinaryOp::Lt;
  else if (check(TokenType::Gt)) op = BinaryOp::Gt;
  else if (check(TokenType::LtEq)) op = BinaryOp::LtEq;
  else if (check(TokenType::GtEq)) op = BinaryOp::GtEq;
  else found = false;

  if (found) {
    ++pos_;
    auto right = parse_additive();
    auto node = std::make_unique<Expr>();
    node->kind = Expr::Kind::BinaryOp;
    node->op = op;
    node->left = std::move(left);
    node->right = std::move(right);
    return node;
  }
  return left;
}

ExprPtr Parser::parse_additive() {
  auto left = parse_primary();
  while (check(TokenType::Plus) || check(TokenType::Minus)) {
    BinaryOp op = check(TokenType::Plus) ? BinaryOp::Plus : BinaryOp::Minus;
    ++pos_;
    auto right = parse_primary();
    auto node = std::make_unique<Expr>();
    node->kind = Expr::Kind::BinaryOp;
    node->op = op;
    node->left = std::move(left);
    node->right = std::move(right);
    left = std::move(node);
  }
  return left;
}

ExprPtr Parser::parse_primary() {
  if (check(TokenType::IntegerLiteral)) {
    auto tok = tokens_[pos_++];
    auto node = std::make_unique<Expr>();
    node->kind = Expr::Kind::IntLiteral;
    node->int_val = tok.int_value;
    return node;
  }
  if (check(TokenType::StringLiteral)) {
    auto tok = tokens_[pos_++];
    auto node = std::make_unique<Expr>();
    node->kind = Expr::Kind::StrLiteral;
    node->str_val = tok.text;
    return node;
  }
  if (check(TokenType::Identifier)) {
    auto tok = tokens_[pos_++];
    auto node = std::make_unique<Expr>();
    node->kind = Expr::Kind::ColumnRef;
    node->str_val = tok.text;
    return node;
  }
  if (match(TokenType::LParen)) {
    auto inner = parse_expr();
    consume(TokenType::RParen);
    return inner;
  }
  throw std::runtime_error("parse error: unexpected token in expression: '" +
                           current().text + "'");
}

Statement parse_sql(const std::string& sql) {
  Lexer lexer(sql);
  auto tokens = lexer.tokenize();
  Parser parser(std::move(tokens));
  return parser.parse();
}

}  // namespace litedb::parser
