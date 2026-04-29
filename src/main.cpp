#include <cstdio>

int main() {
  // The TCP server / SQL frontend will live here once Phases 5 & 6 land.
  // For now, this binary just prints a banner so `litedb` is buildable
  // end-to-end while the storage layer is being developed.
  std::puts("LiteDB: storage engine under construction.");
  return 0;
}
