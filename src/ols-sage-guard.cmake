# A configure-time guard for the pinned image engine, passed to its CMake by both
# entry scripts (CMAKE_PROJECT_INCLUDE_BEFORE) so it runs on every box the same way
# and before any of its targets exist.
#
# Why it is needed. sageattention/math.cuh in the pinned engine declares a device
# wrapper named rsqrt. CUDA 12.9 cannot hold that declaration beside glibc's host
# one - the clash UM780's settled header state answers by mapping rsqrt to a macro,
# which that state's own tool documents as the only form that works for the kernels
# calling these names - and a declaration cannot survive a macro of the same name:
# `float rsqrt(float x)` is rewritten before the compiler sees it, and the file dies
# with "expected an identifier" at sage-attn.cu.
#
# So the wrapper is stepped aside exactly where a map stands in for it, and stands
# as upstream wrote it everywhere else. That is the whole change: where the map is
# there, every call site still gets 1/sqrt through it, and where it is not, nothing
# was touched. On any card below compute 8.9 sage attention is never chosen at
# runtime anyway - the engine's own mode function returns AUTO there - so no kernel
# this pair runs changes shape.
#
# The guard marks the file it edited, so a reconfigure is a no-op and a moved pin
# that no longer matches goes unedited rather than half-written - and says so,
# because a build that dies two files away with words about an identifier gives
# nobody a reason to look here.
#
# The file is looked for where the repository keeps its vendored engine, then at
# the two other shapes a tree may take: a guard that finds nothing used to say
# nothing, and that is what this one did for a directory's worth of difference.
set(OLS_SAGE_REL "ggml/src/ggml-cuda/sageattention/math.cuh")
get_filename_component(OLS_ROOT "${CMAKE_CURRENT_LIST_DIR}" DIRECTORY)
set(OLS_SAGE_TRIES
    "${OLS_ROOT}/vendor/stable-diffusion.cpp/${OLS_SAGE_REL}"
    "${OLS_ROOT}/api/vendor/stable-diffusion.cpp/${OLS_SAGE_REL}")
set(OLS_SAGE_TRIED_SOURCE "")
if(CMAKE_SOURCE_DIR)
  list(APPEND OLS_SAGE_TRIES "${CMAKE_SOURCE_DIR}/${OLS_SAGE_REL}")
  set(OLS_SAGE_TRIED_SOURCE ", and the project being configured")
endif()
set(OLS_SAGE_MATH "")
foreach(OLS_SAGE_TRY ${OLS_SAGE_TRIES})
  if(EXISTS "${OLS_SAGE_TRY}")
    set(OLS_SAGE_MATH "${OLS_SAGE_TRY}")
    break()
  endif()
endforeach()
set(OLS_SAGE_MARK "OLS rsqrt guard")
set(OLS_SAGE_OLD "__forceinline__ __device__ float rsqrt(float x) {")
if(OLS_SAGE_MATH STREQUAL "")
  # Named where it is heard: nothing is edited and nothing is failed, but a box
  # that passes this guard and has no engine tree where any of these say is a box
  # whose pin moved, and its build will say so in words about an identifier.
  message(WARNING
    "no image engine found for the rsqrt guard: nothing was guarded.\n"
    "Looked under ${OLS_ROOT}/vendor, ${OLS_ROOT}/api/vendor${OLS_SAGE_TRIED_SOURCE}.")
elseif(EXISTS "${OLS_SAGE_MATH}")
  file(READ "${OLS_SAGE_MATH}" OLS_SAGE_TEXT)
  if(NOT OLS_SAGE_TEXT MATCHES "${OLS_SAGE_MARK}")
    string(FIND "${OLS_SAGE_TEXT}" "${OLS_SAGE_OLD}" OLS_SAGE_AT REVERSE)
    if(NOT OLS_SAGE_AT EQUAL -1)
      # The body is four lines: the declaration, the value, the assembly and the
      # return. It is wrapped, not rewritten, so upstream's code is still the code.
      string(SUBSTRING "${OLS_SAGE_TEXT}" ${OLS_SAGE_AT} -1 OLS_SAGE_TAIL)
      string(FIND "${OLS_SAGE_TAIL}" "\n}" OLS_SAGE_END)
      math(EXPR OLS_SAGE_KEEP "${OLS_SAGE_END} + 2")
      string(SUBSTRING "${OLS_SAGE_TAIL}" 0 ${OLS_SAGE_KEEP} OLS_SAGE_BLOCK)
      string(REPLACE "${OLS_SAGE_BLOCK}"
             "#ifdef rsqrt /* ${OLS_SAGE_MARK}: a map for this name stands in for the wrapper */\n#else\n${OLS_SAGE_BLOCK}\n#endif\n"
             OLS_SAGE_NEW "${OLS_SAGE_TEXT}")
      if(NOT OLS_SAGE_NEW STREQUAL OLS_SAGE_TEXT)
        file(WRITE "${OLS_SAGE_MATH}" "${OLS_SAGE_NEW}")
        message(STATUS "the sageattention rsqrt wrapper is guarded against this box's header state")
      endif()
    else()
      # The file stands here, carries no mark, and is not the text this guard
      # knows: the pin moved under it. Silence here is what makes that expensive -
      # the build then dies at sage-attn.cu with words about an identifier, on the
      # card generations where these kernels are compiled at all, and nothing in
      # that message names the file to look at. Say it where it is heard.
      message(WARNING
        "the pinned image engine declares its rsqrt wrapper in words this guard does not know: ${OLS_SAGE_MATH}\n"
        "It was left as upstream wrote it. Where this box's header maps that name, the sage kernels will not compile - re-read src/ols-sage-guard.cmake against the new pin.")
    endif()
  endif()
endif()
