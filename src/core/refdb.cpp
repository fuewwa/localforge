#include "refdb.hpp"

#include <git2/sys/refs.h>

#include <memory>
#include <string>
#include <vector>

#include "common.hpp"

namespace {

struct Backend {
  git_refdb_backend parent;
  Db* db;
  int64_t repo;
};

struct Row {
  std::string name;
  std::string target;
  bool symbolic = false;
};

struct Iter {
  git_reference_iterator parent{};
  std::vector<Row> rows;
  size_t pos = 0;
};

Backend* self(git_refdb_backend* b) { return reinterpret_cast<Backend*>(b); }

template <class F>
int guard(F&& fn) {
  try {
    return fn();
  } catch (const std::exception& e) {
    git_error_set_str(GIT_ERROR_REFERENCE, e.what());
    return -1;
  } catch (...) {
    git_error_set_str(GIT_ERROR_REFERENCE, "unknown error");
    return -1;
  }
}

bool glob_match(const char* pat, const char* s) {
  if (*pat == '\0') return *s == '\0';
  if (*pat == '*') {
    for (const char* p = s;; ++p) {
      if (glob_match(pat + 1, p)) return true;
      if (*p == '\0') return false;
    }
  }
  if (*pat == '?') return *s != '\0' && glob_match(pat + 1, s + 1);
  return *pat == *s && glob_match(pat + 1, s + 1);
}

bool fetch(Backend* b, const std::string& name, Row& row) {
  Stmt s(*b->db, "SELECT target, symbolic FROM refs WHERE repo=? AND name=?");
  s.bind_int(1, b->repo).bind_text(2, name);
  if (!s.step()) return false;
  row.name = name;
  row.target = s.col_text(0);
  row.symbolic = s.col_int(1) != 0;
  return true;
}

bool conflicts(Backend* b, const std::string& name) {
  {
    Stmt s(*b->db, "SELECT 1 FROM refs WHERE repo=? AND name>=? AND name<? LIMIT 1");
    s.bind_int(1, b->repo).bind_text(2, name + "/").bind_text(3, name + "0");
    if (s.step()) return true;
  }
  size_t pos = 0;
  while ((pos = name.find('/', pos)) != std::string::npos) {
    Stmt s(*b->db, "SELECT 1 FROM refs WHERE repo=? AND name=?");
    s.bind_int(1, b->repo).bind_text(2, name.substr(0, pos));
    if (s.step()) return true;
    ++pos;
  }
  return false;
}

git_reference* make_ref(const Row& row) {
  if (row.symbolic) return git_reference__alloc_symbolic(row.name.c_str(), row.target.c_str());
  git_oid oid;
  if (git_oid_fromstr(&oid, row.target.c_str()) < 0) return nullptr;
  return git_reference__alloc(row.name.c_str(), &oid, nullptr);
}

int put(Backend* b, const std::string& name, const std::string& target, bool symbolic) {
  Stmt s(*b->db,
         "INSERT INTO refs(repo, name, target, symbolic) VALUES(?, ?, ?, ?) "
         "ON CONFLICT(repo, name) DO UPDATE SET target=excluded.target, symbolic=excluded.symbolic");
  s.bind_int(1, b->repo).bind_text(2, name).bind_text(3, target).bind_int(4, symbolic ? 1 : 0);
  s.run();
  return 0;
}

int drop(Backend* b, const std::string& name) {
  Stmt s(*b->db, "DELETE FROM refs WHERE repo=? AND name=?");
  s.bind_int(1, b->repo).bind_text(2, name);
  s.run();
  return 0;
}

int match_old(const Row* cur, const git_oid* old_id, const char* old_target) {
  if (old_id) {
    if (is_zero(*old_id)) {
      if (cur) return GIT_EMODIFIED;
    } else if (!cur || cur->symbolic || cur->target != to_hex(*old_id)) {
      return GIT_EMODIFIED;
    }
  }
  if (old_target) {
    if (!cur || !cur->symbolic || cur->target != old_target) return GIT_EMODIFIED;
  }
  return 0;
}

int ref_exists(int* exists, git_refdb_backend* be, const char* name) {
  return guard([&] {
    Row row;
    *exists = fetch(self(be), name, row) ? 1 : 0;
    return 0;
  });
}

int ref_lookup(git_reference** out, git_refdb_backend* be, const char* name) {
  return guard([&] {
    Row row;
    if (!fetch(self(be), name, row)) return static_cast<int>(GIT_ENOTFOUND);
    git_reference* ref = make_ref(row);
    if (!ref) return -1;
    *out = ref;
    return 0;
  });
}

int iter_next(git_reference** out, git_reference_iterator* it) {
  Iter* iter = reinterpret_cast<Iter*>(it);
  while (iter->pos < iter->rows.size()) {
    git_reference* ref = make_ref(iter->rows[iter->pos++]);
    if (ref) {
      *out = ref;
      return 0;
    }
  }
  return GIT_ITEROVER;
}

int iter_next_name(const char** out, git_reference_iterator* it) {
  Iter* iter = reinterpret_cast<Iter*>(it);
  if (iter->pos >= iter->rows.size()) return GIT_ITEROVER;
  *out = iter->rows[iter->pos++].name.c_str();
  return 0;
}

void iter_free(git_reference_iterator* it) { delete reinterpret_cast<Iter*>(it); }

int ref_iterator(git_reference_iterator** out, git_refdb_backend* be, const char* glob) {
  return guard([&] {
    Backend* b = self(be);
    std::unique_ptr<Iter> iter(new Iter());
    Stmt s(*b->db, "SELECT name, target, symbolic FROM refs WHERE repo=? AND name>='refs/' AND name<'refs0' ORDER BY name");
    s.bind_int(1, b->repo);
    while (s.step()) {
      Row row;
      row.name = s.col_text(0);
      if (glob && *glob && !glob_match(glob, row.name.c_str())) continue;
      row.target = s.col_text(1);
      row.symbolic = s.col_int(2) != 0;
      iter->rows.push_back(std::move(row));
    }
    iter->parent.next = iter_next;
    iter->parent.next_name = iter_next_name;
    iter->parent.free = iter_free;
    *out = &iter.release()->parent;
    return 0;
  });
}

int write_ref(Backend* b, const git_reference* ref, int force, const git_oid* old_id,
              const char* old_target) {
  std::string name = git_reference_name(ref);
  Tx tx(*b->db);
  Row cur;
  bool exists = fetch(b, name, cur);
  if (!old_id && !old_target && exists && !force) {
    git_error_set(GIT_ERROR_REFERENCE, "reference '%s' already exists", name.c_str());
    return GIT_EEXISTS;
  }
  int rc = match_old(exists ? &cur : nullptr, old_id, old_target);
  if (rc < 0) {
    git_error_set(GIT_ERROR_REFERENCE, "reference '%s' value does not match expected value",
                  name.c_str());
    return rc;
  }
  if (!exists && conflicts(b, name)) {
    git_error_set(GIT_ERROR_REFERENCE, "reference '%s' conflicts with an existing reference",
                  name.c_str());
    return GIT_EEXISTS;
  }
  if (git_reference_type(ref) == GIT_REFERENCE_SYMBOLIC) {
    put(b, name, git_reference_symbolic_target(ref), true);
  } else {
    put(b, name, to_hex(*git_reference_target(ref)), false);
  }
  tx.commit();
  return 0;
}

int ref_write(git_refdb_backend* be, const git_reference* ref, int force, const git_signature*,
              const char*, const git_oid* old_id, const char* old_target) {
  return guard([&] { return write_ref(self(be), ref, force, old_id, old_target); });
}

int delete_ref(Backend* b, const char* name, const git_oid* old_id, const char* old_target) {
  Tx tx(*b->db);
  Row cur;
  if (!fetch(b, name, cur)) {
    git_error_set(GIT_ERROR_REFERENCE, "reference '%s' not found", name);
    return GIT_ENOTFOUND;
  }
  if (match_old(&cur, old_id, old_target) < 0) {
    git_error_set(GIT_ERROR_REFERENCE, "reference '%s' value does not match expected value", name);
    return GIT_EMODIFIED;
  }
  drop(b, name);
  tx.commit();
  return 0;
}

int ref_del(git_refdb_backend* be, const char* name, const git_oid* old_id,
            const char* old_target) {
  return guard([&] { return delete_ref(self(be), name, old_id, old_target); });
}

int ref_rename(git_reference** out, git_refdb_backend* be, const char* old_name,
               const char* new_name, int force, const git_signature*, const char*) {
  return guard([&] {
    Backend* b = self(be);
    Tx tx(*b->db);
    Row cur;
    if (!fetch(b, old_name, cur)) return static_cast<int>(GIT_ENOTFOUND);
    Row other;
    bool taken = fetch(b, new_name, other);
    if (taken && !force) return static_cast<int>(GIT_EEXISTS);
    drop(b, old_name);
    if (!taken && conflicts(b, new_name)) {
      git_error_set(GIT_ERROR_REFERENCE, "reference '%s' conflicts with an existing reference",
                    new_name);
      return static_cast<int>(GIT_EEXISTS);
    }
    put(b, new_name, cur.target, cur.symbolic);
    tx.commit();
    cur.name = new_name;
    git_reference* ref = make_ref(cur);
    if (!ref) return -1;
    *out = ref;
    return 0;
  });
}

int ref_has_log(git_refdb_backend*, const char*) { return 0; }
int ref_ensure_log(git_refdb_backend*, const char*) { return 0; }
int reflog_read(git_reflog**, git_refdb_backend*, const char*) { return GIT_ENOTFOUND; }
int reflog_write(git_refdb_backend*, git_reflog*) { return 0; }
int reflog_rename(git_refdb_backend*, const char*, const char*) { return 0; }
int reflog_delete(git_refdb_backend*, const char*) { return 0; }

int ref_lock(void** payload, git_refdb_backend*, const char* name) {
  *payload = new std::string(name);
  return 0;
}

int ref_unlock(git_refdb_backend* be, void* payload, int success, int, const git_reference* ref,
               const git_signature* sig, const char* message) {
  std::unique_ptr<std::string> name(static_cast<std::string*>(payload));
  if (success == 2) return ref_del(be, name->c_str(), nullptr, nullptr);
  if (success && ref) return ref_write(be, ref, 1, sig, message, nullptr, nullptr);
  return 0;
}

void ref_free(git_refdb_backend* be) { delete self(be); }

}  // namespace

git_refdb_backend* make_refdb_backend(Db* db, int64_t repo) {
  Backend* b = new Backend();
  git_refdb_init_backend(&b->parent, GIT_REFDB_BACKEND_VERSION);
  b->db = db;
  b->repo = repo;
  b->parent.exists = ref_exists;
  b->parent.lookup = ref_lookup;
  b->parent.iterator = ref_iterator;
  b->parent.write = ref_write;
  b->parent.rename = ref_rename;
  b->parent.del = ref_del;
  b->parent.has_log = ref_has_log;
  b->parent.ensure_log = ref_ensure_log;
  b->parent.free = ref_free;
  b->parent.reflog_read = reflog_read;
  b->parent.reflog_write = reflog_write;
  b->parent.reflog_rename = reflog_rename;
  b->parent.reflog_delete = reflog_delete;
  b->parent.lock = ref_lock;
  b->parent.unlock = ref_unlock;
  return &b->parent;
}
