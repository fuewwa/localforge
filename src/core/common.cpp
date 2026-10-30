#include "common.hpp"

#include <chrono>
#include <cstring>

void check(int rc) {
  if (rc >= 0) return;
  const git_error* err = git_error_last();
  std::string msg = clean_utf8((err && err->message) ? err->message : "git error");
  if (rc == GIT_ENOTFOUND) throw NotFound(msg);
  if (rc == GIT_EEXISTS) throw Exists(msg);
  throw Error(msg);
}

std::string to_hex(const git_oid& oid) {
  char buf[GIT_OID_MAX_HEXSIZE + 1];
  git_oid_tostr(buf, sizeof(buf), &oid);
  return buf;
}

git_oid from_hex(const std::string& hex) {
  git_oid oid;
  if (hex.size() != kOidHex || !is_hex(hex)) throw Invalid("invalid object id");
  check(git_oid_fromstr(&oid, hex.c_str()));
  return oid;
}

bool is_hex(const std::string& s) {
  if (s.empty()) return false;
  for (char c : s) {
    bool ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
    if (!ok) return false;
  }
  return true;
}

bool is_zero(const git_oid& oid) {
  for (size_t i = 0; i < kOidSize; ++i) {
    if (oid.id[i] != 0) return false;
  }
  return true;
}

std::string clean_utf8(const std::string& s) {
  std::string out;
  out.reserve(s.size());
  size_t i = 0;
  const size_t n = s.size();
  while (i < n) {
    unsigned char c = static_cast<unsigned char>(s[i]);
    size_t len = 0;
    if (c < 0x80) len = 1;
    else if (c >= 0xC2 && c <= 0xDF) len = 2;
    else if (c >= 0xE0 && c <= 0xEF) len = 3;
    else if (c >= 0xF0 && c <= 0xF4) len = 4;
    bool ok = len != 0 && i + len <= n;
    if (ok) {
      for (size_t k = 1; k < len; ++k) {
        if ((static_cast<unsigned char>(s[i + k]) & 0xC0) != 0x80) {
          ok = false;
          break;
        }
      }
    }
    if (ok && len == 3) {
      unsigned char c1 = static_cast<unsigned char>(s[i + 1]);
      if ((c == 0xE0 && c1 < 0xA0) || (c == 0xED && c1 > 0x9F)) ok = false;
    }
    if (ok && len == 4) {
      unsigned char c1 = static_cast<unsigned char>(s[i + 1]);
      if ((c == 0xF0 && c1 < 0x90) || (c == 0xF4 && c1 > 0x8F)) ok = false;
    }
    if (ok) {
      out.append(s, i, len);
      i += len;
    } else {
      out += "\xEF\xBF\xBD";
      i += 1;
    }
  }
  return out;
}

bool valid_name(const std::string& name) {
  if (name.empty() || name.size() > 64) return false;
  for (size_t i = 0; i < name.size(); ++i) {
    char c = name[i];
    bool alnum = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
    if (i == 0 && !alnum) return false;
    if (!alnum && c != '-' && c != '_') return false;
  }
  return true;
}

int64_t now() {
  return std::chrono::duration_cast<std::chrono::seconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}
