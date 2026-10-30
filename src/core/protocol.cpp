#include "protocol.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>

#include "common.hpp"

namespace {

const char* kAgent = "agent=engine/1";
const size_t kChunk = 65515;

std::string pkt(const std::string& s) {
  char head[8];
  std::snprintf(head, sizeof(head), "%04zx", s.size() + 4);
  return std::string(head) + s;
}

std::string one_line(std::string s) {
  for (char& c : s) {
    if (c == '\n' || c == '\r') c = ' ';
  }
  return clean_utf8(s);
}

class PktReader {
 public:
  enum Kind { Data, Flush, Delim, End };

  explicit PktReader(const std::string& buf) : buf_(buf) {}

  Kind next(std::string& line) {
    if (pos_ + 4 > buf_.size()) return End;
    unsigned len = 0;
    for (int i = 0; i < 4; ++i) {
      char c = buf_[pos_ + i];
      unsigned v;
      if (c >= '0' && c <= '9') v = c - '0';
      else if (c >= 'a' && c <= 'f') v = 10 + c - 'a';
      else if (c >= 'A' && c <= 'F') v = 10 + c - 'A';
      else throw Invalid("malformed packet line");
      len = len * 16 + v;
    }
    if (len == 0) {
      pos_ += 4;
      return Flush;
    }
    if (len == 1) {
      pos_ += 4;
      return Delim;
    }
    if (len < 4 || pos_ + len > buf_.size()) throw Invalid("malformed packet line");
    line.assign(buf_, pos_ + 4, len - 4);
    pos_ += len;
    if (!line.empty() && line.back() == '\n') line.pop_back();
    return Data;
  }

  size_t pos() const { return pos_; }

