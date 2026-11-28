#!/bin/sh
# open-local-services entry point for Linux.
# One script does the whole onboarding: it discovers the tooling this box has
# (cmake, the newest CUDA toolkit with an nvcc, this GPU's compute capability),
# builds whatever is missing - vendor engines, open-local-services - and
# then starts the server in the foreground. Already-built steps are skipped.
# A tree with no C++ sources (a runtime tree) only starts its server.
# Set OLS_RUNTIME_DIR when this tree is only build input; the binary and its
# shared libraries are installed into that tree.
# Pass "build" to build and install without starting the server.
cd "$(dirname "$0")"

# configuration: defaults first, then the process environment, then .env —
# the same order the server itself uses.
envval() {
  fromenv=$(eval "printf '%s' \"\${$1}\"")
  if [ -n "$fromenv" ]; then printf '%s' "$fromenv"; return; fi
  [ -f .env ] && sed -n "s/^$1=//p" .env | head -n 1
}
HOST=$(envval OLS_HOST); [ -n "$HOST" ] || HOST=0.0.0.0
# The port defaults to 8765. OLS_PORT may ask for another port from the
# environment or .env. If that port is in use, the server refuses to start.
PORT=$(envval OLS_PORT)
if [ -z "$PORT" ]; then
  PORT=8765
fi
case "$PORT" in
  *[!0-9]*)
    echo "Invalid OLS_PORT=$PORT: use a number from 1 to 65535."
    exit 1
    ;;
  0)
    echo "Invalid OLS_PORT=$PORT: use a number from 1 to 65535."
    exit 1
    ;;
esac
if [ "$PORT" -gt 65535 ]; then
  echo "Invalid OLS_PORT=$PORT: use a number from 1 to 65535."
  exit 1
fi
LINK=$HOST
[ "$HOST" = 0.0.0.0 ] && LINK=127.0.0.1
MODE=run
FRESH_FULL=
# what the words ask for, in any order: "build" builds and installs without
# starting, key= and rate= are the same two settings run.bat takes, and
# fresh-full-rebuild (or --fresh-full-rebuild) is the one word that lets this
# path restate a tree's shape - see the shape check below, which refuses to
# rebuild a whole tree from scratch without it. cuda, cpu and cpu-avx512 name
# an engine shape; without one the machine chooses, as below.
SHAPE_WANTED=
for opt in "$@"; do
  case "$opt" in
    build) MODE=build ;;
    cuda | cpu | cpu-avx512)
      if [ -n "$SHAPE_WANTED" ]; then
        echo "cuda, cpu and cpu-avx512 name three shapes: name one of them, or none."
        exit 1
      fi
      SHAPE_WANTED=$opt
      ;;
    key=*) OLS_API_KEY="${opt#key=}"; export OLS_API_KEY ;;
    rate=*) OLS_RATE_LIMIT="${opt#rate=}"; export OLS_RATE_LIMIT ;;
    fresh-full-rebuild | --fresh-full-rebuild) FRESH_FULL=1 ;;
  esac
done

echo ""
echo "open-local-services"
echo "==================="
echo "Builds only what is missing, then starts the server."
echo "The pages will be at:  http://$LINK:$PORT/"
echo "The configured port is the only one these doors answer on. If another server holds it,"
echo "this one says so and does not start; the running one is stopped for you when"
echo "this script has built what was missing."
echo ""

# ---- a server already running is fine: it is stopped after the build below
# succeeds, right before its binary is replaced. A failed build exits earlier,
# so the running server never gets downtime from a broken compile. ----


# ---- a runtime tree has no sources: just start its server ----
if [ ! -f src/main.cpp ]; then
  if [ ! -f src/open-local-services ]; then
    echo "This tree has no C++ sources and no src/open-local-services to start."
    echo "Transfer a source tree, or set OLS_RUNTIME_DIR to install into this tree."
    exit 1
  fi
  if pidof open-local-services >/dev/null 2>&1; then
    echo "Stopping the running open-local-services before starting this one."
    kill $(pidof open-local-services)
  fi
  echo "Starting open-local-services. It names the address it answers at as it starts."
  # two ggml copies share this process: the one linked into the binary, and the
  # one inside the image library beside it. ggml stops a process that installs
  # its crash handler twice - before it could listen - so no copy here installs
  # one, and the server starts. a crash still prints its own words, in the same
  # line the failing check names.
  export GGML_NO_BACKTRACE=1
  exec src/open-local-services
