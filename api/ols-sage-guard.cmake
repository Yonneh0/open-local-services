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
# that no longer matches simply goes unedited rather than half-written.
set(OLS_SAGE_MATH "vendor/stable-diffusion.cpp/ggml/src/ggml-cuda/sageattention/math.cuh")
get_filename_component(OLS_ROOT "${CMAKE_CURRENT_LIST_DIR}" DIRECTORY)
set(OLS_SAGE_MATH "${OLS_ROOT}/${OLS_SAGE_MATH}")
set(OLS_SAGE_MARK "OLS rsqrt guard")
set(OLS_SAGE_OLD "__forceinline__ __device__ float rsqrt(float x) {")
if(EXISTS "${OLS_SAGE_MATH}")
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
    endif()
  endif()
endif()
