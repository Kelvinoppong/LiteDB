#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "executor/executor.h"
#include "server/web_server.h"

namespace litedb::server {

class TcpServer {
 public:
  explicit TcpServer(uint16_t port = 5432,
                     std::string db_path = "litedb.db",
                     uint16_t web_port = 8080);
  ~TcpServer();

  TcpServer(const TcpServer&) = delete;
  TcpServer& operator=(const TcpServer&) = delete;

  // Starts listening. Blocks until stop() is called from another thread.
  void start();

  // Signals the server to shut down.
  void stop();

  uint16_t port() const { return port_; }
  uint16_t web_port() const { return web_port_; }

 private:
  void accept_loop();
  void handle_client(int client_fd);
  bool handle_startup(int fd);
  void handle_query(int fd, const std::string& sql,
                    concurrency::Transaction*& current_txn);

  uint16_t port_;
  uint16_t web_port_;
  int listen_fd_ = -1;
  std::atomic<bool> running_{false};
  executor::Executor executor_;
  std::unique_ptr<WebServer> web_server_;
  std::vector<std::thread> client_threads_;
};

}  // namespace litedb::server
