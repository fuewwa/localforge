#include "store.hpp"

#include <algorithm>
#include <cctype>

#include "common.hpp"
#include "protocol.hpp"

namespace {

const char* kSchema =
    "CREATE TABLE IF NOT EXISTS repos("
    " id INTEGER PRIMARY KEY AUTOINCREMENT,"
    " name TEXT NOT NULL UNIQUE,"
    " description TEXT NOT NULL DEFAULT '',"
    " created INTEGER NOT NULL,"
    " updated INTEGER NOT NULL,"
    " status TEXT NOT NULL DEFAULT 'ready',"
    " error TEXT NOT NULL DEFAULT '');"
    "CREATE TABLE IF NOT EXISTS objects("
    " repo INTEGER NOT NULL REFERENCES repos(id) ON DELETE CASCADE,"
    " oid BLOB NOT NULL,"
    " type INTEGER NOT NULL,"
    " size INTEGER NOT NULL,"
    " data BLOB NOT NULL,"
    " PRIMARY KEY(repo, oid)) WITHOUT ROWID;"
    "CREATE TABLE IF NOT EXISTS refs("
    " repo INTEGER NOT NULL REFERENCES repos(id) ON DELETE CASCADE,"
    " name TEXT NOT NULL,"
    " target TEXT NOT NULL,"
    " symbolic INTEGER NOT NULL,"
    " PRIMARY KEY(repo, name)) WITHOUT ROWID;";

const char* kInfoSql =
    "SELECT r.name, r.description, r.status, r.error, r.created, r.updated,"
    " COALESCE((SELECT target FROM refs WHERE repo=r.id AND name='HEAD' AND symbolic=1), ''),"
    " EXISTS(SELECT 1 FROM refs WHERE repo=r.id AND name>='refs/heads/' AND name<'refs/heads0')"
    " FROM repos r";

std::string normalize(const std::string& path) {
  std::vector<std::string> parts;
  std::string cur;
  auto flush = [&] {
    if (cur.empty() || cur == ".") {
      cur.clear();
      return;
    }
    if (cur == "..") throw Invalid("invalid path");
    parts.push_back(cur);
    cur.clear();
  };
  for (char c : path) {
    if (c == '/') flush();
    else cur.push_back(c);
  }
  flush();
  std::string out;
  for (size_t i = 0; i < parts.size(); ++i) {
    if (i) out.push_back('/');
    out += parts[i];
  }
  return out;
}

void resolve(Handle& h, const std::string& ref, Commit& out) {
  git_oid oid;
  bool found = false;
  if (ref.size() == kOidHex && is_hex(ref)) {
    oid = from_hex(ref);
    found = true;
  } else {
    std::vector<std::string> candidates;
    if (ref.empty() || ref == "HEAD") {
      candidates.push_back("HEAD");
    } else {
      candidates.push_back("refs/heads/" + ref);
      candidates.push_back("refs/tags/" + ref);
    }
    for (const std::string& c : candidates) {
      if (git_reference_name_to_id(&oid, h.repo(), c.c_str()) == 0) {
        found = true;
        break;
      }
      git_error_clear();
    }
  }
  if (!found) throw NotFound("reference not found");
  Object obj;
  check(git_object_lookup(obj.out(), h.repo(), &oid, GIT_OBJECT_ANY));
  Object peeled;
  check(git_object_peel(peeled.out(), obj.get(), GIT_OBJECT_COMMIT));
  check(git_commit_lookup(out.out(), h.repo(), git_object_id(peeled.get())));
}

void root_tree(Handle& h, const std::string& ref, Tree& out) {
  Commit commit;
  resolve(h, ref, commit);
  check(git_commit_tree(out.out(), commit.get()));
}

CommitInfo describe(const git_commit* c) {
  CommitInfo info;
  info.id = to_hex(*git_commit_id(c));
  const char* summary = git_commit_summary(const_cast<git_commit*>(c));
  info.summary = clean_utf8(summary ? summary : "");
  const char* message = git_commit_message(c);
  info.message = clean_utf8(message ? message : "");
  const git_signature* author = git_commit_author(c);
  if (author) {
    info.author = clean_utf8(author->name ? author->name : "");
    info.email = clean_utf8(author->email ? author->email : "");
    info.time = author->when.time;
    info.offset = author->when.offset;
  }
  unsigned count = git_commit_parentcount(c);
  for (unsigned i = 0; i < count; ++i) info.parents.push_back(to_hex(*git_commit_parent_id(c, i)));
  return info;
}

std::string status_name(git_delta_t s) {
  switch (s) {
    case GIT_DELTA_ADDED: return "added";
    case GIT_DELTA_DELETED: return "deleted";
    case GIT_DELTA_MODIFIED: return "modified";
    case GIT_DELTA_RENAMED: return "renamed";
    case GIT_DELTA_COPIED: return "copied";
    case GIT_DELTA_TYPECHANGE: return "typechange";
    default: return "changed";
  }
}

std::string lower(std::string s) {
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

}  // namespace

Store::Store(const std::string& path) : path_(path) {
  Db db(path_);
  db.exec(kSchema);
  db.exec("UPDATE repos SET status='failed', error='interrupted' WHERE status='importing'");
}

Store::Row Store::lookup(Db& db, const std::string& name) {
  Stmt s(db, "SELECT id, status FROM repos WHERE name=?");
  s.bind_text(1, name);
  if (!s.step()) throw NotFound("repository not found");
  Row row;
  row.id = s.col_int(0);
  row.status = s.col_text(1);
  return row;
}

std::unique_ptr<Handle> Store::open(const std::string& name, bool require_ready) {
  if (!valid_name(name)) throw NotFound("repository not found");
  Row row;
  {
    Db db(path_);
    row = lookup(db, name);
  }
  if (require_ready && row.status != "ready") throw Invalid("repository is not ready: " + row.status);
  return std::make_unique<Handle>(path_, row.id);
}

RepoInfo Store::read_info(Db&, Stmt& s) {
  RepoInfo info;
  info.name = s.col_text(0);
  info.description = clean_utf8(s.col_text(1));
  info.status = s.col_text(2);
  info.error = clean_utf8(s.col_text(3));
  info.created = s.col_int(4);
  info.updated = s.col_int(5);
  std::string head = s.col_text(6);
  const std::string prefix = "refs/heads/";
  if (head.rfind(prefix, 0) == 0) info.branch = head.substr(prefix.size());
  info.empty = s.col_int(7) == 0;
  return info;
}

void Store::create(const std::string& name, const std::string& description,
                   const std::string& branch, const std::string& status) {
  if (!valid_name(name)) throw Invalid("invalid repository name");
  int valid = 0;
  git_branch_name_is_valid(&valid, branch.c_str());
  if (!valid) throw Invalid("invalid branch name");
  int64_t id = 0;
  {
    Db db(path_);
    Tx tx(db);
    Stmt exists(db, "SELECT 1 FROM repos WHERE name=?");
    exists.bind_text(1, name);
    if (exists.step()) throw Exists("repository already exists");
    Stmt ins(db, "INSERT INTO repos(name, description, created, updated, status) VALUES(?, ?, ?, ?, ?)");
    ins.bind_text(1, name).bind_text(2, description).bind_int(3, now()).bind_int(4, now());
    ins.bind_text(5, status);
    ins.run();
    id = sqlite3_last_insert_rowid(db.raw());
    tx.commit();
  }
  try {
    Handle h(path_, id);
    Reference head;
    check(git_reference_symbolic_create(head.out(), h.repo(), "HEAD", ("refs/heads/" + branch).c_str(),
                                        1, nullptr));
  } catch (...) {
    Db db(path_);
    Stmt del(db, "DELETE FROM repos WHERE id=?");
    del.bind_int(1, id).run();
    throw;
  }
}

void Store::remove(const std::string& name) {
  Db db(path_);
  Stmt s(db, "DELETE FROM repos WHERE name=?");
  s.bind_text(1, name).run();
  if (db.changes() == 0) throw NotFound("repository not found");
}

void Store::set_description(const std::string& name, const std::string& text) {
  Db db(path_);
  Stmt s(db, "UPDATE repos SET description=? WHERE name=?");
  s.bind_text(1, text).bind_text(2, name).run();
  if (db.changes() == 0) throw NotFound("repository not found");
}

void Store::set_status(const std::string& name, const std::string& status,
                       const std::string& error) {
  Db db(path_);
  Stmt s(db, "UPDATE repos SET status=?, error=?, updated=? WHERE name=?");
  s.bind_text(1, status).bind_text(2, error).bind_int(3, now()).bind_text(4, name).run();
  if (db.changes() == 0) throw NotFound("repository not found");
}

void Store::set_branch(const std::string& name, const std::string& branch) {
  int valid = 0;
  git_branch_name_is_valid(&valid, branch.c_str());
  if (!valid) throw Invalid("invalid branch name");
  std::unique_ptr<Handle> h = open(name, true);
  Reference head;
  check(git_reference_symbolic_create(head.out(), h->repo(), "HEAD", ("refs/heads/" + branch).c_str(),
                                      1, nullptr));
}

bool Store::exists(const std::string& name) {
  if (!valid_name(name)) return false;
  Db db(path_);
  Stmt s(db, "SELECT 1 FROM repos WHERE name=?");
  s.bind_text(1, name);
  return s.step();
}

RepoInfo Store::info(const std::string& name) {
  if (!valid_name(name)) throw NotFound("repository not found");
  Db db(path_);
  Stmt s(db, (std::string(kInfoSql) + " WHERE r.name=?").c_str());
  s.bind_text(1, name);
  if (!s.step()) throw NotFound("repository not found");
  return read_info(db, s);
}

std::vector<RepoInfo> Store::list() {
  Db db(path_);
  Stmt s(db, (std::string(kInfoSql) + " ORDER BY r.updated DESC, r.name ASC").c_str());
  std::vector<RepoInfo> out;
  while (s.step()) out.push_back(read_info(db, s));
  return out;
}

RefList Store::refs(const std::string& name) {
  std::unique_ptr<Handle> h = open(name, true);
  RefList out;
  git_reference_iterator* it = nullptr;
  check(git_reference_iterator_new(&it, h->repo()));
  const char* ref_name = nullptr;
  while (git_reference_next_name(&ref_name, it) == 0) {
    std::string n = ref_name;
    if (n.rfind("refs/heads/", 0) == 0) out.branches.push_back(n.substr(11));
    else if (n.rfind("refs/tags/", 0) == 0) out.tags.push_back(n.substr(10));
  }
  git_reference_iterator_free(it);
  git_error_clear();
  return out;
}

std::string Store::kind(const std::string& name, const std::string& ref, const std::string& path) {
  std::unique_ptr<Handle> h = open(name, true);
  std::string p = normalize(path);
  Tree root;
  root_tree(*h, ref, root);
  if (p.empty()) return "tree";
  TreeEntry entry;
  check(git_tree_entry_bypath(entry.out(), root.get(), p.c_str()));
  switch (git_tree_entry_type(entry.get())) {
    case GIT_OBJECT_TREE: return "tree";
    case GIT_OBJECT_BLOB: return "blob";
    default: return "submodule";
  }
}

std::vector<Entry> Store::tree(const std::string& name, const std::string& ref,
                               const std::string& path) {
  std::unique_ptr<Handle> h = open(name, true);
  std::string p = normalize(path);
  Tree root;
  root_tree(*h, ref, root);
  Tree sub;
  git_tree* current = root.get();
  if (!p.empty()) {
    TreeEntry entry;
    check(git_tree_entry_bypath(entry.out(), root.get(), p.c_str()));
    if (git_tree_entry_type(entry.get()) != GIT_OBJECT_TREE) throw Invalid("not a directory");
    check(git_tree_lookup(sub.out(), h->repo(), git_tree_entry_id(entry.get())));
    current = sub.get();
  }
  std::vector<Entry> out;
  size_t count = git_tree_entrycount(current);
  for (size_t i = 0; i < count; ++i) {
    const git_tree_entry* e = git_tree_entry_byindex(current, i);
    Entry item;
    item.name = clean_utf8(git_tree_entry_name(e));
    switch (git_tree_entry_type(e)) {
      case GIT_OBJECT_TREE: item.kind = "tree"; break;
      case GIT_OBJECT_BLOB: {
        item.kind = "blob";
        size_t len = 0;
        git_object_t type;
        if (git_odb_read_header(&len, &type, h->odb(), git_tree_entry_id(e)) == 0) {
          item.size = static_cast<int64_t>(len);
        } else {
          git_error_clear();
        }
        break;
      }
      default: item.kind = "submodule"; break;
    }
    out.push_back(std::move(item));
  }
  auto rank = [](const Entry& e) { return e.kind == "tree" ? 0 : (e.kind == "submodule" ? 1 : 2); };
  std::sort(out.begin(), out.end(), [&](const Entry& a, const Entry& b) {
    if (rank(a) != rank(b)) return rank(a) < rank(b);
    std::string la = lower(a.name), lb = lower(b.name);
    if (la != lb) return la < lb;
    return a.name < b.name;
  });
  return out;
}

FileData Store::blob(const std::string& name, const std::string& ref, const std::string& path,
                     int64_t limit) {
  std::unique_ptr<Handle> h = open(name, true);
  std::string p = normalize(path);
  if (p.empty()) throw Invalid("path required");
  Tree root;
  root_tree(*h, ref, root);
  TreeEntry entry;
  check(git_tree_entry_bypath(entry.out(), root.get(), p.c_str()));
  if (git_tree_entry_type(entry.get()) != GIT_OBJECT_BLOB) throw Invalid("not a file");
  Blob blob;
  check(git_blob_lookup(blob.out(), h->repo(), git_tree_entry_id(entry.get())));
  FileData out;
  out.size = static_cast<int64_t>(git_blob_rawsize(blob.get()));
  out.binary = git_blob_is_binary(blob.get()) != 0;
  size_t take = static_cast<size_t>(out.size);
  if (limit >= 0 && static_cast<int64_t>(take) > limit) {
    take = static_cast<size_t>(limit);
    out.truncated = true;
  }
  out.data.assign(static_cast<const char*>(git_blob_rawcontent(blob.get())), take);
  return out;
}

std::vector<CommitInfo> Store::log(const std::string& name, const std::string& ref, int64_t offset,
                                   int64_t limit) {
  std::unique_ptr<Handle> h = open(name, true);
  Commit start;
  resolve(*h, ref, start);
  Revwalk walk;
  check(git_revwalk_new(walk.out(), h->repo()));
  git_revwalk_sorting(walk.get(), GIT_SORT_TIME);
  check(git_revwalk_push(walk.get(), git_commit_id(start.get())));
  std::vector<CommitInfo> out;
  git_oid oid;
  int64_t index = 0;
  while (git_revwalk_next(&oid, walk.get()) == 0) {
    if (index++ < offset) continue;
    if (static_cast<int64_t>(out.size()) >= limit) break;
    Commit c;
    check(git_commit_lookup(c.out(), h->repo(), &oid));
    out.push_back(describe(c.get()));
  }
  git_error_clear();
  return out;
}

CommitDetail Store::commit(const std::string& name, const std::string& id, int64_t max_patch) {
  std::unique_ptr<Handle> h = open(name, true);
  if (id.size() < 4 || id.size() > kOidHex || !is_hex(id)) throw Invalid("invalid commit id");
  git_oid oid;
  check(git_oid_fromstrn(&oid, id.c_str(), id.size()));
  Object obj;
  check(git_object_lookup_prefix(obj.out(), h->repo(), &oid, id.size(), GIT_OBJECT_COMMIT));
  Commit commit;
  check(git_commit_lookup(commit.out(), h->repo(), git_object_id(obj.get())));
  CommitDetail out;
  out.info = describe(commit.get());

  Tree tree;
  check(git_commit_tree(tree.out(), commit.get()));
  Tree parent_tree;
  if (git_commit_parentcount(commit.get()) > 0) {
    Commit parent;
    check(git_commit_parent(parent.out(), commit.get(), 0));
    check(git_commit_tree(parent_tree.out(), parent.get()));
  }
  git_diff_options opts = GIT_DIFF_OPTIONS_INIT;
  Diff diff;
  check(git_diff_tree_to_tree(diff.out(), h->repo(), parent_tree.get(), tree.get(), &opts));
  git_diff_find_options find = GIT_DIFF_FIND_OPTIONS_INIT;
  git_diff_find_similar(diff.get(), &find);

  size_t count = git_diff_num_deltas(diff.get());
  for (size_t i = 0; i < count; ++i) {
    FileChange fc;
    Patch patch;
    if (git_patch_from_diff(patch.out(), diff.get(), i) == 0 && patch) {
      size_t adds = 0, dels = 0;
      git_patch_line_stats(nullptr, &adds, &dels, patch.get());
      fc.additions = static_cast<int64_t>(adds);
      fc.deletions = static_cast<int64_t>(dels);
    } else {
      git_error_clear();
    }
    const git_diff_delta* d = git_diff_get_delta(diff.get(), i);
    fc.path = clean_utf8(d->new_file.path ? d->new_file.path : "");
    fc.old_path = clean_utf8(d->old_file.path ? d->old_file.path : "");
    fc.status = status_name(d->status);
    fc.binary = (d->flags & GIT_DIFF_FLAG_BINARY) != 0;
    out.files.push_back(std::move(fc));
  }

  git_buf buf = GIT_BUF_INIT;
  int rc = git_diff_to_buf(&buf, diff.get(), GIT_DIFF_FORMAT_PATCH);
  if (rc < 0) {
    git_buf_dispose(&buf);
    check(rc);
  }
  size_t take = buf.size;
  if (max_patch >= 0 && static_cast<int64_t>(take) > max_patch) {
    take = static_cast<size_t>(max_patch);
    out.truncated = true;
  }
  out.patch = clean_utf8(std::string(buf.ptr ? buf.ptr : "", take));
  git_buf_dispose(&buf);
  return out;
}

void Store::fetch(const std::string& name, const std::string& url) {
  std::unique_ptr<Handle> h = open(name, false);
  Remote remote;
  check(git_remote_create_anonymous(remote.out(), h->repo(), url.c_str()));
  git_fetch_options opts = GIT_FETCH_OPTIONS_INIT;
#ifdef GIT_REMOTE_UPDATE_FETCHHEAD
  opts.update_flags = 0;
#else
  opts.update_fetchhead = 0;
#endif
  opts.download_tags = GIT_REMOTE_DOWNLOAD_TAGS_NONE;
  const char* specs[] = {"+refs/heads/*:refs/heads/*", "+refs/tags/*:refs/tags/*"};
  git_strarray arr;
  arr.strings = const_cast<char**>(specs);
  arr.count = 2;
  check(git_remote_fetch(remote.get(), &arr, &opts, nullptr));
  repair_head(*h, "");
  touch(*h);
}

std::string Store::advertise(const std::string& name, const std::string& service) {
  std::unique_ptr<Handle> h = open(name, true);
  return ::advertise(*h, service);
}

std::string Store::upload_pack(const std::string& name, const std::string& request) {
  std::unique_ptr<Handle> h = open(name, true);
  return ::upload_pack(*h, request);
}

std::string Store::receive_pack(const std::string& name, const std::string& request) {
  std::unique_ptr<Handle> h = open(name, true);
  return ::receive_pack(*h, request);
}
