#include "odb.hpp"

#include <zlib.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <random>
#include <string>

#include "common.hpp"

namespace {

struct Backend {
  git_odb_backend parent;
  Db* db;
  int64_t repo;
};

struct PackWriter {
  git_odb_writepack parent;
  Backend* backend;
  git_indexer* indexer;
  std::filesystem::path dir;
};

Backend* self(git_odb_backend* b) { return reinterpret_cast<Backend*>(b); }

template <class F>
int guard(F&& fn) {
  try {
    return fn();
  } catch (const std::exception& e) {
    git_error_set_str(GIT_ERROR_ODB, e.what());
    return -1;
  } catch (...) {
    git_error_set_str(GIT_ERROR_ODB, "unknown error");
    return -1;
  }
}

std::string deflate_bytes(const void* data, size_t len) {
  const void* src = data ? data : "";
  uLongf bound = compressBound(static_cast<uLong>(len));
  std::string out(bound, '\0');
  int rc = compress2(reinterpret_cast<Bytef*>(&out[0]), &bound, static_cast<const Bytef*>(src),
                     static_cast<uLong>(len), Z_DEFAULT_COMPRESSION);
  if (rc != Z_OK) throw Error("compression failed");
  out.resize(bound);
  return out;
}

void put(Stmt& ins, int64_t repo, const git_oid* oid, git_object_t type, const void* data,
         size_t len) {
  std::string packed = deflate_bytes(data, len);
  ins.reset();
  ins.bind_int(1, repo);
  ins.bind_blob(2, oid->id, kOidSize);
  ins.bind_int(3, static_cast<int64_t>(type));
  ins.bind_int(4, static_cast<int64_t>(len));
  ins.bind_blob(5, packed.data(), packed.size());
  ins.run();
}

const char* kInsert =
    "INSERT OR IGNORE INTO objects(repo, oid, type, size, data) VALUES(?, ?, ?, ?, ?)";

void make_oid(git_oid* out, const void* raw) {
#ifdef GIT_EXPERIMENTAL_SHA256
  git_oid_fromraw(out, static_cast<const unsigned char*>(raw), GIT_OID_SHA1);
#else
  git_oid_fromraw(out, static_cast<const unsigned char*>(raw));
#endif
}

void prefix_bounds(const git_oid* shortid, size_t len, unsigned char* lo, unsigned char* hi) {
  for (size_t i = 0; i < kOidSize; ++i) {
    if (i < len / 2) {
      lo[i] = hi[i] = shortid->id[i];
    } else if (i == len / 2 && (len % 2)) {
      lo[i] = shortid->id[i] & 0xF0;
      hi[i] = shortid->id[i] | 0x0F;
    } else {
      lo[i] = 0x00;
      hi[i] = 0xFF;
    }
  }
}

int find_prefix(Backend* b, const git_oid* shortid, size_t len, git_oid* out) {
  unsigned char lo[kOidSize];
  unsigned char hi[kOidSize];
  prefix_bounds(shortid, len, lo, hi);
  Stmt s(*b->db, "SELECT oid FROM objects WHERE repo=? AND oid>=? AND oid<=? LIMIT 2");
  s.bind_int(1, b->repo).bind_blob(2, lo, kOidSize).bind_blob(3, hi, kOidSize);
  if (!s.step()) return GIT_ENOTFOUND;
  make_oid(out, s.col_blob(0));
  if (s.step()) return GIT_EAMBIGUOUS;
  return 0;
}

int load(Backend* b, const git_oid* oid, void** data_p, size_t* len_p, git_object_t* type_p) {
  Stmt s(*b->db, "SELECT type, size, data FROM objects WHERE repo=? AND oid=?");
  s.bind_int(1, b->repo).bind_blob(2, oid->id, kOidSize);
  if (!s.step()) return GIT_ENOTFOUND;
  size_t size = static_cast<size_t>(s.col_int(1));
  void* buf = git_odb_backend_data_alloc(&b->parent, size ? size : 1);
  if (!buf) return -1;
  uLongf out = static_cast<uLongf>(size);
  int rc = uncompress(static_cast<Bytef*>(buf), &out, static_cast<const Bytef*>(s.col_blob(2)),
                      static_cast<uLong>(s.col_bytes(2)));
  if (rc != Z_OK || out != size) {
    git_odb_backend_data_free(&b->parent, buf);
    git_error_set_str(GIT_ERROR_ODB, "corrupt object data");
    return -1;
  }
  *data_p = buf;
  *len_p = size;
  *type_p = static_cast<git_object_t>(s.col_int(0));
  return 0;
}

int odb_read(void** data_p, size_t* len_p, git_object_t* type_p, git_odb_backend* be,
             const git_oid* oid) {
  return guard([&] { return load(self(be), oid, data_p, len_p, type_p); });
}

int odb_read_prefix(git_oid* out, void** data_p, size_t* len_p, git_object_t* type_p,
                    git_odb_backend* be, const git_oid* shortid, size_t len) {
  return guard([&] {
    Backend* b = self(be);
    if (len >= kOidHex) {
      *out = *shortid;
      return load(b, shortid, data_p, len_p, type_p);
    }
    int rc = find_prefix(b, shortid, len, out);
    if (rc < 0) return rc;
    return load(b, out, data_p, len_p, type_p);
  });
}

int odb_read_header(size_t* len_p, git_object_t* type_p, git_odb_backend* be, const git_oid* oid) {
  return guard([&] {
    Backend* b = self(be);
    Stmt s(*b->db, "SELECT type, size FROM objects WHERE repo=? AND oid=?");
    s.bind_int(1, b->repo).bind_blob(2, oid->id, kOidSize);
    if (!s.step()) return static_cast<int>(GIT_ENOTFOUND);
    *type_p = static_cast<git_object_t>(s.col_int(0));
    *len_p = static_cast<size_t>(s.col_int(1));
    return 0;
  });
}

int odb_write(git_odb_backend* be, const git_oid* oid, const void* data, size_t len,
              git_object_t type) {
  return guard([&] {
    Backend* b = self(be);
    Stmt ins(*b->db, kInsert);
    put(ins, b->repo, oid, type, data, len);
    return 0;
  });
}

int odb_exists(git_odb_backend* be, const git_oid* oid) {
  return guard([&] {
    Backend* b = self(be);
    Stmt s(*b->db, "SELECT 1 FROM objects WHERE repo=? AND oid=?");
    s.bind_int(1, b->repo).bind_blob(2, oid->id, kOidSize);
    return s.step() ? 1 : 0;
  });
}

int odb_exists_prefix(git_oid* out, git_odb_backend* be, const git_oid* shortid, size_t len) {
  return guard([&] {
    if (len >= kOidHex) {
      *out = *shortid;
      return odb_exists(be, shortid) ? 0 : static_cast<int>(GIT_ENOTFOUND);
    }
    return find_prefix(self(be), shortid, len, out);
  });
}

int odb_freshen(git_odb_backend* be, const git_oid* oid) {
  int rc = odb_exists(be, oid);
  return rc == 1 ? 0 : -1;
}

int odb_foreach(git_odb_backend* be, git_odb_foreach_cb cb, void* payload) {
  return guard([&] {
    Backend* b = self(be);
    Stmt s(*b->db, "SELECT oid FROM objects WHERE repo=?");
    s.bind_int(1, b->repo);
    while (s.step()) {
      git_oid oid;
      make_oid(&oid, s.col_blob(0));
      int rc = cb(&oid, payload);
      if (rc != 0) return rc;
    }
    return 0;
  });
}

std::filesystem::path make_temp_dir() {
  static std::atomic<uint64_t> counter{0};
  std::random_device rd;
  uint64_t r = (static_cast<uint64_t>(rd()) << 32) ^ rd();
  char buf[96];
  std::snprintf(buf, sizeof(buf), "pack-%016llx-%llu", static_cast<unsigned long long>(r),
                static_cast<unsigned long long>(counter++));
  std::filesystem::path p = std::filesystem::temp_directory_path() / buf;
  std::filesystem::create_directories(p);
  return p;
}

struct CopyCtx {
  Backend* backend;
  git_odb* source;
  Stmt* ins;
};

int copy_object(const git_oid* oid, void* payload) {
  CopyCtx* ctx = static_cast<CopyCtx*>(payload);
  OdbObject obj;
  int rc = git_odb_read(obj.out(), ctx->source, oid);
  if (rc < 0) return rc;
  put(*ctx->ins, ctx->backend->repo, oid, git_odb_object_type(obj.get()),
      git_odb_object_data(obj.get()), git_odb_object_size(obj.get()));
  return 0;
}

int pack_append(git_odb_writepack* wp, const void* data, size_t size, git_indexer_progress* stats) {
  PackWriter* w = reinterpret_cast<PackWriter*>(wp);
  return git_indexer_append(w->indexer, data, size, stats);
}

int pack_commit(git_odb_writepack* wp, git_indexer_progress* stats) {
  PackWriter* w = reinterpret_cast<PackWriter*>(wp);
  return guard([&] {
    int rc = git_indexer_commit(w->indexer, stats);
    if (rc < 0) return rc;
    std::filesystem::path idx = w->dir / (std::string("pack-") + git_indexer_name(w->indexer) + ".idx");
    Odb tmp;
    rc = git_odb_new(tmp.out());
    if (rc < 0) return rc;
    git_odb_backend* pb = nullptr;
    rc = git_odb_backend_one_pack(&pb, idx.string().c_str());
    if (rc < 0) return rc;
    rc = git_odb_add_backend(tmp.get(), pb, 1);
    if (rc < 0) return rc;
    Tx tx(*w->backend->db);
    Stmt ins(*w->backend->db, kInsert);
    CopyCtx ctx{w->backend, tmp.get(), &ins};
    rc = git_odb_foreach(tmp.get(), copy_object, &ctx);
    if (rc < 0) return rc;
    tx.commit();
    return 0;
  });
}

void pack_free(git_odb_writepack* wp) {
  PackWriter* w = reinterpret_cast<PackWriter*>(wp);
  if (w->indexer) git_indexer_free(w->indexer);
  std::error_code ec;
  std::filesystem::remove_all(w->dir, ec);
  delete w;
}

int odb_writepack(git_odb_writepack** out, git_odb_backend* be, git_odb* odb,
                  git_indexer_progress_cb progress_cb, void* progress_payload) {
  return guard([&] {
    PackWriter* w = new PackWriter();
    w->backend = self(be);
    w->indexer = nullptr;
    w->parent.backend = be;
    w->parent.append = pack_append;
    w->parent.commit = pack_commit;
    w->parent.free = pack_free;
    w->dir = make_temp_dir();
    git_indexer_options opts = GIT_INDEXER_OPTIONS_INIT;
    opts.progress_cb = progress_cb;
    opts.progress_cb_payload = progress_payload;
    int rc = git_indexer_new(&w->indexer, w->dir.string().c_str(), 0, odb, &opts);
    if (rc < 0) {
      pack_free(&w->parent);
      return rc;
    }
    *out = &w->parent;
    return 0;
  });
}

void odb_free(git_odb_backend* be) { delete self(be); }

}  // namespace

git_odb_backend* make_odb_backend(Db* db, int64_t repo) {
  Backend* b = new Backend();
  git_odb_init_backend(&b->parent, GIT_ODB_BACKEND_VERSION);
  b->db = db;
  b->repo = repo;
  b->parent.read = odb_read;
  b->parent.read_prefix = odb_read_prefix;
  b->parent.read_header = odb_read_header;
  b->parent.write = odb_write;
  b->parent.exists = odb_exists;
  b->parent.exists_prefix = odb_exists_prefix;
  b->parent.freshen = odb_freshen;
  b->parent.foreach = odb_foreach;
  b->parent.writepack = odb_writepack;
  b->parent.free = odb_free;
  return &b->parent;
}
