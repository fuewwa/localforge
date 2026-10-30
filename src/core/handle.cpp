#include "handle.hpp"

#include <cstdio>
#include <filesystem>
#include <random>
#include <string>

#include <git2/sys/repository.h>

#include "common.hpp"
#include "odb.hpp"
#include "refdb.hpp"

namespace {

struct Skeleton {
  std::filesystem::path dir;

  Skeleton() {
    std::random_device rd;
    char buf[64];
    std::snprintf(buf, sizeof(buf), "skeleton-%08x%08x", rd(), rd());
    dir = std::filesystem::temp_directory_path() / buf;
    git_repository* repo = nullptr;
    check(git_repository_init(&repo, dir.string().c_str(), 1));
    git_repository_free(repo);
  }

  ~Skeleton() {
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
  }
};

const std::string& skeleton_path() {
  static Skeleton skeleton;
  static const std::string path = skeleton.dir.string();
  return path;
}

}  // namespace

Handle::Handle(const std::string& path, int64_t id) : db_(path), id_(id) {
  git_odb* odb = nullptr;
  git_refdb* refdb = nullptr;
  git_config* config = nullptr;
  try {
    check(git_odb_new(&odb));
    git_odb_backend* ob = make_odb_backend(&db_, id_);
    int rc = git_odb_add_backend(odb, ob, 1);
    if (rc < 0) {
      ob->free(ob);
      check(rc);
    }
    check(git_repository_open_bare(&repo_, skeleton_path().c_str()));
    check(git_repository_set_odb(repo_, odb));
    check(git_repository_odb(&odb_, repo_));
    check(git_refdb_new(&refdb, repo_));
    git_refdb_backend* rb = make_refdb_backend(&db_, id_);
    rc = git_refdb_set_backend(refdb, rb);
    if (rc < 0) {
      rb->free(rb);
      check(rc);
    }
    check(git_repository_set_refdb(repo_, refdb));
    check(git_config_new(&config));
    check(git_repository_set_config(repo_, config));
  } catch (...) {
    if (config) git_config_free(config);
    if (refdb) git_refdb_free(refdb);
    if (odb_) git_odb_free(odb_);
    if (repo_) git_repository_free(repo_);
    if (odb) git_odb_free(odb);
    throw;
  }
  git_config_free(config);
  git_refdb_free(refdb);
  git_odb_free(odb);
}

Handle::~Handle() {
  if (odb_) git_odb_free(odb_);
  if (repo_) git_repository_free(repo_);
}
