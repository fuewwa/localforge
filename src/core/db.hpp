#pragma once

#include <sqlite3.h>

#include <cstdint>
#include <string>

class Db {
 public:
  explicit Db(const std::string& path);
  ~Db();
  Db(const Db&) = delete;
  Db& operator=(const Db&) = delete;

  sqlite3* raw() const { return db_; }
  void exec(const char* sql);
  void begin();
  void commit();
  void rollback();
  int64_t changes() const;

 private:
  sqlite3* db_ = nullptr;
};

class Stmt {
 public:
  Stmt(Db& db, const char* sql);
  ~Stmt();
  Stmt(const Stmt&) = delete;
  Stmt& operator=(const Stmt&) = delete;

  Stmt& bind_int(int idx, int64_t value);
  Stmt& bind_text(int idx, const std::string& value);
  Stmt& bind_blob(int idx, const void* data, size_t len);
  bool step();
  void run();
  void reset();

  int64_t col_int(int idx) const;
  std::string col_text(int idx) const;
  const void* col_blob(int idx) const;
  size_t col_bytes(int idx) const;

 private:
  Db& db_;
  sqlite3_stmt* stmt_ = nullptr;
};

class Tx {
 public:
  explicit Tx(Db& db) : db_(db) { db_.begin(); }
  ~Tx() {
    if (!done_) {
      try {
        db_.rollback();
      } catch (...) {
      }
    }
  }
  Tx(const Tx&) = delete;
  Tx& operator=(const Tx&) = delete;
  void commit() {
    db_.commit();
    done_ = true;
  }

 private:
  Db& db_;
  bool done_ = false;
};
