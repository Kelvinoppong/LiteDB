#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

#include "executor/executor.h"

namespace litedb::server {

// Tiny dependency-free HTTP server for the browser query UI.
class WebServer {
 public:
  WebServer(uint16_t port, executor::Executor& executor);
  ~WebServer();

  WebServer(const WebServer&) = delete;
  WebServer& operator=(const WebServer&) = delete;

  void start();
  void stop();

  uint16_t port() const { return port_; }

 private:
  void accept_loop();
  void handle_client(int client_fd);
  std::string handle_request(const std::string& request);
  std::string execute_sql(const std::string& sql);

  uint16_t port_;
  int listen_fd_ = -1;
  executor::Executor& executor_;
  std::atomic<bool> running_{false};
  std::thread accept_thread_;
  std::vector<std::thread> client_threads_;
};

}  // namespace litedb::server
