cmake_minimum_required(VERSION 3.18)
project(engine LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release)
endif()

find_package(Python 3 REQUIRED COMPONENTS Interpreter Development.Module)
execute_process(
  COMMAND ${Python_EXECUTABLE} -m pybind11 --cmakedir
  OUTPUT_VARIABLE pybind11_DIR
  OUTPUT_STRIP_TRAILING_WHITESPACE
  ERROR_QUIET)
find_package(pybind11 CONFIG REQUIRED)

find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBGIT2 REQUIRED IMPORTED_TARGET libgit2>=1.7)
pkg_check_modules(SQLITE3 REQUIRED IMPORTED_TARGET sqlite3)
pkg_check_modules(ZLIB REQUIRED IMPORTED_TARGET zlib)

pybind11_add_module(engine MODULE
  bindings.cpp
  common.cpp
  db.cpp
  odb.cpp
  refdb.cpp
  handle.cpp
  store.cpp
  protocol.cpp)

target_link_libraries(engine PRIVATE PkgConfig::LIBGIT2 PkgConfig::SQLITE3 PkgConfig::ZLIB)

set(OUT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/..")
set_target_properties(engine PROPERTIES
  LIBRARY_OUTPUT_DIRECTORY "$<1:${OUT_DIR}>"
  RUNTIME_OUTPUT_DIRECTORY "$<1:${OUT_DIR}>")