fi

# ---- discover the tooling ----
if ! command -v cmake >/dev/null 2>&1; then
  echo "cmake was not found on this machine. Install it, then run this again."
  exit 1
fi

# /usr/local/cuda is the newest toolkit, which may carry no nvcc for this GPU;
# take the newest CUDA toolkit that actually has one. A box with none builds
# the cpu engine shape instead: the server itself asks nothing of a toolkit,
# and the engine library beside it is what carries a device.
NVCC=""
for D in /usr/local/cuda-*/bin/nvcc /usr/local/cuda/bin/nvcc; do
  [ -x "$D" ] && NVCC="$D"
done
# whether this box takes the card shape or the cpu shape is the nvcc above,
# unless a shape word was named - and naming cuda on a box with no toolkit is
# refused, not quietly answered with another shape. The generations the engine
# carries are named below, not guessed from the card in front of it, so one
# library answers on the box that built it and on every box of those
# generations. cpu-avx512 is a shape of its own because it is a claim of its
# own: see the flags below and the loader that hands that name to a chip which
# says yes to it.
ENGINE_SHAPE=cpu
if [ -n "$NVCC" ]; then ENGINE_SHAPE=cuda; fi
if [ -n "$SHAPE_WANTED" ]; then
  if [ "$SHAPE_WANTED" = cuda ] && [ -z "$NVCC" ]; then
    echo "cuda was named; no nvcc under /usr/local answers. The cpu shapes are what this box can build."
    exit 1
  fi
  ENGINE_SHAPE=$SHAPE_WANTED
fi
# the CUDA engine library carries the kernels for every generation this
# project supports: the loader inside it picks each card's own when it is
# opened, so the machine that builds and the machines that run need no
# matching of generations.
CUDA_ARCH_LIST="70;75;86"
if [ "$ENGINE_SHAPE" = cuda ]; then
  echo "CUDA compiler: $NVCC"
  echo "engine shape: cuda, kernels for sm_70, sm_75 and sm_86 in one library"
elif [ "$ENGINE_SHAPE" = cpu-avx512 ]; then
  echo "engine shape: cpu-avx512, standing as libstable-diffusion-cpu-avx512.so for a chip that answers avx512"
else
  if [ -z "$NVCC" ]; then
    echo "No CUDA toolkit with an nvcc found under /usr/local: the engine builds on the cpu alone"
  fi
  echo "engine shape: cpu, built for the avx2 floor this fleet stands on"
fi
JN=$(nproc)

RUNTIME=${OLS_RUNTIME_DIR:-.}
# the comparison below is against an absolute path, so settle this one first
if [ -d "$RUNTIME" ]; then RUNTIME=$(cd "$RUNTIME" && pwd); fi
mkdir -p vendor "$RUNTIME/weights" "$RUNTIME/data"

# a current glibc declares rsqrtf for the host before nvcc gets its device
# intrinsic, and CUDA 12 ships no header that undoes that; declaring the device
# one first makes the CUDA kernels of a current ggml compile. this is a build
# setting, not a change to any engine source, and the pinned trees are the same
# on both machines.
CUDA_SHIM="$PWD/data/.rsqrtf-device-decl.h"
# the header is written only when its text is not already there: every CUDA
# object names it as a forced include, so a run that rewrote it, unchanged,
# would set its timestamp forward and ask the compiler for the whole kernel set
# again. A run that changes nothing keeps the file and its stamp.
printf '%s\n' '#pragma once' '#ifdef __CUDACC__' \
  'extern "C" __device__ float rsqrtf(float);' '#endif' > "$CUDA_SHIM.new"