 private:
  const std::string& buf_;
  size_t pos_ = 0;
};

struct RefEntry {
  std::string name;
  git_oid oid;
  bool has_peeled = false;
  git_oid peeled;
};

std::vector<RefEntry> collect(Handle& h) {
  std::vector<RefEntry> out;
  git_reference_iterator* it = nullptr;
  check(git_reference_iterator_new(&it, h.repo()));
  try {
    git_reference* raw = nullptr;
    while (git_reference_next(&raw, it) == 0) {
      Reference ref;
      *ref.out() = raw;
      Reference resolved;
      if (git_reference_resolve(resolved.out(), ref.get()) < 0) {
        git_error_clear();
        continue;
      }
      const git_oid* target = git_reference_target(resolved.get());
      if (!target) continue;
      RefEntry e;
      e.name = git_reference_name(ref.get());
      e.oid = *target;
      Object obj;
      if (git_object_lookup(obj.out(), h.repo(), target, GIT_OBJECT_ANY) == 0 &&
          git_object_type(obj.get()) == GIT_OBJECT_TAG) {
        Object peeled;
        if (git_object_peel(peeled.out(), obj.get(), GIT_OBJECT_ANY) == 0) {
          e.has_peeled = true;
          e.peeled = *git_object_id(peeled.get());
        }
      }
      git_error_clear();
      out.push_back(std::move(e));
    }
  } catch (...) {
    git_reference_iterator_free(it);
    throw;
  }
  git_reference_iterator_free(it);
  return out;
}

bool head_symref(Handle& h, std::string& target, git_oid& oid) {
  Reference head;
  if (git_reference_lookup(head.out(), h.repo(), "HEAD") < 0) {
    git_error_clear();
    return false;
  }
  if (git_reference_type(head.get()) != GIT_REFERENCE_SYMBOLIC) return false;
  target = git_reference_symbolic_target(head.get());
  if (git_reference_name_to_id(&oid, h.repo(), "HEAD") < 0) {
    git_error_clear();
    return false;
  }
  return true;
}

struct Sink {
  std::string* out;
  bool sideband;
};

int sink_write(void* buf, size_t size, void* payload) {
  Sink* sink = static_cast<Sink*>(payload);
  const char* p = static_cast<const char*>(buf);
  if (!sink->sideband) {
    sink->out->append(p, size);
    return 0;
  }
  while (size > 0) {
    size_t n = std::min(size, kChunk);
    std::string chunk(1, '\x01');
    chunk.append(p, n);
    sink->out->append(pkt(chunk));
    p += n;
    size -= n;
  }
  return 0;
}

void add_target(Handle& h, Packbuilder& pb, Revwalk& walk, const git_oid& oid, bool& pushed) {
  Object obj;
  check(git_object_lookup(obj.out(), h.repo(), &oid, GIT_OBJECT_ANY));
  switch (git_object_type(obj.get())) {
    case GIT_OBJECT_COMMIT:
      check(git_revwalk_push(walk.get(), &oid));
      pushed = true;
      break;
    case GIT_OBJECT_TAG: {
      check(git_packbuilder_insert(pb.get(), &oid, nullptr));
      Object target;
      check(git_object_peel(target.out(), obj.get(), GIT_OBJECT_ANY));
      add_target(h, pb, walk, *git_object_id(target.get()), pushed);
      break;
    }
    default:
      check(git_packbuilder_insert_recur(pb.get(), &oid, nullptr));
      break;
  }
}

std::string build_pack(Handle& h, const std::vector<git_oid>& wants,
                       const std::vector<git_oid>& common, bool sideband) {
  Packbuilder pb;
  check(git_packbuilder_new(pb.out(), h.repo()));
  git_packbuilder_set_threads(pb.get(), 1);
  Revwalk walk;
  check(git_revwalk_new(walk.out(), h.repo()));
  bool pushed = false;
  for (const git_oid& w : wants) add_target(h, pb, walk, w, pushed);
  for (const git_oid& c : common) {
    Object obj;
    if (git_object_lookup(obj.out(), h.repo(), &c, GIT_OBJECT_COMMIT) == 0) {
      git_revwalk_hide(walk.get(), &c);
    } else {
      git_error_clear();
    }
  }
  if (pushed) check(git_packbuilder_insert_walk(pb.get(), walk.get()));
  std::string out;
  Sink sink{&out, sideband};
  check(git_packbuilder_foreach(pb.get(), sink_write, &sink));
  if (sideband) out += "0000";
  return out;
}

struct Command {
  git_oid old_id;
  git_oid new_id;
  std::string ref;
  std::string error;
};

bool branch_exists(Handle& h, const std::string& name) {
  Reference ref;
  int rc = git_reference_lookup(ref.out(), h.repo(), name.c_str());
  if (rc < 0) git_error_clear();
  return rc == 0;
}

std::string last_error(const char* fallback) {
  const git_error* err = git_error_last();
  std::string msg = (err && err->message) ? err->message : fallback;
  return one_line(msg);
}

std::string apply(Handle& h, const Command& c) {
  if (c.ref.rfind("refs/", 0) != 0) return "funny refname";
  int valid = 0;
  git_reference_name_is_valid(&valid, c.ref.c_str());
  if (!valid) return "invalid ref name";

  if (is_zero(c.new_id)) {
    Reference ref;
    int rc = git_reference_lookup(ref.out(), h.repo(), c.ref.c_str());
    if (rc == GIT_ENOTFOUND) {
      git_error_clear();
      return "ref does not exist";
    }
    if (rc < 0) return last_error("lookup failed");
    if (!is_zero(c.old_id)) {
      const git_oid* cur = git_reference_target(ref.get());
      if (!cur || !git_oid_equal(cur, &c.old_id)) return "stale info";
    }
    Reference head;
    if (git_reference_lookup(head.out(), h.repo(), "HEAD") == 0 &&
        git_reference_type(head.get()) == GIT_REFERENCE_SYMBOLIC &&
        c.ref == git_reference_symbolic_target(head.get())) {
      return "deletion of the current branch prohibited";
    }
    git_error_clear();
    if (git_reference_delete(ref.get()) < 0) return last_error("delete failed");
    return "";
  }

  if (!git_odb_exists(h.odb(), &c.new_id)) return "missing necessary objects";
  if (c.ref.rfind("refs/heads/", 0) == 0) {
    size_t len = 0;
    git_object_t type = GIT_OBJECT_INVALID;
    if (git_odb_read_header(&len, &type, h.odb(), &c.new_id) < 0 || type != GIT_OBJECT_COMMIT) {
      git_error_clear();
      return "branch must point to a commit";
    }
  }
  Reference out;
  int rc;
  if (is_zero(c.old_id)) {
    rc = git_reference_create(out.out(), h.repo(), c.ref.c_str(), &c.new_id, 0, nullptr);
  } else {
    rc = git_reference_create_matching(out.out(), h.repo(), c.ref.c_str(), &c.new_id, 1,
                                       &c.old_id, nullptr);
  }
  if (rc == GIT_EEXISTS || rc == GIT_EMODIFIED) {
    git_error_clear();
    return "stale info";
  }
  if (rc < 0) return last_error("update failed");
  return "";
}

void unpack(Handle& h, const char* data, size_t len) {
  if (len >= 12 && std::memcmp(data, "PACK", 4) == 0) {
    uint32_t count = (static_cast<uint32_t>(static_cast<unsigned char>(data[8])) << 24) |
                     (static_cast<uint32_t>(static_cast<unsigned char>(data[9])) << 16) |
                     (static_cast<uint32_t>(static_cast<unsigned char>(data[10])) << 8) |
                     static_cast<uint32_t>(static_cast<unsigned char>(data[11]));
    if (count == 0) return;
  }
  git_odb_writepack* wp = nullptr;
  check(git_odb_write_pack(&wp, h.odb(), nullptr, nullptr));
  git_indexer_progress stats;
  std::memset(&stats, 0, sizeof(stats));
  int rc = wp->append(wp, data, len, &stats);
  if (rc >= 0) rc = wp->commit(wp, &stats);
  std::string msg = rc < 0 ? last_error("invalid pack") : "";
  wp->free(wp);
  if (rc < 0) throw Error(msg);
}

}  // namespace

