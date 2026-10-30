localforge
==========

A git hosting service that runs on your own machine. Every repository lives
inside a single SQLite file: commits, trees, files, branches and tags. There is
no bare repository on disk and no ``git`` binary is needed on the server.

Clone, fetch and push work over HTTP with a stock ``git`` client. A small web
interface lets you browse files, read the commit log and inspect diffs.

How it works
------------

* ``src/core`` is the engine, written in C++17. It uses libgit2 with custom
  object-database and reference-database backends that store everything in
  SQLite, and implements the git smart HTTP protocol (upload-pack and
  receive-pack) on top of them. It is exposed to Python as the ``engine``
  module through pybind11.
* ``src/server`` is the Flask application: pages, JSON status endpoint and the
  git protocol endpoints.
* ``templates`` and ``static`` hold the HTML, CSS and JavaScript of the site.
* ``app.py`` starts the site.

Objects are stored zlib-compressed, one row per object. Pushes are applied
atomically per reference.

Requirements
------------

* A C++17 compiler, CMake 3.18 or newer, pkg-config
* libgit2 1.7 or newer (development files); developed against 1.7, 1.8 and 1.9 differences are handled but 1.9 is untested
* SQLite 3 and zlib (development files)
* Python 3.9 or newer, with Flask and pybind11

Arch Linux::

    sudo pacman -S base-devel cmake pkgconf libgit2 sqlite zlib python python-flask pybind11

Debian and Ubuntu (libgit2-dev must be version 1.7 or newer)::

    sudo apt install build-essential cmake pkg-config libgit2-dev libsqlite3-dev zlib1g-dev \
        python3-dev python3-flask pybind11-dev

macOS (Homebrew)::

    brew install cmake pkg-config libgit2 sqlite zlib python pybind11
    python3 -m pip install flask

Windows (MSYS2, UCRT64 shell)::

    pacman -S mingw-w64-ucrt-x86_64-toolchain mingw-w64-ucrt-x86_64-cmake \
        mingw-w64-ucrt-x86_64-pkgconf mingw-w64-ucrt-x86_64-libgit2 \
        mingw-w64-ucrt-x86_64-sqlite3 mingw-w64-ucrt-x86_64-zlib \
        mingw-w64-ucrt-x86_64-python mingw-w64-ucrt-x86_64-python-flask \
        mingw-w64-ucrt-x86_64-pybind11

If your distribution packages Flask or pybind11 as Python-only packages, a
virtual environment with ``pip install flask pybind11`` works as well.

Build
-----

From the repository root::

    cmake -S src/core -B build
    cmake --build build

The compiled ``engine`` module is placed in ``src/``.

Run
---

::

    python3 app.py

Then open http://127.0.0.1:5000.

Options::

    --host         address to listen on (default 127.0.0.1)
    --port         port to listen on (default 5000)
    --data         directory that holds the database (default ./data)
    --max-body-mb  largest accepted push or request body (default 512)
    --debug        enable the Flask debugger

The database is ``forge.db`` inside the data directory. Copy that one file to
back up or move every repository.

Usage
-----

Create a repository on the home page, then::

    git remote add origin http://127.0.0.1:5000/<name>.git
    git push -u origin main
    git clone http://127.0.0.1:5000/<name>.git

An existing repository can be imported from an http(s) or git URL, or from an
absolute path on the machine. Importing runs in the background and the page
refreshes when it is done.

Each repository page has these tabs:

* home: back to the list of repositories
* preview: browse directories and open files, with a branch and tag selector
* logs: commit history with a link to every commit and its diff

A settings link on the right edge changes the description and default branch
and deletes the repository.

Security notes
--------------

The server has no accounts. By default it listens on 127.0.0.1 only. If you
pass ``--host 0.0.0.0``, everyone who can reach the port can read, push to and
delete every repository. Put it behind a reverse proxy with authentication
before doing that.

Limitations
-----------

* Shallow and partial clones are not supported.
* Request bodies are held in memory, so very large pushes need memory to match.
* Only the SHA-1 object format is supported.
* Import over ssh is not supported.