if ! cmp -s "$CUDA_SHIM.new" "$CUDA_SHIM" 2>/dev/null; then
  mv -f "$CUDA_SHIM.new" "$CUDA_SHIM"
else
  rm -f "$CUDA_SHIM.new"
fi
CUDA_FLAGS="-include $CUDA_SHIM"
CUDA_FLAGS_OPT="-DCMAKE_CUDA_FLAGS=$CUDA_FLAGS"

# ---- the roots the engine shape builds in, named like run.bat names them ----
# the CUDA engine builds in build-cuda, the cpu engine in the unqualified
# build, and the avx512 claim in build-avx512: one tree per shape, so no two
# of them ever share a cache - and the same roots build-rel.sh builds the same
# shapes in, which is what keeps a release build and a development build of one
# shape the same incremental tree. Two entry points naming one root with the
# same defines is what keeps the build incremental: a flipped define changes
# every compile command in the tree and rebuilds an engine for hours over and
# over.
if [ "$ENGINE_SHAPE" = cuda ]; then
  SD_BUILD="vendor/stable-diffusion.cpp/build-cuda"
elif [ "$ENGINE_SHAPE" = cpu-avx512 ]; then
  SD_BUILD="vendor/stable-diffusion.cpp/build-avx512"
else
  SD_BUILD="vendor/stable-diffusion.cpp/build"
fi
API_BUILD="build"

# the exact vendor commits this code is written against. the release gate is the
# same test result on both machines, and that only holds while both build the
# same engine source - so the pins are named here, once, and checked below.
# a maintainer who moves a pin moves it in run.bat to match.
PIN_IMAGE=b13a48219d5cdf408edcb93da6cd69b612ed03d3        # leejet/stable-diffusion.cpp
PIN_IMAGE_GGML=dc998771e700d49bbdf9a39cede22bb5b3311767   # leejet/ggml (image engine)

# ---- the shape of an existing tree is asked before it is touched ----
# every one of these names decides the compile commands of every object in that
# tree, so asking a tree for another card generation or another backend would
# make cmake rewrite those commands and ask the compiler for the whole tree
# again - hours of work that reads nothing new. A path that wants that says so
# and stops; the caller who means it passes fresh-full-rebuild.
read_cache() {
  # $1 = cache file, $2 = define name. the cache types these with whatever
  # suffix it likes, so only the name before the first = is matched.
  sed -n "s/^$2[^=]*=\(.*\)/\1/p" "$1" | head -n 1
}
refuses_a_full_rebuild() {
  echo "This tree holds the shape of another build: its cache names"
  echo "  $SHAPE_NAME = $SHAPE_HAS, and this run asks for $SHAPE_WANT."
  echo "Restating it would recompile everything the tree holds, which is a full"
  echo "rebuild - this path does not do one silently. Nothing here clears a tree:"
  echo "to rebuild that shape from scratch on purpose, run again with"
  echo "fresh-full-rebuild, or name the shape that tree already holds."
  exit 1
}
check_shape() {
  # $1 = cache file, $2 = define name, $3 = the value this shape asks for.
  # a name this shape does not ask for is this tree's shape alone, so it is
  # not asked of a tree that never carried it.
  SHAPE_CACHE=$1
  SHAPE_NAME=$2
  SHAPE_WANT=$3
  [ -n "$SHAPE_WANT" ] || return 0
  SHAPE_HAS=$(read_cache "$SHAPE_CACHE" "$SHAPE_NAME")
  [ -n "$SHAPE_HAS" ] || return 0
  if [ "$SHAPE_HAS" != "$SHAPE_WANT" ]; then
    if [ "$FRESH_FULL" = 1 ]; then
      # the promise the word makes, kept: the tree is restated, not nursed.
      # clearing it is what makes the configure below run again with this
      # shape's flags - a configure stamp that survived here would leave the
      # old claim in the objects while this path reported a new one, which is
      # the silent reshape the checks above exist to refuse.
      echo "fresh-full-rebuild was given: $SD_BUILD is cleared, then configured for $SHAPE_NAME=$SHAPE_WANT."
      SHAPE_RESTATED=1
      return 0
    fi
    refuses_a_full_rebuild
  fi
  return 0
}

