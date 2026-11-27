cmake_minimum_required(VERSION 3.14)
project(open-local-services LANGUAGES CXX)

set(ROOT ${CMAKE_CURRENT_SOURCE_DIR}/..)
set(SD_VENDOR ${ROOT}/vendor/stable-diffusion.cpp)

# The image engine is not linked into this binary. The engine and its ggml
# stand in a library beside the server - stable-diffusion-cuda, -vulkan, or
# -cpu - opened at start by EngineLoader.cpp; this tree reads the engine's
# header alone. The engine libraries are built by the same entry-point
# scripts that build this server, against the engine's own cmake in its own
# build roots, and put where the server can open them - which is why one CUDA
# engine library carries every card generation its build was pointed at: the
# loader inside the library picks the card's own kernels when it is opened.
# The scripts' shape words - OLS_SD_BUILD, OLS_CUDA, OLS_NO_DEVICE - name the
# engine libraries the scripts build beside this server, not this binary:
# they are read here so a hand-configured build hears, in words, that these
# days they change nothing about the server itself.
foreach(ols_named OLS_SD_BUILD OLS_CUDA OLS_NO_DEVICE)
  if(DEFINED ${ols_named})
    message(STATUS "${ols_named} is named: the server links no engine and no backend - the word stands for the engine libraries built beside it")
  endif()
endforeach()

# The TLS library the wire speaks through, carried inside this binary like the
# engines above it. It is a static copy - no dll is shipped - and it keeps its
# own build and its own flags: its fatal-warnings switch, which the upstream
# default puts on, is asked off here so a newer toolchain's opinion of vendored
# code cannot stop this build, and its tests and sample programs are named off
# because nothing in this tree reads them.
set(USE_STATIC_MBEDTLS_LIBRARY ON CACHE BOOL "the TLS library is carried inside this binary")
set(USE_SHARED_MBEDTLS_LIBRARY OFF CACHE BOOL "no TLS dll is shipped")
set(ENABLE_TESTING OFF CACHE BOOL "the TLS library's own tests are not part of this build")
set(ENABLE_PROGRAMS OFF CACHE BOOL "the TLS library's sample programs are not part of this build")
set(MBEDTLS_FATAL_WARNINGS OFF CACHE BOOL "the vendored library keeps its own flags, like the engines")
add_subdirectory("${ROOT}/vendor/mbedtls" "${CMAKE_BINARY_DIR}/mbedtls-tls")

# The pages carried inside the binary. The webui folder of the tree this build
# stands in is read whole into webui-embedded.cpp, and that file is compiled
# into the server: a released .exe carries its own pages, so it answers with
# them when no webui folder sits beside it, and the expand door writes them
# out to start serving the outside copies. Nothing is generated at run time;
# the build watches the folder instead - every page stands as an input of the
# generated file - so the build is the only place these bytes are read, and
# the build always sees their newest bytes.
# The folder is read by a script of its own, and the file it writes is declared
# an output with every page named among its inputs: any builder then re-reads
# the folder whenever a page changed - on every generator, including the
# Visual Studio one, which is not a builder that re-checks globbed lists at
# build time. A page added rather than edited still needs the configure asked
# again (CONFIGURE_DEPENDS on the glob below does that where the builder
# understands it) - and a build that re-reads the same bytes recompiles
# nothing, because the script stamps by content, not by having run.
function(ols_write_webui_embedded out_file)
  file(REAL_PATH "${ROOT}/webui" web_root)
  if(NOT IS_DIRECTORY "${web_root}")
    message(FATAL_ERROR "the pages must be in this tree to be carried inside the binary: ${web_root}")
  endif()
  file(GLOB_RECURSE web_items CONFIGURE_DEPENDS "${web_root}/*")
  if(NOT web_items)
    message(FATAL_ERROR "the pages folder holds nothing to carry inside the binary")
  endif()
  set(page_files "")
  foreach(item ${web_items})
    if(NOT IS_DIRECTORY "${item}")
      list(APPEND page_files "${item}")
    endif()
  endforeach()
  list(SORT page_files)
  set(web_script "${CMAKE_CURRENT_LIST_DIR}/cmake/write-webui-embedded.cmake")
  execute_process(
    COMMAND ${CMAKE_COMMAND} "-Dweb_root=${web_root}" "-Dout=${out_file}" -P "${web_script}"
    RESULT_VARIABLE web_stamp_result
    OUTPUT_VARIABLE web_stamp_line
    OUTPUT_STRIP_TRAILING_WHITESPACE)
  if(NOT web_stamp_result EQUAL 0)
    message(FATAL_ERROR "the pages could not be read into the binary: ${web_stamp_line}")
  endif()
  message(STATUS "${web_stamp_line}")
  add_custom_command(
    OUTPUT "${out_file}"
    COMMAND ${CMAKE_COMMAND} "-Dweb_root=${web_root}" "-Dout=${out_file}" -P "${web_script}"
    DEPENDS ${page_files} "${web_script}"
    COMMENT "reading the pages the binary carries"
    VERBATIM)
