Contributing
============

Thanks for your interest. This document explains how the code is organised, how
to build and check a change, and what a good change looks like.

Getting set up
--------------

Install the requirements listed in ``README.rst`` and build the engine::

    cmake -S src/core -B build
    cmake --build build

Run the site with a throwaway data directory so you never touch real
repositories::

    python3 app.py --port 5055 --data /tmp/forge-data --debug

After changing C++ code, rebuild and restart the server. Changes to templates,
CSS and JavaScript only need a page reload, and Python changes are picked up by
the ``--debug`` reloader.

Where things live
-----------------

``src/core``
    The C++17 engine, built as the ``engine`` Python module.

    * ``db`` is a thin RAII wrapper over SQLite (connection, statement,
      transaction).
    * ``odb`` is the libgit2 object database backend. It stores objects in the
      ``objects`` table and implements ``writepack`` so fetch and push can feed
      packs into SQLite.
    * ``refdb`` is the libgit2 reference backend over the ``refs`` table.
    * ``handle`` ties both backends into one ``git_repository``.
    * ``store`` holds the repository table and everything the web layer reads:
      trees, blobs, logs, commits and diffs.
    * ``protocol`` implements the smart HTTP protocol: ref advertisement,
      upload-pack and receive-pack.
    * ``bindings`` is the pybind11 glue.

``src/server``
    The Flask application. ``pages`` renders the site, ``smart`` serves the git
    protocol, ``imports`` runs background imports, ``factory`` builds the app
    and maps engine errors to HTTP responses.

``templates``, ``static``
    Jinja templates, plain CSS and plain JavaScript. There is no build step and
    no external script, font or stylesheet. Please keep it that way, the site
    must work without network access.

Code style
----------

* C++17, no compiler extensions.
* No comments in code. Names and structure should explain themselves, and
  anything that does not fit in a name belongs in this document or in
  ``README.rst``.
* Names do not repeat the project name. Write ``open()`` and ``store.cpp``, not
  a prefixed variant.
* No exceptions may cross into libgit2. Every callback that libgit2 calls wraps
  its body in a guard that turns exceptions into an error code and a libgit2
  error message.
* Own libgit2 and SQLite resources with the RAII types in ``common.hpp`` and
  ``db.hpp`` rather than freeing by hand.
* Text that reaches Python must be valid UTF-8. Run author names, messages,
  paths and patches through ``clean_utf8`` and return file contents as bytes.
* Python follows PEP 8 with a line length of up to 120 characters.
* In templates, keep markup semantic, keep text readable in both light and dark
  mode, and make every interactive element reachable by keyboard.

Compatibility
-------------

The engine targets libgit2 1.7 and newer. The 1.8 and 1.9 releases moved and
renamed a few functions, so new libgit2 calls should be checked against the
headers of more than one release. Use ``__has_include`` or feature macros
instead of version checks where possible.

Only the SHA-1 object format is supported. Do not add code that assumes another
hash size.

Checking a change
-----------------

There is no automated test suite yet, so changes are checked by hand against a
real ``git`` client. A good change comes with the commands you ran. At minimum,
for anything touching the engine or the protocol, run the following against a
fresh data directory::

    git clone http://127.0.0.1:5055/demo.git
    cd demo
    echo hello > a.txt
    git add . && git commit -m first
    git push -u origin main
    git clone http://127.0.0.1:5055/demo.git ../copy
    git -C ../copy fsck

Then also try the cases your change could affect: a second push that updates
existing files, a new branch, a tag, deleting a branch, a force push, and an
import from a local path.

Adding a test suite is a welcome contribution. A script that starts the server
on a free port and drives ``git`` through the scenarios above would be a good
first step.

Proposing a change
------------------

* Open an issue or describe the idea first for anything larger than a small fix.
* Keep each change focused on one thing, with a short imperative commit
  subject such as ``Reject deleting the default branch``.
* Explain why the change is needed, not only what it does.
* Mention which libgit2 version you built and tested against.
* Do not commit build output, database files or the ``data`` directory. The
  ``.gitignore`` already covers them.

Reporting problems
------------------

Include the output of ``git --version``, your libgit2 version, your operating
system, the exact commands you ran and the server log. For build failures,
include the full compiler output, because the first error is often not the root
cause.

Security issues, such as a way to read or write outside a repository, should be
reported privately to the maintainer rather than in a public issue.

License
-------

By contributing you agree that your work is released under the same license as
the project.
