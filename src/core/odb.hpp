#pragma once

#include <git2.h>
#include <git2/sys/odb_backend.h>

#include <cstdint>

#include "db.hpp"

git_odb_backend* make_odb_backend(Db* db, int64_t repo);