# ---- fetch missing vendor sources ----
if [ ! -f vendor/nlohmann/json.hpp ]; then
  echo "Fetching the json header"
  mkdir -p vendor/nlohmann
  curl -sSL --fail -o vendor/nlohmann/json.hpp \
    https://raw.githubusercontent.com/nlohmann/json/develop/single_include/nlohmann/json.hpp || exit 1
fi
if [ ! -d vendor/stable-diffusion.cpp ]; then
  echo "Fetching the image engine source"
  git clone --depth 1 https://github.com/leejet/stable-diffusion.cpp vendor/stable-diffusion.cpp || exit 1
fi
for SUB in vendor/stable-diffusion.cpp; do
  if [ -f "$SUB/.gitmodules" ] && [ ! -d "$SUB/ggml" ]; then
    echo "Fetching the ggml this engine was written against"
    git -C "$SUB" submodule update --init --depth 1 ggml || exit 1
  fi
done

# ---- vendor sources: clone what is missing ----
pin() {
  # $1 = folder, $2 = commit, $3 = owner/name on github. a folder already
  # recorded at that commit is done; a git tree that can see the commit is
  # checked out; anything else takes that exact snapshot from github, so both
  # machines end up compiling the same source even when the commit is no
  # longer reachable from what this box happens to have.
  dir=$1
  sha=$2
  repo=$3
  if [ -f "$dir/.pin" ] && [ "$(cat "$dir/.pin")" = "$sha" ]; then
    return 0
  fi
  if [ -d "$dir/.git" ]; then
    if git -C "$dir" rev-parse --quiet --verify "$sha" >/dev/null; then
      if git -C "$dir" checkout --quiet --detach "$sha"; then
        echo "$sha" > "$dir/.pin"
        return 0
      fi
    fi
    if git -C "$dir" fetch --quiet --depth 1 origin "$sha"; then
      if git -C "$dir" checkout --quiet --detach "$sha"; then
        echo "$sha" > "$dir/.pin"
        return 0
      fi
    fi
  fi
  echo "Taking the exact source of $sha for $dir"
  mkdir -p "$dir"
  if curl -sSL --fail -o "$dir/.snapshot.tgz" "https://codeload.github.com/$repo/tar.gz/$sha"; then
    tar -xzf "$dir/.snapshot.tgz" -C "$dir" --strip-components=1
    rm -f "$dir/.snapshot.tgz"
    echo "$sha" > "$dir/.pin"
    return 0
  fi
  return 1
}
for WHAT in "vendor/stable-diffusion.cpp $PIN_IMAGE leejet/stable-diffusion.cpp" \
  "vendor/stable-diffusion.cpp/ggml $PIN_IMAGE_GGML leejet/ggml"; do
  set -- $WHAT
  if [ -d "$1" ]; then
    pin "$1" "$2" "$3" || { echo "Could not put $1 on commit $2 - it is not the source this code expects."; exit 1; }
  fi
done

# the TLS library arrives the same way run.bat brings it: the release archive
# of the pinned version, its sha256 read back before it is unpacked, and the
# version recorded in the .pin beside its CMakeLists. the server build adds
# this tree as a subdirectory, so a box that stands without it configures into
# nothing - which is exactly what UM780 showed the first time this line was
# built on linux.
PIN_TLS=3.6.8
PIN_TLS_SHA256=bc54b818e87f49c0b3281dd85d24adaede0e63b4e822e3fbd49c005a38b9595b
if [ ! -f vendor/mbedtls/CMakeLists.txt ]; then
  echo "Taking the TLS library release archive for mbedtls-$PIN_TLS"
  mkdir -p vendor/mbedtls
  curl -sSL --fail --retry 2 -o vendor/mbedtls/mbedtls-release.tar.bz2 \
    "https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-$PIN_TLS/mbedtls-$PIN_TLS.tar.bz2" || exit 1
  echo "$PIN_TLS_SHA256  vendor/mbedtls/mbedtls-release.tar.bz2" | sha256sum -c - >/dev/null || {
    rm -f vendor/mbedtls/mbedtls-release.tar.bz2
    echo "the TLS archive did not answer the expected sha256 - it is not unpacked."
    exit 1
  }
  tar -xjf vendor/mbedtls/mbedtls-release.tar.bz2 -C vendor/mbedtls --strip-components=1 || exit 1
  rm -f vendor/mbedtls/mbedtls-release.tar.bz2
  echo "mbedtls-$PIN_TLS" > vendor/mbedtls/.pin
