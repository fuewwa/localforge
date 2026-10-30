#include "db.hpp"

#include "common.hpp"

namespace {

[[noreturn]] void fail(sqlite3* db, const char* what) {
  std::string msg = what;
  if (db) {
    msg += ": ";
    msg += sqlite3_errmsg(db);
  }
  throw Error(msg);
}

}  // namespace

Db::Db(const std::string& path) {
  int rc = sqlite3_open_v2(path.c_str(), &db_,
                           SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
                           nullptr);
  if (rc != SQLITE_OK) {
    std::string msg = db_ ? sqlite3_errmsg(db_) : "cannot open database";
    if (db_) sqlite3_close(db_);
    db_ = nullptr;
    throw Error("cannot open database: " + msg);
  }
  sqlite3_busy_timeout(db_, 60000);
  exec("PRAGMA foreign_keys=ON");
  exec("PRAGMA journal_mode=WAL");
  exec("PRAGMA synchronous=NORMAL");
}

Db::~Db() {
  if (db_) sqlite3_close(db_);
}

void Db::exec(const char* sql) {
  char* err = nullptr;
  int rc = sqlite3_exec(db_, sql, nullptr, nullptr, &err);
  if (rc != SQLITE_OK) {
    std::string msg = err ? err : "sql error";
    sqlite3_free(err);
    throw Error(msg);
  }
}

void Db::begin() { exec("BEGIN IMMEDIATE"); }
void Db::commit() { exec("COMMIT"); }
void Db::rollback() { exec("ROLLBACK"); }
int64_t Db::changes() const { return sqlite3_changes(db_); }

Stmt::Stmt(Db& db, const char* sql) : db_(db) {
  if (sqlite3_prepare_v2(db.raw(), sql, -1, &stmt_, nullptr) != SQLITE_OK) {
    fail(db.raw(), "prepare failed");
  }
}

Stmt::~Stmt() {
  if (stmt_) sqlite3_finalize(stmt_);
}

Stmt& Stmt::bind_int(int idx, int64_t value) {
  if (sqlite3_bind_int64(stmt_, idx, value) != SQLITE_OK) fail(db_.raw(), "bind failed");
  return *this;
}

Stmt& Stmt::bind_text(int idx, const std::string& value) {
  if (sqlite3_bind_text(stmt_, idx, value.data(), static_cast<int>(value.size()),
                        SQLITE_TRANSIENT) != SQLITE_OK) {
    fail(db_.raw(), "bind failed");
  }
  return *this;
}

Stmt& Stmt::bind_blob(int idx, const void* data, size_t len) {
  const void* ptr = data ? data : "";
  if (sqlite3_bind_blob64(stmt_, idx, ptr, len, SQLITE_TRANSIENT) != SQLITE_OK) {
    fail(db_.raw(), "bind failed");
  }
  return *this;
}

bool Stmt::step() {
  int rc = sqlite3_step(stmt_);
  if (rc == SQLITE_ROW) return true;
  if (rc == SQLITE_DONE) return false;
  fail(db_.raw(), "step failed");
}

void Stmt::run() { step(); }

void Stmt::reset() {
  sqlite3_reset(stmt_);
  sqlite3_clear_bindings(stmt_);
}

int64_t Stmt::col_int(int idx) const { return sqlite3_column_int64(stmt_, idx); }

std::string Stmt::col_text(int idx) const {
  const unsigned char* p = sqlite3_column_text(stmt_, idx);
  int n = sqlite3_column_bytes(stmt_, idx);
  return p ? std::string(reinterpret_cast<const char*>(p), static_cast<size_t>(n)) : std::string();
}

const void* Stmt::col_blob(int idx) const { return sqlite3_column_blob(stmt_, idx); }

size_t Stmt::col_bytes(int idx) const { return static_cast<size_t>(sqlite3_column_bytes(stmt_, idx)); }
