cmake_minimum_required(VERSION 3.14)
project(open-local-services LANGUAGES CXX)

set(ROOT ${CMAKE_CURRENT_SOURCE_DIR}/..)
set(SD_VENDOR ${PROJECT_SOURCE_DIR}/vendor/stable-diffusion.cpp)

# The trees the image engine was built in; the voice engine is audio.cpp - its
# C API is linked into this binary, so there is no voice engine process and no
# library to pick.
# The voice engine's ggml is the machine's device probe: the same static
# library that runs the voice models enumerates the GPU the pages show.
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

# The voice engine, built in the api tree beside everything else here: every
# model the engine knows is compiled in (full), so the pages carry every family
# and package its own specs name - a family this machine has no files for is
# listed as such and offers the hand that opens it once the files come. eSpeak
# is linked statically (the validated Windows path for Kokoro's phonemizer, no
# dll needed), and the device backend follows the platform like the other
# engines.
set(AUDIOCPP_MODEL_SET "full" CACHE STRING "voice engine: every model the engine knows")
set(AUDIOCPP_STATIC_ESPEAK ON CACHE BOOL "link eSpeak-ng's code in; keep its data package")
# The device backend follows the platform - the CUDA device on Linux, the Vulkan
# device on Windows - unless the build names OLS_CUDA, which asks for the CUDA
# device on Windows too: a machine whose card is CUDA-first is then built whole
# on it, the engines with it. The scripts that do this name the build root for
# the backend, so the two builds of one tree never share a cache.
if(NOT OLS_NO_DEVICE)
  if(OLS_CUDA)
    set(ENGINE_ENABLE_CUDA ON CACHE BOOL "voice engine runs on the CUDA device")
    message(STATUS "device backend for this build: CUDA, named by -DOLS_CUDA")
  elseif(MSVC)
    set(ENGINE_ENABLE_VULKAN ON CACHE BOOL "voice engine runs on the Vulkan device")
  else()
    set(ENGINE_ENABLE_CUDA ON CACHE BOOL "voice engine runs on the CUDA device")
  endif()
else()
  message(STATUS "no device backend in this target: the voice engine carries the cpu backend only")
endif()
add_subdirectory("${PROJECT_SOURCE_DIR}/vendor/audio.cpp" "${CMAKE_BINARY_DIR}/audio-engine")

# The pages carried inside the binary. The webui folder of the tree this build
# stands in is read whole into webui-embedded.cpp when the build configures, and
# that file is compiled into the server: a released .exe carries its own pages,
# so it answers with them when no webui folder sits beside it, and the expand
# door writes them out to start serving the outside copies. Nothing is generated
# at run time and nothing watches the folder - the choice between the outside
# copy and the inside one is a bool, set when the server is pointed at its
# pages - so the build is the only place these bytes are read.
#
# A page is kept as ordinary string literals, byte for byte: a text page keeps
# its text with only the few characters a C++ literal needs escaped, and any
# other file - the icon - goes through octal escapes so its bytes stay exact.
# Long files are split at line ends, which keeps every literal inside what a
# compiler takes as one string.
function(ols_c_escape_text out value)
  string(REPLACE "\\" "\\\\" escaped "${value}")
  string(REPLACE "\"" "\\\"" escaped "${escaped}")
  string(REPLACE "\n" "\\n" escaped "${escaped}")
  string(REPLACE "\r" "\\r" escaped "${escaped}")
  string(REPLACE "\t" "\\t" escaped "${escaped}")
  set(${out} "${escaped}" PARENT_SCOPE)
endfunction()