fi

# ---- the ssh road's pinned hands (E2) ----
# the clone doors spawn an ssh running git-upload-pack for an ssh address, and
# refuse every such ask in words until it stands. on windows run.bat fetches
# its release zip; here the studio asks nothing of a download - the pinned
# portable OpenSSH is named below, and the transport's honest branch says the
# same words until it does. when this machine takes its hands, they stand flat
# under vendor/openssh where sshBinDir reads them, on the release run.bat
# names: PIN_SSH=10.4.0.0p1-Preview (the spike's build).
if [ ! -f vendor/openssh/ssh ]; then
  echo "The ssh road has no pinned hands here yet: vendor/openssh/ssh is owed (run.bat names the release)." >&2
fi

# ---- the one incremental path every entry point shares ----
# build-rel.sh names these same values for its native targets, in the same
# roots, and the image engine carries the configure-time guard for the pinned
# rsqrt wrapper. Naming one shape in both entry points is what keeps a shared
# tree incremental: the other shape's flags would change every compile command,
# and a rebuild of unchanged code is a defect.
#
# What the instruction-set options mean here, measured on the pinned engine and
# its ggml rather than assumed: they are a claim made to the compiler, not a
# promise the library keeps at run time. -DGGML_AVX512=ON hands -mavx512f and
# friends to every object of the cpu backend (ggml/src/ggml-cpu/CMakeLists.txt),
# the compiler then writes zmm instructions wherever it finds them worth using -
# including in code no dispatch guard stands over, and ggml_cpu_has_avx512()
# answers from the very macro those flags set (ggml-cpu.c), so a library built
# that way cannot discover later that the chip it landed on has none. What
# follows is a process dying of SIGILL at its first touch of the engine: seen
# 2026-11-27 on a kaby lake nuc (avx2, no avx512) as `trap invalid opcode ... in
# libstable-diffusion-cpu.so` at a constant-store inside ggml_cpu_init. The one
# ggml setting that does dispatch on the chip is GGML_CPU_ALL_VARIANTS, and it
# wants GGML_BACKEND_DL - backends as separate libraries this project does not
# stage; until that road is taken, a shape says what it claims and its library
# is named for it. So the plain cpu shape asks for avx2, fma and f16c - the
# floor every machine of this fleet stands on - and nothing above it, and the
# avx512 claim lives in the cpu-avx512 shape, whose library the loader hands
# only to a chip that says yes.
NOVEC_FLAGS="-DGGML_NATIVE=OFF -DGGML_OPENMP=OFF -DGGML_AVX=ON -DGGML_AVX2=ON -DGGML_FMA=ON -DGGML_F16C=ON -DGGML_AVX512=OFF -DGGML_AVX512_VBMI=OFF -DGGML_AVX512_VNNI=OFF -DGGML_AVX512_BF16=OFF"
SD_AVX512_FLAGS="-DGGML_NATIVE=OFF -DGGML_OPENMP=OFF -DGGML_AVX=ON -DGGML_AVX2=ON -DGGML_FMA=ON -DGGML_F16C=ON -DGGML_AVX512=ON -DGGML_AVX512_VBMI=ON -DGGML_AVX512_VNNI=ON -DGGML_AVX512_BF16=ON"
COMMON="-DCMAKE_BUILD_TYPE=Release"
AVX512_WANT=OFF
if [ "$ENGINE_SHAPE" = cpu-avx512 ]; then
  COMMON="$COMMON $SD_AVX512_FLAGS"
  AVX512_WANT=ON
