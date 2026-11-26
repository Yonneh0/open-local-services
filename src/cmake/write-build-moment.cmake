# Writes the moment of this build into a .cpp of its own. This runs as a
# build step, never as a configure step: every build of the server - a bare
# re-link with nothing else dirty included - runs it, so the binary can carry
# no moment but the one it was actually made at. The file is only replaced
# when its bytes differ, and the bytes name a second, so a build that lands
# in the same second as its predecessor says the same truth and costs no
# recompiling. Either way the file's mtime moves to now: ninja believes an
# object up to date while its source's mtime has not passed it, and restat
# alone does not talk it out of that - so a stamp older than this step must
# not stand even when its words have not changed, or the link keeps an
# object that post-dates what it names. The stamp is never newer than the
# moment it speaks; the binary is never older than the stamp it carries.
string(TIMESTAMP moment "%b %d %Y %H:%M:%S" UTC)
string(TIMESTAMP epoch "%s" UTC)
file(WRITE "${out}.standing"
  "// written fresh by the build - do not edit\n"
  "#include <string>\n"
  "namespace ols { namespace platform {\n"
  "std::string buildMoment()\n{\n"
  "  return \"${moment} UTC\";\n"
  "}\n"
  "long long buildMomentSeconds()\n{\n"
  "  return ${epoch}LL;\n"
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
file(TOUCH "${out}")