function(ols_write_webui_embedded out_file)
  file(REAL_PATH "${ROOT}/webui" web_root)
  if(NOT IS_DIRECTORY "${web_root}")
    message(FATAL_ERROR "the pages must be in this tree to be carried inside the binary: ${web_root}")
  endif()
  file(GLOB web_items "${web_root}/*")
  if(NOT web_items)
    message(FATAL_ERROR "the pages folder holds nothing to carry inside the binary")
  endif()
  list(SORT web_items)

  set(declarations "")
  set(name_list "")
  set(part_list "")
  set(held 0)
  foreach(item ${web_items})
    if(IS_DIRECTORY "${item}")
      continue()
    endif()
    set(src "${item}")
    get_filename_component(page_name "${src}" NAME)
    string(REPLACE "." "_" cid "${page_name}")
    string(REGEX REPLACE "[^A-Za-z0-9_]" "_" cid "${cid}")
    math(EXPR held "${held} + 1")
    string(APPEND name_list "\"${page_name}\", ")
    string(APPEND part_list "kOlsParts_${cid}, ")
    set(one_file "static const char *const kOlsParts_${cid}[] = {\n")
    get_filename_component(page_ext "${src}" LAST_EXT)
    if(page_ext MATCHES "^\\.(html|js|css|svg|txt|md)$")
      file(READ "${src}" page_text)
      # The whole page is escaped in one pass, then cut into literals at line
      # ends. A cut only ever sits after the two characters a line end became,
      # so no escape is split between two literals. Eight thousand bytes is the
      # ceiling a literal gets: an older MSVC refuses one past about sixteen
      # thousand of its own characters, and that compiler is what the other
      # Windows box builds with.
      ols_c_escape_text(whole "${page_text}")
      set(rest "${whole}")
      while(1)
        string(LENGTH "${rest}" rest_len)
        if(rest_len LESS_EQUAL 8000)
          if(NOT rest STREQUAL "")
            string(APPEND one_file "  \"${rest}\",\n")
          endif()
          break()
        endif()
        string(SUBSTRING "${rest}" 4000 4000 tail)
        string(FIND "${tail}" "\\n" found)
        if(found EQUAL -1)
          # a stretch with no line end in it is cut plainly, and a cut that
          # would fall inside an escaped pair is moved back by one.
          set(cut 8000)
          string(SUBSTRING "${rest}" 7990 10 probe)
          set(run 0)
          set(i 9)
          while(i GREATER_EQUAL 0)
            string(SUBSTRING "${probe}" "${i}" 1 ch)
            if(ch STREQUAL "\\")
              math(EXPR run "${run} + 1")
            else()
              set(i -1)
            endif()
            math(EXPR i "${i} - 1")
          endwhile()
          math(EXPR odd "${run} % 2")
          if(odd EQUAL 1)
            math(EXPR cut "8000 - 1")
          endif()
        else()
          math(EXPR cut "4000 + ${found} + 2")
        endif()
        string(SUBSTRING "${rest}" 0 "${cut}" piece)
        string(SUBSTRING "${rest}" "${cut}" -1 rest)
        string(APPEND one_file "  \"${piece}\",\n")
      endwhile()
    else()
      # The bytes of a picture or an icon become octal escapes, taken a whole
      # byte at a time and closed when the bytes run out - a loop that counts the
      # bytes it has read, never one that counts the bytes left standing. A byte
      # costs four characters of source - a backslash and three digits - so the
      # ceiling the words above ask for is three and a half thousand bytes, and a
      # count in bytes that forgot the cost would pass it more than once over.
      file(READ "${src}" page_hex HEX)
      string(LENGTH "${page_hex}" hex_len)
      set(hex_pos 0)
      set(accum "")
      set(accum_bytes 0)
      while(hex_pos LESS hex_len)
        string(SUBSTRING "${page_hex}" "${hex_pos}" 2 hex_byte)
        math(EXPR byte_value "0x${hex_byte}")
        math(EXPR oct_a "${byte_value} / 64")
        math(EXPR oct_b "(${byte_value} / 8) % 8")
        math(EXPR oct_c "${byte_value} % 8")
        string(APPEND accum "\\${oct_a}${oct_b}${oct_c}")
        math(EXPR accum_bytes "${accum_bytes} + 1")
        math(EXPR hex_pos "${hex_pos} + 2")
        if(accum_bytes GREATER_EQUAL 3500)
          string(APPEND one_file "  \"${accum}\",\n")
          set(accum "")
          set(accum_bytes 0)
        endif()
      endwhile()
      if(NOT accum STREQUAL "")
        string(APPEND one_file "  \"${accum}\",\n")
      endif()
    endif()
    string(APPEND one_file "  nullptr,\n};\n")
    string(APPEND declarations "${one_file}")
  endforeach()

  # The stamped copy is put in place only when its bytes differ: a configure
  # that read the same pages leaves the file the compiler already saw, with its
  # date, so the build after it compiles nothing new.
  set(stamped "${CMAKE_BINARY_DIR}/webui-embedded.stamped.cpp")
  file(WRITE "${stamped}"
    "// written by the api build from ${web_root} when it configured - do not edit\n"
    "#include \"WebAssets.h\"\n"
    "namespace ols { namespace webres {\n"
    "${declarations}"
    "const char *const kEmbeddedNames[${held}] = { ${name_list} };\n"
    "const char *const *const kEmbeddedParts[${held}] = { ${part_list} };\n"
    "const int kEmbeddedCount = ${held};\n"
    "} }\n")
  file(SHA256 "${stamped}" stamped_sum)
  set(standing_sum "")
  if(EXISTS "${out_file}")
    file(SHA256 "${out_file}" standing_sum)
  endif()
  if(NOT stamped_sum STREQUAL standing_sum)
    file(RENAME "${stamped}" "${out_file}")
    message(STATUS "pages carried inside this binary: ${held} files, from ${web_root} (written again)")
  else()
    file(REMOVE "${stamped}")
    message(STATUS "pages carried inside this binary: ${held} files, from ${web_root} (unchanged)")
  endif()
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
  "// written by the api build when it configured - do not edit\n"
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
# rule more: it is written by a build step that never counts itself up to
# date, so every build of the server stamps the binary afresh - a bare re-link
# after a single .cpp included. The compiler's own dates cannot say this:
# __DATE__ and __TIME__ are words each object speaks for itself, and an
# incremental build re-links objects it never touched, letting an old object
# date a newer binary.
set(BUILD_MOMENT_CPP "${CMAKE_BINARY_DIR}/build-moment.cpp")
add_custom_target(build-moment ALL
  COMMAND ${CMAKE_COMMAND} -Dout="${BUILD_MOMENT_CPP}"
          -P "${CMAKE_CURRENT_LIST_DIR}/cmake/write-build-moment.cmake")
