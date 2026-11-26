cmake_minimum_required(VERSION 3.14)
project(open-local-services LANGUAGES CXX)

set(ROOT ${CMAKE_CURRENT_SOURCE_DIR}/..)
set(SD_VENDOR ${ROOT}/vendor/stable-diffusion.cpp)

# The trees the image engine was built in.
# The image engine's own ggml is linked into this binary beside the image
# library: it is the machine's device probe, the same library that runs the
# pictures enumerating the GPU the pages show.
# Unless an entry point names the image tree as OLS_SD_BUILD, it is the build
# subfolder inside the engine source itself - the same place on either
# platform, never a short alias in an unrelated one.
set(SD_BUILD ${SD_VENDOR}/build)
foreach(ols_named OLS_SD_BUILD)
  if(DEFINED ${ols_named})
    string(REPLACE "OLS_" "" ols_root "${ols_named}")
    set(${ols_root} "${${ols_named}}")
  endif()
endforeach()

# The device backend follows the platform - the CUDA device on Linux, the Vulkan
# device on Windows - unless the build names OLS_CUDA, which asks for the CUDA
# device on Windows too: a machine whose card is CUDA-first is then built whole
# on it, the engines with it. The scripts that do this name the build root for
# the backend, so the two builds of one tree never share a cache. The choice
# is kept in locals: only the stated shape below reads it.
set(ols_cuda OFF)
set(ols_vulkan OFF)
if(NOT OLS_NO_DEVICE)
  if(OLS_CUDA)
    set(ols_cuda ON)
    message(STATUS "device backend for this build: CUDA, named by -DOLS_CUDA")
  elseif(MSVC)
    set(ols_vulkan ON)
  else()
    set(ols_cuda ON)
  endif()
else()
  message(STATUS "no device backend in this target: the image engine carries the cpu backend only")
endif()

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
  # The doors that ask its symbols - GitSmartHttp.cpp, OpenLocalServices.cpp,
  # and gitcore/fetch.cpp (which unity-includes it) - pull the unit in beside
  # their own, where the inline definitions land in those doors' objects and
  # fold to one across the link; the harness reaches the same ground through
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

# The shape this binary is, told to the process as it is compiled: the pages
# put these words in the browser's tab beside the machine's name, so three
# open tabs say which machine stands behind each. The device follows the same
# choice made at the top of this file - CUDA where the build named it, Vulkan
# on Windows, CUDA elsewhere, and no device at all when the shape asked for
# none. The cuda architecture is the first one the build was pointed at, which
# every script here passes as -DCMAKE_CUDA_ARCHITECTURES.
if(OLS_NO_DEVICE)
  string(TOLOWER "${CMAKE_SYSTEM_PROCESSOR}" ols_cpu_isa)
  if(ols_cpu_isa STREQUAL "amd64")
    set(ols_cpu_isa "x86_64")
  endif()
  target_compile_definitions(open-local-services PRIVATE OLS_BACKEND="cpu" OLS_CPU_ISA="${ols_cpu_isa}")
elseif(ols_cuda)
  set(ols_cuda_arch "")
  if(CMAKE_CUDA_ARCHITECTURES)
    list(GET CMAKE_CUDA_ARCHITECTURES 0 ols_cuda_arch)
    string(REGEX REPLACE "-.*" "" ols_cuda_arch "${ols_cuda_arch}")
  endif()
  target_compile_definitions(open-local-services PRIVATE OLS_BACKEND="cuda" OLS_CUDA_ARCH="${ols_cuda_arch}")
else()
  target_compile_definitions(open-local-services PRIVATE OLS_BACKEND="vulkan")
endif()

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
  "${SD_VENDOR}/ggml/include"
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

pick_library(SD_LIB "${SD_BUILD}" "stable-diffusion")

# The image engine's own ggml is linked in whole, before the image library: the
# registry names the devices the pages show, and the base and the device
# libraries its build recorded carry the calls it makes - the probe answers
# from inside this binary, never from the shared library's copy. The linker
# reads a list left to right, so the archives sit after the objects that
# call them and ahead of any library that exports the same names.
pick_library(GGML_LIB "${SD_BUILD}/ggml/src" "ggml")
unset(olsGgmlParts)
unset(olsGgmlCuda)
foreach(olsGgmlPart ggml-base ggml-cpu ggml-cuda)
  # a component only where the build recorded one: a cpu shape carries no
  # cuda library, and asking for one that is not there is not a build error.
  # where the build kept a component in a folder of its own, its library is
  # read from there.
  pick_library_if_present(olsGgmlPartLib "${SD_BUILD}/ggml/src" "${olsGgmlPart}")
  if(NOT olsGgmlPartLib)
    pick_library_if_present(olsGgmlPartLib "${SD_BUILD}/ggml/src/${olsGgmlPart}" "${olsGgmlPart}")
  endif()
  if(olsGgmlPartLib)
    list(APPEND olsGgmlParts "${olsGgmlPartLib}")
    if(olsGgmlPart STREQUAL "ggml-cuda")
      set(olsGgmlCuda ON)
    endif()
  endif()
endforeach()
target_link_libraries(open-local-services PRIVATE
  "${GGML_LIB}"
  ${olsGgmlParts}
  "${SD_LIB}")

if(olsGgmlCuda)
  # the device library the build recorded asks of the CUDA runtime and cuBLAS:
  # the toolkit's import libraries are named where the toolkit stands, whole -
  # on Windows cuBLASLt keeps its own import library, unlike the linux side's
  # one shared runtime.
  if(CMAKE_CUDA_COMPILER)
    get_filename_component(cuda_bin "${CMAKE_CUDA_COMPILER}" PATH)
    get_filename_component(cuda_root "${cuda_bin}" PATH)
    unset(olsCudaRuntimeLibs)
    foreach(olsCudaRuntime cudart cublas cublasLt cuda)
      pick_library_if_present(olsCudaRuntimeLib "${cuda_root}/lib/x64" "${olsCudaRuntime}")
      if(olsCudaRuntimeLib)
        list(APPEND olsCudaRuntimeLibs "${olsCudaRuntimeLib}")
      endif()
    endforeach()
    target_link_libraries(open-local-services PRIVATE ${olsCudaRuntimeLibs})
  endif()
endif()

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
  if(CMAKE_CUDA_COMPILER)
    get_filename_component(cuda_bin "${CMAKE_CUDA_COMPILER}" PATH)
    get_filename_component(cuda_root "${cuda_bin}" PATH)
    set(cuda_lib "${cuda_root}/lib64")
    # this build is a CUDA build, so the api may ask the CUDA runtime at run
    # time what the GPU actually is; where it cannot ask, it must say so and
    # leave the engine alone
    target_compile_definitions(open-local-services PRIVATE OLS_CUDART_LINKED)
    if(EXISTS "${cuda_lib}/libcudart.so")
      target_link_options(open-local-services PRIVATE "-Wl,-rpath,${cuda_lib}" "-L${cuda_lib}")
      target_link_libraries(open-local-services PRIVATE cudart cublas cuda)
    endif()
  endif()
  # setup.sh may install the binary and its shared libraries elsewhere.
  target_link_options(open-local-services PRIVATE "-Wl,-rpath,$ORIGIN")
endif()

# The git core's own tests: a small harness of its own, built beside the server.
add_subdirectory("${ROOT}/tests/gitcore" "${CMAKE_BINARY_DIR}/git-tests")