else
  COMMON="$COMMON $NOVEC_FLAGS"
fi
OLS_GUARD="-DCMAKE_PROJECT_INCLUDE_BEFORE=$PWD/src/ols-sage-guard.cmake"

# ---- build the image engine: asked for on every run, as run.bat asks it ----
# an engine root that stands here is asked for anything new on every run, the
# way the src build below is asked: the engine's sources belong to this tree,
# and an edit no build ever reads is an edit that never happened. cmake answers
# that ask with a no-op when nothing changed, so asking costs the seconds it
# takes to look. This is the same ask build-rel.bat makes of the same root, so
# neither entry point leaves the other a tree it must build again from scratch.
# What is not asked twice is the configure, and the shape checks decide whether
# the tree that stands is the tree this shape wants at all.
if [ -f "$SD_BUILD/CMakeCache.txt" ]; then
  # a tree that stands here was configured for one backend; asking this shape
  # of another is a full rebuild, so it is refused before cmake is asked
  # anything at all.
  check_shape "$SD_BUILD/CMakeCache.txt" GGML_VULKAN "OFF"
  # the instruction-set claim this shape makes, asked of the tree that stands:
  # a tree built without avx512 is not the tree the cpu-avx512 shape wants, and
  # the other way round, and neither is fixed by nursing it - see the flags
  # above for why a flipped claim is a different library, not a rebuild.
  check_shape "$SD_BUILD/CMakeCache.txt" GGML_AVX512 "$AVX512_WANT"
  if [ "$ENGINE_SHAPE" = cuda ]; then
    check_shape "$SD_BUILD/CMakeCache.txt" CMAKE_CUDA_ARCHITECTURES "$CUDA_ARCH_LIST"
    check_shape "$SD_BUILD/CMakeCache.txt" GGML_CUDA "ON"
  else
    check_shape "$SD_BUILD/CMakeCache.txt" GGML_CUDA "OFF"
  fi
fi
if [ "${SHAPE_RESTATED:-0}" = 1 ]; then
  # the one clearing this path does, and only on the word above: an engine
  # tree whose claim this shape does not want holds nothing this run may keep.
  rm -rf "$SD_BUILD"
fi
if [ ! -f "$SD_BUILD/.configured-$PIN_IMAGE" ]; then
  if [ "$ENGINE_SHAPE" = cuda ]; then
    echo "Building the image engine as a shared library with CUDA compute, kernels for sm_70, sm_75, sm_86"
    cmake -S vendor/stable-diffusion.cpp -B "$SD_BUILD" \
      $COMMON "$CUDA_FLAGS_OPT" $OLS_GUARD \
      -DSD_BUILD_SHARED_LIBS=ON -DSD_BUILD_EXAMPLES=OFF -DSD_BUILD_CLI=OFF -DSD_BUILD_TEST=OFF \
      -DGGML_CUDA=ON -DGGML_VULKAN=OFF \
      -DCMAKE_CUDA_COMPILER=$NVCC "-DCMAKE_CUDA_ARCHITECTURES=$CUDA_ARCH_LIST"
  else
    echo "Building the image engine as a shared library on the cpu alone"
    cmake -S vendor/stable-diffusion.cpp -B "$SD_BUILD" \
      $COMMON $OLS_GUARD \
      -DSD_BUILD_SHARED_LIBS=ON -DSD_BUILD_EXAMPLES=OFF -DSD_BUILD_CLI=OFF -DSD_BUILD_TEST=OFF \
      -DGGML_CUDA=OFF -DGGML_VULKAN=OFF
  fi
  # the stamp names what it records: the configure this file stands for, not a
  # build, which is asked again every run.
  touch "$SD_BUILD/.configured-$PIN_IMAGE"
fi
cmake --build "$SD_BUILD" -j "$JN" || exit 1

echo ""

