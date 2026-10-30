#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "handle.hpp"

struct RepoInfo {
  std::string name;
  std::string description;
  std::string status;
  std::string error;
  std::string branch;
  int64_t created = 0;
  int64_t updated = 0;
  bool empty = true;
};

struct Entry {
  std::string name;
  std::string kind;
  int64_t size = 0;
};

struct CommitInfo {
  std::string id;
  std::string summary;
  std::string message;
  std::string author;
  std::string email;
  int64_t time = 0;
  int offset = 0;
  std::vector<std::string> parents;
};

struct FileChange {
  std::string path;
  std::string old_path;
  std::string status;
  int64_t additions = 0;
  int64_t deletions = 0;
  bool binary = false;
};

struct CommitDetail {
  CommitInfo info;
  std::vector<FileChange> files;
  std::string patch;
  bool truncated = false;
};

struct FileData {
  int64_t size = 0;
  bool binary = false;
  bool truncated = false;
  std::string data;
};

struct RefList {
  std::vector<std::string> branches;
  std::vector<std::string> tags;
};

class Store {
 public:
  explicit Store(const std::string& path);

  void create(const std::string& name, const std::string& description, const std::string& branch,
              const std::string& status);
  void remove(const std::string& name);
  void set_description(const std::string& name, const std::string& text);
  void set_status(const std::string& name, const std::string& status, const std::string& error);
  void set_branch(const std::string& name, const std::string& branch);
  bool exists(const std::string& name);
  RepoInfo info(const std::string& name);
  std::vector<RepoInfo> list();

  RefList refs(const std::string& name);
  std::string kind(const std::string& name, const std::string& ref, const std::string& path);
  std::vector<Entry> tree(const std::string& name, const std::string& ref, const std::string& path);
  FileData blob(const std::string& name, const std::string& ref, const std::string& path,
                int64_t limit);
  std::vector<CommitInfo> log(const std::string& name, const std::string& ref, int64_t offset,
                              int64_t limit);
  CommitDetail commit(const std::string& name, const std::string& id, int64_t max_patch);

  void fetch(const std::string& name, const std::string& url);
  std::string advertise(const std::string& name, const std::string& service);
  std::string upload_pack(const std::string& name, const std::string& request);
  std::string receive_pack(const std::string& name, const std::string& request);

 private:
  struct Row {
    int64_t id = 0;
    std::string status;
  };

  Row lookup(Db& db, const std::string& name);
  std::unique_ptr<Handle> open(const std::string& name, bool require_ready);
  RepoInfo read_info(Db& db, Stmt& s);

  std::string path_;
};
