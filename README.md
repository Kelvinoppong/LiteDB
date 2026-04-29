# LiteDB

An embedded relational database engine written in C++17, built from scratch as a learning project.

## Status

Work in progress. Built incrementally in phases.

| Phase | Component                          | Status      |
| ----- | ---------------------------------- | ----------- |
| 1     | Page manager + buffer pool         | In progress |
| 2     | B+ Tree index                      | Planned     |
| 3     | Write-ahead log + crash recovery   | Planned     |
| 4     | MVCC concurrency control           | Planned     |
| 5     | SQL parser + query executor        | Planned     |
| 6     | PostgreSQL wire protocol           | Planned     |

## Build

Requires CMake 3.15+, a C++17 compiler, and GoogleTest.

```bash
sudo apt-get install -y cmake g++ libgtest-dev
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Debug
make -j$(nproc)
ctest --output-on-failure
```

## Layout

```
src/
  storage/      4KB pages, disk manager, buffer pool, B+ tree, LSM tree
  wal/          write-ahead log, REDO/UNDO recovery
  concurrency/  MVCC version chains, transaction manager
  parser/       lexer + recursive-descent SQL parser
  executor/     query plan execution
  server/       TCP server + PostgreSQL wire protocol
tests/          GoogleTest unit tests
benchmarks/     TPC-C and SQLite parity checks
```

## Phase 1 — Page Manager + Buffer Pool

The current phase implements the storage foundation:

- **Page** — fixed 4KB slotted page with header (`page_id`, `lsn`,
  `free_space_offset`, `slot_count`) and a slot directory growing forward
  while tuple data grows backward from the page end.
- **DiskManager** — pread/pwrite-based page I/O against a single database
  file. Allocates new pages by extending the file.
- **BufferPool** — fixed-size frame pool with a page table, free list, and
  LRU replacement policy. Supports `fetch_page`, `new_page`, `unpin_page`,
  `flush_page`, and `flush_all`. Pinned pages are never evicted.

The Phase 1 deliverable test writes 1,000 pages, closes the file, reopens
it, and verifies every page reads back identically.
