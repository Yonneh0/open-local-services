# Written once per build of the server, at configure time and again whenever a
# page changes: reads the webui folder whole and lays it as webui-embedded.cpp,
# the translation unit that carries the pages inside the binary. A released
# .exe answers with these bytes when no webui folder sits beside it, and the
# expand door writes them out to start serving the outside copies - so this
# file is the only place the build ever reads the folder, and the custom
# command in api/CMakeLists.txt names every page as an input of its output.
#
# A page is kept as ordinary string literals, byte for byte: a text page keeps
# its text with only the few characters a C++ literal needs escaped, and any
# other file - the icon - goes through octal escapes so its bytes stay exact.
# Long files are split at line ends, which keeps every literal inside what a
# compiler takes as one string.
#
# -Dweb_root=<folder>  the folder to read.
# -Dout=<file>         the .cpp to put the result at.
# The script states what it did on one line; a configure shows that line in
# its own output and a build shows it in the build log.

if(NOT DEFINED web_root OR NOT DEFINED out)
  message(FATAL_ERROR "this script is driven by api/CMakeLists.txt: -Dweb_root= and -Dout= are both required")
endif()
if(NOT IS_DIRECTORY "${web_root}")
  message(FATAL_ERROR "the pages must be in this tree to be carried inside the binary: ${web_root}")
endif()
file(GLOB web_items "${web_root}/*")
if(NOT web_items)
  message(FATAL_ERROR "the pages folder holds nothing to carry inside the binary")
endif()
list(SORT web_items)

function(ols_c_escape_text out value)
  string(REPLACE "\\" "\\\\" escaped "${value}")
  string(REPLACE "\"" "\\\"" escaped "${escaped}")
  string(REPLACE "\n" "\\n" escaped "${escaped}")
  string(REPLACE "\r" "\\r" escaped "${escaped}")
  string(REPLACE "\t" "\\t" escaped "${escaped}")
  set(${out} "${escaped}" PARENT_SCOPE)
endfunction()

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

# The stamped copy is put in place only when its bytes differ: a build that
# re-runs this script over the same pages leaves the file the compiler already
# saw, with its date, so nothing after it recompiles. The stamping is what
# lets the custom command re-run over every touched page without turning one
# saved file into a rebuild of the server.
get_filename_component(out_dir "${out}" DIRECTORY)
set(stamped "${out_dir}/webui-embedded.stamped.cpp")
file(WRITE "${stamped}"
  "// written by the api build from ${web_root} - do not edit\n"
  "#include \"WebAssets.h\"\n"
  "namespace ols { namespace webres {\n"
  "${declarations}"
  "const char *const kEmbeddedNames[${held}] = { ${name_list} };\n"
  "const char *const *const kEmbeddedParts[${held}] = { ${part_list} };\n"
  "const int kEmbeddedCount = ${held};\n"
  "} }\n")
file(SHA256 "${stamped}" stamped_sum)
set(standing_sum "")
if(EXISTS "${out}")
  file(SHA256 "${out}" standing_sum)
endif()
if(NOT stamped_sum STREQUAL standing_sum)
  file(RENAME "${stamped}" "${out}")
  message("pages carried inside this binary: ${held} files, from ${web_root} (written again)")
else()
  file(REMOVE "${stamped}")
  message("pages carried inside this binary: ${held} files, from ${web_root} (unchanged)")
endif()
