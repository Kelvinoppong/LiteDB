#include <cstdlib>
#include <iostream>
#include <string>

#include "server/tcp_server.h"

int main(int argc, char* argv[]) {
  uint16_t port = 5432;
  if (argc > 1) {
    port = static_cast<uint16_t>(std::stoi(argv[1]));
  }
  std::string db_path = "litedb.db";
  if (argc > 2) {
    db_path = argv[2];
  }
  uint16_t web_port = 8080;
  if (argc > 3) {
    web_port = static_cast<uint16_t>(std::stoi(argv[3]));
  }

  try {
    litedb::server::TcpServer server(port, db_path, web_port);
    server.start();
  } catch (const std::exception& e) {
    std::cerr << "Fatal: " << e.what() << "\n";
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}
