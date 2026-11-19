# Writes the moment of this build into a .cpp of its own. This runs as a
# build step, never as a configure step: every build of the server - a bare
# re-link with nothing else dirty included - runs it, so the binary can carry
# no moment but the one it was actually made at. The file is only replaced
# when its bytes differ, and the bytes name a second, so a build that lands
# in the same second as its predecessor says the same truth and costs no
# recompiling.
string(TIMESTAMP moment "%b %d %Y %H:%M:%S" UTC)
file(WRITE "${out}.standing"
  "// written fresh by the build - do not edit\n"
  "#include <string>\n"
  "namespace ols { namespace platform {\n"
  "std::string buildMoment()\n{\n"
  "  return \"${moment} UTC\";\n"
  "}\n"
  "} }\n")
file(SHA256 "${out}.standing" fresh)
set(standing "")
if(EXISTS "${out}")
  file(SHA256 "${out}" standing)
endif()
if(NOT fresh STREQUAL standing)
  file(RENAME "${out}.standing" "${out}")
else()
  file(REMOVE "${out}.standing")
endif()
