#pragma once

#include <git2.h>
#include <git2/sys/refdb_backend.h>

#include <cstdint>

#include "db.hpp"

git_refdb_backend* make_refdb_backend(Db* db, int64_t repo);
