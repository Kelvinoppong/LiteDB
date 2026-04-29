#include "parser/lexer.h"

#include <algorithm>
#include <cctype>
#include <unordered_map>

namespace litedb::parser {

namespace {

const std::unordered_map<std::string, TokenType> kKeywords = {
    {"select", TokenType::Select},   {"insert", TokenType::Insert},
    {"into", TokenType::Into},       {"values", TokenType::Values},
    {"from", TokenType::From},       {"where", TokenType::Where},
    {"create", TokenType::Create},   {"table", TokenType::Table},
    {"update", TokenType::Update},   {"set", TokenType::Set},
    {"delete", TokenType::Delete},   {"and", TokenType::And},
    {"or", TokenType::Or},           {"not", TokenType::Not},
    {"int", TokenType::Int},         {"text", TokenType::Text},
    {"integer", TokenType::Int},     {"primary", TokenType::PrimaryKey},
    {"begin", TokenType::Begin},     {"commit", TokenType::Commit},
    {"rollback", TokenType::Rollback},
};

}  // namespace

Lexer::Lexer(std::string input) : input_(std::move(input)) {}

std::vector<Token> Lexer::tokenize() {
  std::vector<Token> tokens;
  while (true) {
    Token tok = next_token();
    tokens.push_back(tok);
    if (tok.type == TokenType::Eof || tok.type == TokenType::Invalid) break;
  }
  return tokens;
}

char Lexer::peek() const {
  return pos_ < input_.size() ? input_[pos_] : '\0';
}

char Lexer::advance() {
  return pos_ < input_.size() ? input_[pos_++] : '\0';
}

void Lexer::skip_whitespace() {
  while (pos_ < input_.size() && std::isspace(static_cast<unsigned char>(input_[pos_]))) {
    ++pos_;
  }
}

Token Lexer::make_token(TokenType type, std::string text) {
  return {type, std::move(text), 0};
}

Token Lexer::next_token() {
  skip_whitespace();
  if (pos_ >= input_.size()) return {TokenType::Eof, "", 0};

  char c = peek();

  if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') {
    return read_identifier_or_keyword();
  }
  if (std::isdigit(static_cast<unsigned char>(c))) {
    return read_number();
  }
  if (c == '\'') {
    return read_string();
  }

  advance();
  switch (c) {
    case '*': return make_token(TokenType::Star, "*");
    case ',': return make_token(TokenType::Comma, ",");
    case ';': return make_token(TokenType::Semicolon, ";");
    case '(': return make_token(TokenType::LParen, "(");
    case ')': return make_token(TokenType::RParen, ")");
    case '+': return make_token(TokenType::Plus, "+");
    case '-':
      if (std::isdigit(static_cast<unsigned char>(peek()))) {
        --pos_;
        return read_number();
      }
      return make_token(TokenType::Minus, "-");
    case '=': return make_token(TokenType::Eq, "=");
    case '<':
      if (peek() == '=') { advance(); return make_token(TokenType::LtEq, "<="); }
      if (peek() == '>') { advance(); return make_token(TokenType::Neq, "<>"); }
      return make_token(TokenType::Lt, "<");
    case '>':
      if (peek() == '=') { advance(); return make_token(TokenType::GtEq, ">="); }
      return make_token(TokenType::Gt, ">");
    case '!':
      if (peek() == '=') { advance(); return make_token(TokenType::Neq, "!="); }
      return make_token(TokenType::Invalid, "!");
    default:
      return make_token(TokenType::Invalid, std::string(1, c));
  }
}

Token Lexer::read_identifier_or_keyword() {
  std::size_t start = pos_;
  while (pos_ < input_.size() &&
         (std::isalnum(static_cast<unsigned char>(input_[pos_])) || input_[pos_] == '_')) {
    ++pos_;
  }
  std::string word = input_.substr(start, pos_ - start);
  std::string lower = word;
  std::transform(lower.begin(), lower.end(), lower.begin(),
                 [](unsigned char c) { return std::tolower(c); });

  // Handle "PRIMARY KEY" as two tokens; we handle "primary" here.
  auto it = kKeywords.find(lower);
  if (it != kKeywords.end()) {
    // If this is "primary", peek ahead for "key".
    if (it->second == TokenType::PrimaryKey) {
      std::size_t saved = pos_;
      skip_whitespace();
      if (pos_ + 3 <= input_.size()) {
        std::string next3 = input_.substr(pos_, 3);
        std::transform(next3.begin(), next3.end(), next3.begin(),
                       [](unsigned char ch) { return std::tolower(ch); });
        if (next3 == "key") {
          pos_ += 3;
          return {TokenType::PrimaryKey, "primary key", 0};
        }
      }
      pos_ = saved;
      return {TokenType::Identifier, word, 0};
    }
    return {it->second, word, 0};
  }
  return {TokenType::Identifier, word, 0};
}

Token Lexer::read_number() {
  std::size_t start = pos_;
  if (input_[pos_] == '-') ++pos_;
  while (pos_ < input_.size() && std::isdigit(static_cast<unsigned char>(input_[pos_]))) {
    ++pos_;
  }
  std::string num_str = input_.substr(start, pos_ - start);
  Token tok{TokenType::IntegerLiteral, num_str, 0};
  tok.int_value = std::stoll(num_str);
  return tok;
}

Token Lexer::read_string() {
  advance();  // skip opening quote
  std::string result;
  while (pos_ < input_.size()) {
    if (input_[pos_] == '\'') {
      if (pos_ + 1 < input_.size() && input_[pos_ + 1] == '\'') {
        result += '\'';
        pos_ += 2;
        continue;
      }
      advance();  // skip closing quote
      return {TokenType::StringLiteral, result, 0};
    }
    result += input_[pos_++];
  }
  return {TokenType::Invalid, "unterminated string literal", 0};
}

}  // namespace litedb::parser
