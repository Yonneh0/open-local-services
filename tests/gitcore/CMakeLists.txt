# tests/gitcore/CMakeLists.txt — the test target of this folder.
#
# Two roads reach the same target, and they are the same build: the folder
# stands alone (cmake -S this folder, a scratch build of the harness and the
# cases it holds), and the server build adds it with one add_subdirectory line.
# Nothing here reaches the server, its engines, or their libraries — a case
# reads the golden files of this folder and states what git.exe said — so this
# target asks nothing of them and nothing of it reaches the binary.

cmake_minimum_required(VERSION 3.14)

add_executable(ols-git-tests main.cpp crash_guard.cpp ../../src/gitcore/refs.cpp ../../src/gitcore/diff.cpp ../../src/gitcore/objects.cpp ../../src/gitcore/packfile.cpp ../../src/gitcore/zlibwrap.cpp ../../src/gitcore/receive.cpp ../../src/gitcore/fetch_wire.cpp ../../src/gitcore/fetch.cpp ../../src/gitcore/merge.cpp ../../src/ProjectPulls.cpp ../../src/ProjectIssues.cpp)

# The code under test is named here — refs.cpp, the B3 refs unit, diff.cpp,
# the B4 line diff, and receive.cpp, the receive-pack answer — while the cases
# are not: main.cpp pulls each one in, and
# that is what the compiler reads as a source dependency, so a changed case
# rebuilds this target and CMakeLists.txt never has to know what cases stand.
# receive.cpp is named because none of the cases reads it in: its definitions
# stand external, so the link asks for them once, from this object.

set_property(TARGET ols-git-tests PROPERTY CXX_STANDARD 17)

# Where the gitcore headers stand, so a case can say #include "gitcore/refs.h".
target_include_directories(ols-git-tests PRIVATE ../../src)

# ProjectPulls.cpp reaches nlohmann/json.hpp through Common.h, and that header
# lives in vendor beside the other vendored libraries.
target_include_directories(ols-git-tests PRIVATE ../../vendor)

# The same warning level the server's own sources are compiled at, with the
# level the generator puts in taken out first so the compiler does not report
# an override of it for every file.
if(MSVC)
  string(REGEX REPLACE "/W[0-9]" "" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
  target_compile_options(ols-git-tests PRIVATE /utf-8 /MP /W4)
else()
  target_compile_options(ols-git-tests PRIVATE -Wall)
endif()

# Where the fixtures of this folder stand, stated to the cases so that one of
# them can open a golden file by its own name with no build root in sight. The
# path is carried with forward slashes, as CMake holds it: a backslash in a
# string literal would be an escape in the source. A path handed to the program
# still outranks this.
target_compile_definitions(ols-git-tests PRIVATE
  OLS_TEST_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures")