void touch(Handle& h) {
  Stmt s(h.db(), "UPDATE repos SET updated=? WHERE id=?");
  s.bind_int(1, now()).bind_int(2, h.id());
  s.run();
}

void repair_head(Handle& h, const std::string& preferred) {
  Reference head;
  if (git_reference_lookup(head.out(), h.repo(), "HEAD") < 0) {
    git_error_clear();
    return;
  }
  if (git_reference_type(head.get()) == GIT_REFERENCE_SYMBOLIC &&
      branch_exists(h, git_reference_symbolic_target(head.get()))) {
    return;
  }
  std::vector<std::string> candidates;
  if (!preferred.empty()) candidates.push_back(preferred);
  candidates.push_back("refs/heads/main");
  candidates.push_back("refs/heads/master");
  for (const RefEntry& e : collect(h)) {
    if (e.name.rfind("refs/heads/", 0) == 0) candidates.push_back(e.name);
  }
  for (const std::string& name : candidates) {
    if (!branch_exists(h, name)) continue;
    Reference out;
    check(git_reference_symbolic_create(out.out(), h.repo(), "HEAD", name.c_str(), 1, nullptr));
    return;
  }
}

std::string advertise(Handle& h, const std::string& service) {
  bool upload = service == "upload-pack";
  if (!upload && service != "receive-pack") throw Invalid("unknown service");
  std::string out = pkt("# service=git-" + service + "\n") + "0000";

  std::vector<RefEntry> refs = collect(h);
  std::vector<std::string> lines;
  std::string caps;
  if (upload) {
    caps = "multi_ack_detailed side-band-64k ofs-delta object-format=sha1";
    std::string target;
    git_oid head_oid;
    if (head_symref(h, target, head_oid)) {
      lines.push_back(to_hex(head_oid) + " HEAD");
      caps += " symref=HEAD:" + target;
    }
  } else {
    caps = "report-status delete-refs ofs-delta object-format=sha1";
  }
  caps += std::string(" ") + kAgent;
  for (const RefEntry& e : refs) {
    lines.push_back(to_hex(e.oid) + " " + e.name);
    if (e.has_peeled) lines.push_back(to_hex(e.peeled) + " " + e.name + "^{}");
  }
  if (lines.empty()) lines.push_back(std::string(kOidHex, '0') + " capabilities^{}");
  for (size_t i = 0; i < lines.size(); ++i) {
    std::string line = lines[i];
    if (i == 0) {
      line.push_back('\0');
      line += caps;
    }
    line.push_back('\n');
    out += pkt(line);
  }
  out += "0000";
  return out;
}

