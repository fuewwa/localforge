#pragma once

#include <git2.h>

#if defined(__has_include)
#if __has_include(<git2/sys/errors.h>)
#include <git2/sys/errors.h>
#endif
#endif

#include <cstdint>
#include <stdexcept>
#include <string>

#ifdef GIT_OID_SHA1_SIZE
constexpr size_t kOidSize = GIT_OID_SHA1_SIZE;
constexpr size_t kOidHex = GIT_OID_SHA1_HEXSIZE;
#else
constexpr size_t kOidSize = GIT_OID_RAWSZ;
constexpr size_t kOidHex = GIT_OID_HEXSZ;
#endif

struct Error : std::runtime_error {
  using std::runtime_error::runtime_error;
};
struct NotFound : Error {
  using Error::Error;
};
struct Exists : Error {
  using Error::Error;
};
struct Invalid : Error {
  using Error::Error;
};

void check(int rc);

template <class T, void (*Free)(T*)>
class Owned {
 public:
  Owned() = default;
  ~Owned() { reset(); }
  Owned(const Owned&) = delete;
  Owned& operator=(const Owned&) = delete;
  T* get() const { return ptr_; }
  T** out() {
    reset();
    return &ptr_;
  }
  explicit operator bool() const { return ptr_ != nullptr; }

 private:
  void reset() {
    if (ptr_) {
      Free(ptr_);
      ptr_ = nullptr;
    }
  }
  T* ptr_ = nullptr;
};

using Commit = Owned<git_commit, git_commit_free>;
using Tree = Owned<git_tree, git_tree_free>;
using Blob = Owned<git_blob, git_blob_free>;
using Object = Owned<git_object, git_object_free>;
using Revwalk = Owned<git_revwalk, git_revwalk_free>;
using Diff = Owned<git_diff, git_diff_free>;
using Patch = Owned<git_patch, git_patch_free>;
using Reference = Owned<git_reference, git_reference_free>;
using Packbuilder = Owned<git_packbuilder, git_packbuilder_free>;
using Remote = Owned<git_remote, git_remote_free>;
using Odb = Owned<git_odb, git_odb_free>;
using OdbObject = Owned<git_odb_object, git_odb_object_free>;
using TreeEntry = Owned<git_tree_entry, git_tree_entry_free>;

std::string to_hex(const git_oid& oid);
git_oid from_hex(const std::string& hex);
bool is_hex(const std::string& s);
bool is_zero(const git_oid& oid);
std::string clean_utf8(const std::string& s);
bool valid_name(const std::string& name);
int64_t now();
