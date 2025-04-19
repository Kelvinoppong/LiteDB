#include "server/tcp_server.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <utility>

#include "parser/parser.h"
#include "server/pg_protocol.h"

namespace litedb::server {

namespace {

bool send_all(int fd, const std::string& data) {
  std::size_t sent = 0;
  while (sent < data.size()) {
    ssize_t n = ::send(fd, data.data() + sent, data.size() - sent, MSG_NOSIGNAL);
    if (n <= 0) return false;
    sent += static_cast<std::size_t>(n);
  }
  return true;
}

bool recv_exact(int fd, void* buf, std::size_t len) {
  auto* p = static_cast<char*>(buf);
  std::size_t got = 0;
  while (got < len) {
    ssize_t n = ::recv(fd, p + got, len - got, 0);
    if (n <= 0) return false;
    got += static_cast<std::size_t>(n);
  }
  return true;
}

}  // namespace

TcpServer::TcpServer(uint16_t port, std::string db_path, uint16_t web_port)
    : port_(port), web_port_(web_port), executor_(std::move(db_path)) {}

TcpServer::~TcpServer() {
  stop();
  for (auto& t : client_threads_) {
    if (t.joinable()) t.join();
  }
}

void TcpServer::start() {
  listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
  if (listen_fd_ < 0) {
    throw std::system_error(errno, std::generic_category(), "socket");
  }

  int opt = 1;
  ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = INADDR_ANY;
  addr.sin_port = htons(port_);

  if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
    ::close(listen_fd_);
    throw std::system_error(errno, std::generic_category(), "bind");
  }

  if (::listen(listen_fd_, 16) < 0) {
    ::close(listen_fd_);
    throw std::system_error(errno, std::generic_category(), "listen");
  }

  running_.store(true);
  web_server_ = std::make_unique<WebServer>(web_port_, executor_);
  web_server_->start();

  std::cerr << "LiteDB PostgreSQL wire listening on port " << port_ << "\n";
  std::cerr << "LiteDB browser UI: http://127.0.0.1:" << web_port_ << "/\n";
  accept_loop();
}

void TcpServer::stop() {
  running_.store(false);
  if (web_server_) {
    web_server_->stop();
  }
  if (listen_fd_ >= 0) {
    ::shutdown(listen_fd_, SHUT_RDWR);
    ::close(listen_fd_);
    listen_fd_ = -1;
  }
}

void TcpServer::accept_loop() {
  while (running_.load()) {
    sockaddr_in client_addr{};
    socklen_t len = sizeof(client_addr);
    int client_fd = ::accept(listen_fd_,
                             reinterpret_cast<sockaddr*>(&client_addr), &len);
    if (client_fd < 0) {
      if (!running_.load()) break;
      continue;
    }
    client_threads_.emplace_back(&TcpServer::handle_client, this, client_fd);
  }
}

void TcpServer::handle_client(int client_fd) {
  if (!handle_startup(client_fd)) {
    ::close(client_fd);
    return;
  }
  concurrency::Transaction* current_txn = nullptr;

  // Send initial ready.
  std::string init;
  init += PgWriter::auth_ok();
  init += PgWriter::parameter_status("server_version", "LiteDB 0.1");
  init += PgWriter::parameter_status("client_encoding", "UTF8");
  init += PgWriter::ready_for_query();
  send_all(client_fd, init);

  // Message loop.
  while (running_.load()) {
    char type = 0;
    if (!recv_exact(client_fd, &type, 1)) break;

    uint32_t net_len = 0;
    if (!recv_exact(client_fd, &net_len, 4)) break;
    uint32_t len = ntohl(net_len) - 4;

    std::string payload(len, '\0');
    if (len > 0 && !recv_exact(client_fd, payload.data(), len)) break;

    if (type == pg::kTerminate) break;

    if (type == pg::kQuery) {
      // Remove trailing null.
      if (!payload.empty() && payload.back() == '\0') payload.pop_back();
      handle_query(client_fd, payload, current_txn);
    } else {
      auto err = PgWriter::error_response("ERROR", "0A000",
                                          "unsupported message type");
      send_all(client_fd,
               err + PgWriter::ready_for_query(current_txn ? 'T' : 'I'));
    }
  }
  if (current_txn != nullptr) {
    try {
      executor_.rollback_transaction(current_txn);
    } catch (...) {
      // Best effort cleanup on disconnect.
    }
  }
  ::close(client_fd);
}

bool TcpServer::handle_startup(int fd) {
  // Read startup message: 4-byte length + 4-byte protocol version.
  uint32_t net_len = 0;
  if (!recv_exact(fd, &net_len, 4)) return false;
  uint32_t total_len = ntohl(net_len);
  if (total_len < 8) return false;

  uint32_t net_proto = 0;
  if (!recv_exact(fd, &net_proto, 4)) return false;

  // Skip remaining startup params.
  if (total_len > 8) {
    std::string rest(total_len - 8, '\0');
    if (!recv_exact(fd, rest.data(), rest.size())) return false;
  }

  // We don't do SSL or real auth — just accept.
  return true;
}

void TcpServer::handle_query(int fd, const std::string& sql,
                             concurrency::Transaction*& current_txn) {
  try {
    auto stmt = parser::parse_sql(sql);
    executor::QueryResult result;
    if (std::holds_alternative<parser::BeginStmt>(stmt)) {
      if (current_txn != nullptr) {
        result.success = false;
        result.error = "transaction already in progress";
      } else {
        current_txn = executor_.begin_transaction();
        result.status_message = "BEGIN";
      }
    } else if (std::holds_alternative<parser::CommitStmt>(stmt)) {
      if (current_txn == nullptr) {
        result.success = false;
        result.error = "no transaction in progress";
      } else {
        executor_.commit_transaction(current_txn);
        current_txn = nullptr;
        result.status_message = "COMMIT";
      }
    } else if (std::holds_alternative<parser::RollbackStmt>(stmt)) {
      if (current_txn == nullptr) {
        result.success = false;
        result.error = "no transaction in progress";
      } else {
        executor_.rollback_transaction(current_txn);
        current_txn = nullptr;
        result.status_message = "ROLLBACK";
      }
    } else {
      result = executor_.execute(stmt, current_txn);
    }

    std::string response;
    if (!result.success) {
      response += PgWriter::error_response("ERROR", "42000", result.error);
    } else {
      if (!result.column_names.empty()) {
        response += PgWriter::row_description(result.column_names);
        for (const auto& row : result.rows) {
          std::vector<std::string> vals;
          for (const auto& v : row) {
            if (std::holds_alternative<int64_t>(v)) {
              vals.push_back(std::to_string(std::get<int64_t>(v)));
            } else {
              vals.push_back(std::get<std::string>(v));
            }
          }
          response += PgWriter::data_row(vals);
        }
      }
      response += PgWriter::command_complete(result.status_message);
    }
    response += PgWriter::ready_for_query(current_txn ? 'T' : 'I');
    send_all(fd, response);
  } catch (const std::exception& e) {
    auto err = PgWriter::error_response("ERROR", "42601", e.what());
    send_all(fd, err + PgWriter::ready_for_query(current_txn ? 'T' : 'I'));
  }
}

}  // namespace litedb::server