# ---- build open-local-services: configure once, then a quick incremental build ----
# this is the development path: the build names its version dev, and the binary
# carries the moment the compiler ran beside that word. build-rel.sh is the one
# that names a commit, and only after it checked the tree.
# the server links no engine and no backend: one server build answers for every
# machine of this platform, and the engine library beside it is what differs.
if [ ! -f "$API_BUILD/CMakeCache.txt" ]; then
  echo "Configuring the open-local-services build"
  cmake -S src -B "$API_BUILD" \
    -DCMAKE_BUILD_TYPE=Release -DOLS_VERSION=dev || exit 1
else
  # the cache can name a version another path put in it, and it can still carry
  # the names of the days the server linked the engine; this path reads what
  # the cache answers with and touches the cache only when it answers
  # differently: configuring marks everything the builders hold, and a tree
  # already naming this very version needs no touch at all. the names are read
  # before the colon - the cache types these defines as it likes, only the
  # name is stable.
  CACHE_VERSION=$(sed -n 's/^OLS_VERSION[^=]*=\(.*\)/\1/p' "$API_BUILD/CMakeCache.txt" | head -n 1)
  NEEDS_RENAME=0
  if [ "$CACHE_VERSION" != "dev" ]; then NEEDS_RENAME=1; fi
  if grep -q '^OLS_SD_BUILD' "$API_BUILD/CMakeCache.txt" 2>/dev/null; then NEEDS_RENAME=1; fi
  # a configure that died partway leaves its cache standing with nothing
  # generated beside it - UM780's first meeting of this line left exactly
  # that, and an incremental build then asks a tree for its Makefile and stops.
  # cmake_install.cmake is what every generator writes when it generates; a
  # cache without it is a half a tree, and gets restated, not nursed.
  if [ ! -f "$API_BUILD/cmake_install.cmake" ]; then
    echo "The build cache stands without generated build files - a configure died partway. Restating it."
    NEEDS_RENAME=1
  fi
  if [ "$NEEDS_RENAME" != 0 ]; then
    # the version is a name the link answers with, so restating it re-links
    # what the tree holds; nothing else about this build is a shape any more -
    # the engine library beside it carries all of that.
    echo "The build cache names another version or the old engine link; restating it"
    cmake -S src -B "$API_BUILD" \
      -DCMAKE_BUILD_TYPE=Release -DOLS_VERSION=dev || exit 1
  fi
fi
echo "Checking for anything new in src and rebuilding it if needed"
if ! cmake --build "$API_BUILD" -j "$JN"; then
  # A build that does not finish says so and stops here: its cache already
  # answers with these very names, read above, so nothing about the tree's
  # shape is in question and no one-file mistake can throw the whole tree
  # away. A scratch build is made by hand - delete the tree, and this path
  # configures it again above.
  echo "The build did not finish. The tree keeps its shape and its cache."
  exit 1
fi

# ---- the build above succeeded, so a fresh binary exists: stop any running
# server right here, right before its binary is replaced.
# A failed build exited long before this point, so downtime stays tiny. ----
stop_running() {
  # %1 = process name, %2 = what to say when the stop belongs to someone else
  PIDS=$(pidof "$1" 2>/dev/null) || return 0
  [ -n "$PIDS" ] || return 0
  # A server that stands at the manager's level is root's to stop; a plain
  # person is told so rather than left staring at a failed kill. The probe is
  # a real no-password ask (sudo -n true), never sudo -n -v: the timestamp
  # check answers the asking rule even where the policy says NOPASSWD, and a
  # box that would run the kill is left thinking it would not.
  KILL="kill"
  if [ "$(id -u)" -ne 0 ] && command -v sudo >/dev/null 2>&1 && sudo -n true >/dev/null 2>&1; then
    KILL="sudo -n kill"
  fi
  if $KILL $PIDS 2>/dev/null; then
    echo "$2"
  else
    echo "$1 is running under a manager this user cannot reach; the new binary"
    echo "is in place but still not the one answering. Restart it through that"
    echo "manager (for a systemd unit: sudo systemctl restart <unit>) before testing."
  fi
}
stop_running open-local-services "Stopping the running open-local-services to replace its binary."

