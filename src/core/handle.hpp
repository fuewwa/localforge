#pragma once

#include <git2.h>

#include <cstdint>
#include <string>

#include "db.hpp"

class Handle {
 public:
  Handle(const std::string& path, int64_t id);
  ~Handle();
  Handle(const Handle&) = delete;
  Handle& operator=(const Handle&) = delete;

  git_repository* repo() const { return repo_; }
  git_odb* odb() const { return odb_; }
  Db& db() { return db_; }
  int64_t id() const { return id_; }

 private:
  Db db_;
  int64_t id_;
  git_repository* repo_ = nullptr;
  git_odb* odb_ = nullptr;
};