set_source_files_properties("${BUILD_MOMENT_CPP}" PROPERTIES GENERATED TRUE)

add_executable(open-local-services
  main.cpp
  OpenLocalServices.cpp
  MonitorService.cpp
  ImageService.cpp
  VerifiedRoom.cpp
  Deployment.cpp
  VoiceService.cpp
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
elseif(ENGINE_ENABLE_CUDA)
  set(ols_cuda_arch "")
  if(CMAKE_CUDA_ARCHITECTURES)
    list(GET CMAKE_CUDA_ARCHITECTURES 0 ols_cuda_arch)
    string(REGEX REPLACE "-.*" "" ols_cuda_arch "${ols_cuda_arch}")
  endif()
  target_compile_definitions(open-local-services PRIVATE OLS_BACKEND="cuda" OLS_CUDA_ARCH="${ols_cuda_arch}")
else()
  target_compile_definitions(open-local-services PRIVATE OLS_BACKEND="vulkan")
endif()

# A small console entry that starts the linked voice engine and speaks one
# line, used by the release scripts to prove the engine really runs inside
# the shipped binary before anything gets deployed.
add_executable(voice-probe
  voice-probe.cpp
  VoiceService.cpp)
set_property(TARGET voice-probe PROPERTY CXX_STANDARD 17)

# The C ABI facade of the voice engine, compiled here from the vendored tree
# and linked against the engine's own runtime: this is the whole of the seam,
# the module above calls the C API directly.
add_library(ols-voice-capi OBJECT
  "${PROJECT_SOURCE_DIR}/vendor/audio.cpp/src/capi/audiocpp.cpp")
set_property(TARGET ols-voice-capi PROPERTY CXX_STANDARD 17)
target_link_libraries(ols-voice-capi PRIVATE engine_runtime)

if(MSVC)
  target_compile_options(open-local-services PRIVATE /utf-8 /MP)
  add_compile_definitions(_CRT_SECURE_NO_WARNINGS _WINSOCK_DEPRECATED_NO_WARNINGS)
else()
  add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
  target_compile_options(open-local-services PRIVATE -O2)
endif()

target_include_directories(open-local-services PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}"
  "${PROJECT_SOURCE_DIR}/vendor"
  "${SD_VENDOR}/include"
  "${SD_VENDOR}/thirdparty")