# ---- the engine library the server opens, named for its shape ----
# the server links no engine: the library beside it is what draws pictures,
# opened by name at start. this box's engine goes beside the server under the
# name the server asks it by.
ENGINE_FILE=$(find "$SD_BUILD" -maxdepth 3 -name 'libstable-diffusion.so*' 2>/dev/null |
  grep -v '\.so\.' | head -n 1)
[ -n "$ENGINE_FILE" ] || ENGINE_FILE=$(find "$SD_BUILD" -maxdepth 3 -name 'libstable-diffusion.so.*' 2>/dev/null | head -n 1)
if [ -z "$ENGINE_FILE" ]; then
  echo "The engine build left no libstable-diffusion.so under $SD_BUILD - something went wrong earlier." >&2
  exit 1
fi
echo "Engine library: $ENGINE_FILE, standing as libstable-diffusion-$ENGINE_SHAPE.so"

# ---- install into the runtime tree when this tree is build input only ----
if [ "$RUNTIME" != "$PWD" ]; then
  echo "Installing the server and its engine library into $RUNTIME"
  INSTALLED_ELSEWHERE=1
  mkdir -p "$RUNTIME/src"
  cp -f "$API_BUILD/open-local-services" "$RUNTIME/src/open-local-services"
  cp -f "$ENGINE_FILE" "$RUNTIME/src/libstable-diffusion-$ENGINE_SHAPE.so"
else
  # a server the service manager restarts on its own keeps the old file busy
  # while we replace it: write next to it, then rename, which settles the
  # directory entry whatever is executing the old one.
  cp -f "$API_BUILD/open-local-services" src/open-local-services.new
  mv -f src/open-local-services.new src/open-local-services
  cp -f "$ENGINE_FILE" "src/libstable-diffusion-$ENGINE_SHAPE.so.new"
  mv -f "src/libstable-diffusion-$ENGINE_SHAPE.so.new" "src/libstable-diffusion-$ENGINE_SHAPE.so"
fi

# ---- stage for /api/update ----
# bin/ is the room a running server reads when it is handed over: what stands
# here named for the program - the binary and every engine library - moves
# into place while the server is down. one staged set per box: the shape just
# compiled is the shape this box runs.
mkdir -p bin
cp -f "$API_BUILD/open-local-services" bin/open-local-services
cp -f "$ENGINE_FILE" "bin/libstable-diffusion-$ENGINE_SHAPE.so"

if [ "$MODE" = build ]; then
  echo ""
  echo "Built. Start the server with: run.sh"
  exit 0
fi

# A tree that is build input only has no server of its own to start: the binary
# it just made went to the tree that asked for it, and whatever stands beside
# this tree is what an earlier build left behind. Starting that would answer
# with old code while the lines above say the new one was just installed.
if [ "${INSTALLED_ELSEWHERE:-}" = 1 ]; then
  echo ""
  echo "This tree is build input only: the server and its libraries stand in"
  echo "$RUNTIME, and that tree is the one to start from."
  exit 0
fi

# ---- start ----
echo ""
echo "Starting open-local-services"
echo "The server names the address it answers at on the lines below: the port it"
echo "asks for. If that port is not free, this script reports the refusal; it never"
echo "rolls the server over to another port."
echo "It links to the home page and the image studio. The server"
echo "runs in this terminal; close it to stop."
echo ""
echo "The first thing the server does is stand at the level this machine's manager"
echo "works at - root, here through sudo - because a boot entry, a unit enabled, a"
echo "task ended are the manager's to take, and a plain person's process is refused"
echo "by its own machine. The line it prints names which level it stands at. A"
echo "machine that cannot reach root this way still runs every door; only the"
echo "manager asks answer differently, and OLS_ELEVATE=off declines the ask."
echo ""
# the one ggml lives inside the engine library beside the server; the server
# itself carries no backend at all, so no copy can register the same device
# twice. the variable is kept for the engine's own ggml: a crash still prints
# its own words, in the same line the failing check names, and the server's
# crash handler stays the single one.
export GGML_NO_BACKTRACE=1
exec src/open-local-services