endfunction()

set(WEBUI_EMBEDDED_CPP "${CMAKE_BINARY_DIR}/webui-embedded.cpp")
ols_write_webui_embedded("${WEBUI_EMBEDDED_CPP}")

# The version this code states is carried in a file of its own, written when its
# bytes differ from the ones already standing there: a -D on the command line
# does not make an object out of date, so a binary built after a commit that
# changed no source would otherwise keep printing the commit an older object was
# compiled from, and a release version is meant to be a commit. This file is
# compiled by every build of the server, so what the binary, its start-up line
# and its pages print is the commit this build named.
if(NOT DEFINED OLS_VERSION)
  set(OLS_VERSION "")
  message(STATUS "code version: not named by this build, so the binary states none")
else()
  message(STATUS "code version: this binary carries ${OLS_VERSION}")
endif()
set(VERSION_STAMP_CPP "${CMAKE_BINARY_DIR}/version-stamped.cpp")
set(VERSION_STAMP_STANDING "${CMAKE_BINARY_DIR}/version-stamped.standing.cpp")
file(WRITE "${VERSION_STAMP_STANDING}"
  "// written by the src build when it configured - do not edit\n"
  "#include <string>\n"
  "namespace ols { namespace platform {\n"
  "std::string buildVersion()\n{\n"
  "  return \"${OLS_VERSION}\";\n"
  "}\n"
  "} }\n")
file(SHA256 "${VERSION_STAMP_STANDING}" version_stamp_sum)
set(version_stamp_standing_sum "")
if(EXISTS "${VERSION_STAMP_CPP}")
  file(SHA256 "${VERSION_STAMP_CPP}" version_stamp_standing_sum)
endif()
if(NOT version_stamp_sum STREQUAL version_stamp_standing_sum)
  file(RENAME "${VERSION_STAMP_STANDING}" "${VERSION_STAMP_CPP}")
else()
  file(REMOVE "${VERSION_STAMP_STANDING}")
endif()

# The build's moment rides the same single-file road as the version, with one
# rule more: it carries no stamp older than the build that linked it. The
# compiler's own dates cannot say this: __DATE__ and __TIME__ are words each
# object speaks for itself, and an incremental build re-links objects it never
# touched, letting an old object date a newer binary. Nor does an output-less
# step do it: ninja then tracks the .cpp by mtime alone at plan time, before
# the step has run, and prunes the compile that should follow it - measured
# here once stamping a binary two hours its own link-time old. The shape that
# holds: an always-run target touches a trigger file, the source carries a
# producing edge behind that trigger, and the write script moves the file's
# mtime to now whenever the second has moved on - so ninja re-plans, sees the
# fresher .cpp, recompiles the stamp TU, and re-links behind it for true. A
# build landing in the same second as its predecessor says the same words and
# costs nothing more. Three shapes were measured before this one held: a
# producing edge with no inputs runs only when its output is absent, so alone
# it never restamps; an OUTPUT spelled with backslashes while the target
# carries it forward-slashed leaves the edge orphaned, every build answering
# "no work to do"; and a trigger that first appears at build time fails the
# first build outright, ninja wanting an edge for a file nothing makes yet -
# so the configure step touches the trigger into being.
set(BUILD_MOMENT_CPP "${CMAKE_BINARY_DIR}/build-moment.cpp")
file(TOUCH "${BUILD_MOMENT_CPP}.trigger")
add_custom_target(build-moment ALL
  COMMAND ${CMAKE_COMMAND} -E touch "${BUILD_MOMENT_CPP}.trigger")