target_include_directories(voice-probe PRIVATE
  "${PROJECT_SOURCE_DIR}/vendor")

# The version a build names (-DOLS_VERSION: dev in the development scripts, the
# commit in the release builders) is turned into that stated version above,
# where the server's files are gathered; nothing here reads it again.

function(pick_library out base name)
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
  if(NOT found)
    message(FATAL_ERROR "missing library ${name} under ${base}")
  endif()
  set(${out} "${found}" PARENT_SCOPE)
endfunction()

pick_library(SD_LIB "${SD_BUILD}" "stable-diffusion")

# The voice engine links as real targets from the tree added above: its
# runtime pulls in its ggml, and its ggml pulled in whatever device library
# that build recorded - the same chain the standalone app linked, now ending
# inside this binary.
if(NOT MSVC)
  # On Windows each engine keeps its own library and the two ggmls cannot see
  # each other. Here one binary holds the voice engine's ggml and one shared
  # library carries the image engine's, so the order of the link decides which
  # copy answers the server's own ggml calls. measured on the linux build with
  # the image library first: a pocket tts job reached ggml_backend_graph_compute
  # inside the image library, whose ggml_nrows then read a graph the other
  # copy had built, and the server stopped on a segmentation fault. So the
  # voice engine's own archives are named before the image library here - as
  # link options, which reach the linker before any library does - and their
  # symbols are kept out of the dynamic table below, so each side answers
  # itself and neither is handed a tensor it was not built for.
  # The voice engine's ggml is named here, between its own objects and the
  # image library: the linker reads a list left to right, so an archive has to
  # sit after the objects that call it and before any shared library that
  # exports the same names. Named before every library instead, the archives
  # answer nothing - nothing was undefined at that point yet - and the voice
  # engine's calls still fell through to the image library's copy.
  unset(olsGgmlTargets)
  foreach(olsGgmlTarget ggml ggml-base ggml-cpu ggml-cuda)
    if(TARGET ${olsGgmlTarget})
      list(APPEND olsGgmlTargets ${olsGgmlTarget})
    endif()
  endforeach()
  target_link_libraries(open-local-services PRIVATE
    ols-voice-capi
    engine_runtime
    ${olsGgmlTargets}
    "${SD_LIB}")
else()
  target_link_libraries(open-local-services PRIVATE
    "${SD_LIB}"
    ols-voice-capi
    engine_runtime)
endif()

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

target_link_libraries(voice-probe PRIVATE
  ols-voice-capi
  engine_runtime)

if(NOT MSVC)
  find_package(Threads REQUIRED)
  target_link_libraries(open-local-services PRIVATE pthread)
  # The voice engine's ggml is linked in statically. Its symbols have to stay
  # out of the dynamic table, or the stable-diffusion library would bind its
  # own ggml calls to this copy instead of the one already inside it.
  # The names are kept local and cleared first: a plain GGML_STATIC is also a
  # cached option's name, and a cache entry left there (GGML_STATIC:BOOL=OFF)
  # would be joined in front of the archive list and reach the linker as
  # "--exclude-libs=OFF:libggml.a…", which excludes nothing.
  unset(olsGgmlHidden)
  if(NOT OLS_NO_DEVICE)
    list(APPEND olsGgmlHidden ggml-cuda)
  endif()
  unset(olsGgmlArchives)
  foreach(ggml_archive ggml ggml-base ggml-cpu ${olsGgmlHidden})
    list(APPEND olsGgmlArchives "lib${ggml_archive}.a")
  endforeach()
  string(JOIN ":" olsGgmlList ${olsGgmlArchives})
  # the same archives, named where the linker reads them before any library: a
  # link option is put on the command line ahead of the libraries a target names
  # and ahead of the objects that need them, so the order that works is the one
  # this target's own libraries are given in - see the block above, which names
  # the ggml targets between the voice engine and the image library.
  target_link_options(open-local-services PRIVATE "-Wl,--exclude-libs=${olsGgmlList}")
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