std::string upload_pack(Handle& h, const std::string& request) {
  PktReader rd(request);
  std::vector<git_oid> wants;
  std::string caps;
  std::string line;
  for (;;) {
    PktReader::Kind kind = rd.next(line);
    if (kind == PktReader::Flush || kind == PktReader::End) break;
    if (kind != PktReader::Data) continue;
    if (line.rfind("want ", 0) == 0) {
      std::string rest = line.substr(5);
      if (rest.size() < kOidHex) throw Invalid("malformed want line");
      if (wants.empty() && rest.size() > kOidHex) caps = rest.substr(kOidHex);
      wants.push_back(from_hex(rest.substr(0, kOidHex)));
    } else if (line.rfind("shallow ", 0) == 0 || line.rfind("deepen", 0) == 0 ||
               line.rfind("filter ", 0) == 0) {
      return pkt("ERR shallow and partial requests are not supported\n");
    }
  }
  if (wants.empty()) return "";
  for (const git_oid& w : wants) {
    if (!git_odb_exists(h.odb(), &w)) {
      return pkt("ERR upload-pack: not our ref " + to_hex(w) + "\n");
    }
  }

  bool multi = caps.find("multi_ack_detailed") != std::string::npos;
  bool sideband = caps.find("side-band-64k") != std::string::npos;
  std::vector<git_oid> common;
  std::string out;
  for (;;) {
    PktReader::Kind kind = rd.next(line);
    if (kind == PktReader::End) break;
    if (kind == PktReader::Flush) {
      out += pkt("NAK\n");
      return out;
    }
    if (kind != PktReader::Data) continue;
    if (line.rfind("have ", 0) == 0) {
      if (line.size() < 5 + kOidHex) throw Invalid("malformed have line");
      git_oid oid = from_hex(line.substr(5, kOidHex));
      if (git_odb_exists(h.odb(), &oid)) {
        common.push_back(oid);
        if (multi) out += pkt("ACK " + to_hex(oid) + " common\n");
        else if (common.size() == 1) out += pkt("ACK " + to_hex(oid) + "\n");
      }
    } else if (line == "done") {
      break;
    }
  }
  if (!common.empty()) {
    if (multi) out += pkt("ACK " + to_hex(common.back()) + "\n");
  } else {
    out += pkt("NAK\n");
  }
  out += build_pack(h, wants, common, sideband);
  return out;
}

std::string receive_pack(Handle& h, const std::string& request) {
  PktReader rd(request);
  std::vector<Command> cmds;
  std::string line;
  bool first = true;
  for (;;) {
    PktReader::Kind kind = rd.next(line);
    if (kind != PktReader::Data) break;
    std::string text = line;
    size_t nul = text.find('\0');
    if (nul != std::string::npos) text.resize(nul);
    first = false;
    if (text.rfind("shallow ", 0) == 0) continue;
    if (text.size() < 2 * kOidHex + 3) throw Invalid("malformed command");
    Command c;
    c.old_id = from_hex(text.substr(0, kOidHex));
    c.new_id = from_hex(text.substr(kOidHex + 1, kOidHex));
    c.ref = text.substr(2 * kOidHex + 2);
    cmds.push_back(std::move(c));
  }
  (void)first;
  if (cmds.empty()) return "";

  bool needs_pack = false;
  for (const Command& c : cmds) {
    if (!is_zero(c.new_id)) needs_pack = true;
  }
  std::string unpack_status = "ok";
  size_t offset = rd.pos();
  if (needs_pack && offset < request.size()) {
    try {
      unpack(h, request.data() + offset, request.size() - offset);
    } catch (const std::exception& e) {
      unpack_status = one_line(e.what());
    }
  }

  std::string first_branch;
  for (Command& c : cmds) {
    if (unpack_status != "ok") {
      c.error = "unpacker error";
      continue;
    }
    try {
      c.error = apply(h, c);
    } catch (const std::exception& e) {
      c.error = one_line(e.what());
    }
    if (c.error.empty() && first_branch.empty() && !is_zero(c.new_id) &&
        c.ref.rfind("refs/heads/", 0) == 0) {
      first_branch = c.ref;
    }
  }
  try {
    repair_head(h, first_branch);
    touch(h);
  } catch (const std::exception&) {
  }

  std::string out = pkt("unpack " + unpack_status + "\n");
  for (const Command& c : cmds) {
    if (c.error.empty()) out += pkt("ok " + c.ref + "\n");
    else out += pkt("ng " + c.ref + " " + c.error + "\n");
  }
  out += "0000";
  return out;
}
