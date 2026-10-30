#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include "common.hpp"
#include "store.hpp"

namespace py = pybind11;

namespace {

struct Runtime {
  Runtime() { git_libgit2_init(); }
  ~Runtime() { git_libgit2_shutdown(); }
};

py::bytes run_bytes(Store& s, std::string (Store::*fn)(const std::string&, const std::string&),
                    const std::string& name, const std::string& arg) {
  std::string out;
  {
    py::gil_scoped_release release;
    out = (s.*fn)(name, arg);
  }
  return py::bytes(out);
}

}  // namespace

PYBIND11_MODULE(engine, m) {
  static Runtime runtime;
  const py::call_guard<py::gil_scoped_release> nogil;

  py::register_exception<Error>(m, "Error");
  py::object base = m.attr("Error");
  py::register_exception<NotFound>(m, "NotFound", base);
  py::register_exception<Exists>(m, "Exists", base);
  py::register_exception<Invalid>(m, "Invalid", base);

  py::class_<RepoInfo>(m, "RepoInfo")
      .def_readonly("name", &RepoInfo::name)
      .def_readonly("description", &RepoInfo::description)
      .def_readonly("status", &RepoInfo::status)
      .def_readonly("error", &RepoInfo::error)
      .def_readonly("branch", &RepoInfo::branch)
      .def_readonly("created", &RepoInfo::created)
      .def_readonly("updated", &RepoInfo::updated)
      .def_readonly("empty", &RepoInfo::empty);

  py::class_<Entry>(m, "Entry")
      .def_readonly("name", &Entry::name)
      .def_readonly("kind", &Entry::kind)
      .def_readonly("size", &Entry::size);

  py::class_<CommitInfo>(m, "CommitInfo")
      .def_readonly("id", &CommitInfo::id)
      .def_readonly("summary", &CommitInfo::summary)
      .def_readonly("message", &CommitInfo::message)
      .def_readonly("author", &CommitInfo::author)
      .def_readonly("email", &CommitInfo::email)
      .def_readonly("time", &CommitInfo::time)
      .def_readonly("offset", &CommitInfo::offset)
      .def_readonly("parents", &CommitInfo::parents);

  py::class_<FileChange>(m, "FileChange")
      .def_readonly("path", &FileChange::path)
      .def_readonly("old_path", &FileChange::old_path)
      .def_readonly("status", &FileChange::status)
      .def_readonly("additions", &FileChange::additions)
      .def_readonly("deletions", &FileChange::deletions)
      .def_readonly("binary", &FileChange::binary);

  py::class_<CommitDetail>(m, "CommitDetail")
      .def_readonly("info", &CommitDetail::info)
      .def_readonly("files", &CommitDetail::files)
      .def_readonly("patch", &CommitDetail::patch)
      .def_readonly("truncated", &CommitDetail::truncated);

  py::class_<FileData>(m, "FileData")
      .def_readonly("size", &FileData::size)
      .def_readonly("binary", &FileData::binary)
      .def_readonly("truncated", &FileData::truncated)
      .def_property_readonly("data", [](const FileData& f) { return py::bytes(f.data); });

  py::class_<RefList>(m, "RefList")
      .def_readonly("branches", &RefList::branches)
      .def_readonly("tags", &RefList::tags);

  py::class_<Store>(m, "Store")
      .def(py::init<const std::string&>(), nogil)
      .def("create", &Store::create, nogil, py::arg("name"), py::arg("description") = "",
           py::arg("branch") = "main", py::arg("status") = "ready")
      .def("remove", &Store::remove, nogil)
      .def("set_description", &Store::set_description, nogil)
      .def("set_status", &Store::set_status, nogil, py::arg("name"), py::arg("status"),
           py::arg("error") = "")
      .def("set_branch", &Store::set_branch, nogil)
      .def("exists", &Store::exists, nogil)
      .def("info", &Store::info, nogil)
      .def("list", &Store::list, nogil)
      .def("refs", &Store::refs, nogil)
      .def("kind", &Store::kind, nogil)
      .def("tree", &Store::tree, nogil)
      .def("blob", &Store::blob, nogil, py::arg("name"), py::arg("ref"), py::arg("path"),
           py::arg("limit") = -1)
      .def("log", &Store::log, nogil)
      .def("commit", &Store::commit, nogil, py::arg("name"), py::arg("id"),
           py::arg("max_patch") = -1)
      .def("fetch", &Store::fetch, nogil)
      .def("advertise",
           [](Store& s, const std::string& name, const std::string& service) {
             return run_bytes(s, &Store::advertise, name, service);
           })
      .def("upload_pack",
           [](Store& s, const std::string& name, const py::bytes& req) {
             return run_bytes(s, &Store::upload_pack, name, std::string(req));
           })
      .def("receive_pack", [](Store& s, const std::string& name, const py::bytes& req) {
        return run_bytes(s, &Store::receive_pack, name, std::string(req));
      });
}