add_custom_command(OUTPUT "${BUILD_MOMENT_CPP}"
  COMMAND ${CMAKE_COMMAND} "-Dout=${BUILD_MOMENT_CPP}"
          -P "${CMAKE_CURRENT_LIST_DIR}/cmake/write-build-moment.cmake"
  DEPENDS "${BUILD_MOMENT_CPP}.trigger")


add_executable(open-local-services
  main.cpp
  OpenLocalServices.cpp
  TlsLayer.cpp
  MonitorService.cpp
  PeerService.cpp
  ProjectService.cpp
  ProjectGit.cpp
  ProjectPulls.cpp
  ProjectIssues.cpp
  GitSmartHttp.cpp
  gitcore/objects.cpp
  gitcore/packfile.cpp
  gitcore/refs.cpp
  gitcore/diff.cpp
  # The wire unit is not named here: its definitions are inline, and a
  # translation unit that only defines inline functions emits no code for them,
  # so compiling wire.cpp as a source would hand the link nothing to resolve.
  # The units that ask its symbols - GitSmartHttp.cpp, OpenLocalServices.cpp,
  # gitcore/fetch.cpp and gitcore/fetch_wire.cpp - pull the unit in beside
  # their own with a unity include (gcc proved the need: inline definitions do
  # not fold across sibling objects there, and an omitted include reads back
  # an undefined reference), the harness reaches the same ground through
  # test_wire.cpp's include - which is why the harness names no wire.cpp
  # source either. receive.cpp stands in this list because no door unity-
  # includes it: its definitions are plain, one object, once.
  gitcore/receive.cpp
  # The merge engine the pull doors stand on: plain definitions, one object,
  # once - the same dress as receive.cpp above.
  gitcore/merge.cpp
  # The fetch client's ground: ProjectService.cpp's clone road asks lsRemote and
  # fetchRepo, and fetch.cpp asks the fetch_wire readers - plain sources the way
  # receive.cpp stands, so one copy crosses the link.
  gitcore/fetch.cpp
  gitcore/fetch_wire.cpp
  gitcore/zlibwrap.cpp
  # The ssh road the clone doors read an address off (E2): plain definitions,
  # one object, once - the same dress as receive.cpp above.
  SshFetch.cpp
  ImageService.cpp
  EngineLoader.cpp
  VerifiedRoom.cpp
  Deployment.cpp
  NetworkService.cpp
  WebAssets.cpp
  HardwareInfo.cpp
  "${WEBUI_EMBEDDED_CPP}"
  "${VERSION_STAMP_CPP}"
  "${BUILD_MOMENT_CPP}")
add_dependencies(open-local-services build-moment)

set_property(TARGET open-local-services PROPERTY CXX_STANDARD 17)

# The shape this binary answers as is no longer stamped into it at compile
# time: it is read from the machine at start - the engine library that came,
# the card that answered - and spoken by /api/system through those readings
# (EngineLoader.cpp). One server binary of a platform serves every machine of
# that platform; what differs between boxes is the library standing beside it.

# The server's own sources are compiled at the strictest warning level this
# compiler has, so the compiler, and not a reader, states what in them is never
# read or can never be reached. The vendored engines keep their own build and
# their own flags: nothing here asks a warning of their code.
if(MSVC)
  # the level the build system puts in by default is not the one we want, and
  # leaving both on a line makes the compiler report an override for every file;
  # the level is taken out here and named once below.
  string(REGEX REPLACE "/W[0-9]" "" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
  target_compile_options(open-local-services PRIVATE /utf-8 /MP /W4)
  add_compile_definitions(_CRT_SECURE_NO_WARNINGS _WINSOCK_DEPRECATED_NO_WARNINGS)
else()
  add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
  target_compile_options(open-local-services PRIVATE -O2 -Wall)
endif()

target_include_directories(open-local-services PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}"
  "${ROOT}/vendor"
  "${SD_VENDOR}/include"
  "${SD_VENDOR}/thirdparty")

# The version a build names (-DOLS_VERSION: dev in the development scripts, the
# commit in the release builders) is turned into that stated version above,
# where the server's files are gathered; nothing here reads it again.

function(pick_library_if_present out base name)
  set(found "")
  # The find is anchored where the entry points stand - the project root - since
  # that is where they name their build trees from. A relative path handed to the
  # linker reads as a library name, and the linker adds its extension a second
  # time, so the path is always carried on whole.
  file(REAL_PATH "${ROOT}" projectHere)
  set(baseAbsolute "${base}")
  if(NOT IS_ABSOLUTE "${base}")
    set(baseAbsolute "${projectHere}/${base}")
  endif()
  if(MSVC)
    # On MSVC a built library lands in a Release folder under the build root, and
    # a shared target keeps its import library one level above its dll. A build
    # driven by ninja instead leaves it in the root itself, so that is tried last,
    # never over a Release folder that does hold one.
    foreach(try "${baseAbsolute}/Release/${name}.lib" "${baseAbsolute}/bin/Release/${name}.lib" "${baseAbsolute}/bin/${name}.lib" "${baseAbsolute}/bin/Release/${name}.dll" "${baseAbsolute}/Release/${name}.dll" "${baseAbsolute}/${name}.lib")
      if(NOT found AND EXISTS "${try}")
        set(found "${try}")
      endif()
    endforeach()
  else()
    foreach(dir "${baseAbsolute}" "${baseAbsolute}/bin")
      foreach(ext .so .a)
        if(EXISTS "${dir}/lib${name}${ext}")
          set(found "${dir}/lib${name}${ext}")
        endif()
      endforeach()
    endforeach()
  endif()
  set(${out} "${found}" PARENT_SCOPE)
endfunction()

function(pick_library out base name)
  # a library the link needs is named where it answers with it: a missing one
  # stops the build, plainly.
  pick_library_if_present(_plFound "${base}" "${name}")
  if(NOT _plFound)
    message(FATAL_ERROR "missing library ${name} under ${base}")
  endif()
  set(${out} "${_plFound}" PARENT_SCOPE)
endfunction()

# The engine and its ggml are not in this link at all: the server opens one
# engine library beside itself at start (EngineLoader.cpp). The engine keeps
# whatever CUDA, Vulkan, or plain-cpu code it was built with entirely inside
# its own file, together with the runtime imports its build recorded.

# The TLS library joins the link as one name: the mbedtls target carries its
# own x509 and crypto archives, and those carry the system libraries they ask
# of (ws2_32, bcrypt), in the order that works.
target_link_libraries(open-local-services PRIVATE mbedtls)

if(MSVC)
  # The share of each thread of the machine's processor is read by asking the
  # system module for its per-processor record. That module is not one the
  # development tools link by default, so the newest import library kept for
  # this architecture under the Windows development kits is named here. Where
  # none is kept, the server states nothing about its threads, and the pages
  # say so rather than drawing a flat line.
  file(GLOB systemImportLibraries
    "C:/Program Files (x86)/Windows Kits/*/Lib/*/*/x64/ntdll.lib"
    "C:/Program Files/Windows Kits/*/Lib/*/*/x64/ntdll.lib")
  if(systemImportLibraries)
    list(SORT systemImportLibraries)
    list(GET systemImportLibraries -1 newestSystemImportLibrary)
    target_link_libraries(open-local-services PRIVATE "${newestSystemImportLibrary}")
  endif()
endif()

if(NOT MSVC)
  find_package(Threads REQUIRED)
  target_link_libraries(open-local-services PRIVATE pthread)
  # setup.sh may install the binary and its shared libraries elsewhere.
  target_link_options(open-local-services PRIVATE "-Wl,-rpath,$ORIGIN")
endif()

# The git core's own tests: a small harness of its own, built beside the server.
add_subdirectory("${ROOT}/tests/gitcore" "${CMAKE_BINARY_DIR}/git-tests")
