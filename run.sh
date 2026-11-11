#!/bin/sh
# open-local-services entry point for Linux.
# One script does the whole onboarding: it discovers the tooling this box has
# (cmake, the newest CUDA toolkit with an nvcc, this GPU's compute capability),
# builds whatever is missing - vendor engines, voice models, open-local-services - and
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
# rebuild a whole tree from scratch without it.
for opt in "$@"; do
  case "$opt" in
    build) MODE=build ;;
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
if [ ! -f api/main.cpp ]; then
  if [ ! -f api/open-local-services ]; then
    echo "This tree has no C++ sources and no api/open-local-services to start."
    echo "Transfer a source tree, or set OLS_RUNTIME_DIR to install into this tree."
    exit 1
  fi
  if pidof open-local-services >/dev/null 2>&1; then
    echo "Stopping the running open-local-services before starting this one."
    kill $(pidof open-local-services)
  fi
  echo "Starting open-local-services. It names the address it answers at as it starts."
  # two ggml copies share this process: the voice engine's, linked into the
  # binary, and the one whose plugin libraries sit beside it. ggml stops a
  # process that installs its crash handler twice - before it could listen - so
  # no copy here installs one, and the server starts. a crash still prints its
  # own words, in the same line the failing check names.
  export GGML_NO_BACKTRACE=1
  exec api/open-local-services
fi

# ---- discover the tooling ----
if ! command -v cmake >/dev/null 2>&1; then
  echo "cmake was not found on this machine. Install it, then run this again."
  exit 1
fi

# /usr/local/cuda is the newest toolkit, which may carry no nvcc for this GPU;
# take the newest CUDA toolkit that actually has one.
NVCC=""
for D in /usr/local/cuda-*/bin/nvcc /usr/local/cuda/bin/nvcc; do
  [ -x "$D" ] && NVCC="$D"
done
if [ -z "$NVCC" ]; then
  echo "No CUDA toolkit with an nvcc found under /usr/local - this build needs one."
  exit 1
fi
# The GPU in this box decides the compute capability, unless one is stated: the
# environment and .env come first, so a box whose nvidia-smi cannot name the
# field still builds for the card its owner named. With neither, CMake probes.
CUDA_ARCH=$(envval CUDA_ARCH)
if [ -z "$CUDA_ARCH" ]; then
  CUDA_ARCH=$(nvidia-smi --query-gpu=compute_capability --format=csv,noheader 2>/dev/null |
    head -n 1 | tr -cd '0-9')
fi
ARCH_OPTS=
if [ -n "$CUDA_ARCH" ]; then
  ARCH_OPTS="-DCMAKE_CUDA_ARCHITECTURES=$CUDA_ARCH"
  echo "GPU compute capability: $CUDA_ARCH"
else
  echo "GPU capability not queryable; CMake will probe the device"
fi
JN=$(nproc)
echo "CUDA compiler: $NVCC, compute capability: $CUDA_ARCH, parallel jobs: $JN"

RUNTIME=${OLS_RUNTIME_DIR:-.}
# the comparison below is against an absolute path, so settle this one first
if [ -d "$RUNTIME" ]; then RUNTIME=$(cd "$RUNTIME" && pwd); fi
mkdir -p api/vendor "$RUNTIME/weights" "$RUNTIME/data/voices" "$RUNTIME/data/tts/outputs" \
  "$RUNTIME/data/tts/scripts" "$RUNTIME/data/voice-presets"

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

# ---- the roots of this machine's shape, named like build-rel.sh names them ----
# one build tree holds one backend and one card generation, so the development
# path takes the same card-named roots the release path uses for that shape —
# a CUDA box develops in the -c<arch> roots, and its unqualified roots belong
# to the cpu-only shape alone. Two entry points naming one root with the same
# defines is what keeps the build incremental: a flipped define changes every
# compile command in the tree and rebuilds an engine for hours over and over.
SFX=""
if [ -n "$CUDA_ARCH" ]; then SFX="-c$CUDA_ARCH"; else SFX="-cprobe"; fi
SD_BUILD="api/vendor/stable-diffusion.cpp/build$SFX"
API_BUILD="api/build$SFX"

# the exact vendor commits this code is written against. the release gate is the
# same test result on both machines, and that only holds while both build the
# same engine source - so the pins are named here, once, and checked below.
# a maintainer who moves a pin moves it in run.bat to match.
PIN_VOICE=41bb3f0ffb6b46b7a3bb3c02fcea52900d88001b        # 0xShug0/audio.cpp (inside the api binary)
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
      echo "fresh-full-rebuild was given: restating $SHAPE_NAME on this tree."
      return 0
    fi
    refuses_a_full_rebuild
  fi
  return 0
}
PIN_VOICE=41bb3f0ffb6b46b7a3bb3c02fcea52900d88001b        # 0xShug0/audio.cpp (inside the api binary)
PIN_IMAGE=b13a48219d5cdf408edcb93da6cd69b612ed03d3        # leejet/stable-diffusion.cpp
PIN_IMAGE_GGML=dc998771e700d49bbdf9a39cede22bb5b3311767   # leejet/ggml (image engine)

# ---- fetch missing vendor sources ----
if [ ! -f api/vendor/nlohmann/json.hpp ]; then
  echo "Fetching the json header"
  mkdir -p api/vendor/nlohmann
  curl -sSL --fail -o api/vendor/nlohmann/json.hpp \
    https://raw.githubusercontent.com/nlohmann/json/develop/single_include/nlohmann/json.hpp || exit 1
fi
if [ ! -d api/vendor/audio.cpp ]; then
  echo "Fetching the voice engine source"
  git clone --depth 1 https://github.com/0xShug0/audio.cpp api/vendor/audio.cpp || exit 1
fi
if [ ! -d api/vendor/stable-diffusion.cpp ]; then
  echo "Fetching the image engine source"
  git clone --depth 1 https://github.com/leejet/stable-diffusion.cpp api/vendor/stable-diffusion.cpp || exit 1
fi
for SUB in api/vendor/stable-diffusion.cpp; do
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
for WHAT in "api/vendor/audio.cpp $PIN_VOICE 0xShug0/audio.cpp" \
  "api/vendor/stable-diffusion.cpp $PIN_IMAGE leejet/stable-diffusion.cpp" \
  "api/vendor/stable-diffusion.cpp/ggml $PIN_IMAGE_GGML leejet/ggml"; do
  set -- $WHAT
  if [ -d "$1" ]; then
    pin "$1" "$2" "$3" || { echo "Could not put $1 on commit $2 - it is not the source this code expects."; exit 1; }
  fi
done

# ---- the voice engine has no stage of its own ----
# it is linked into open-local-services, so the api build below configures and builds it
# in its own tree beside everything else there: one ggml for it, built with the
# code that calls it, and nothing beside the server that could be a different
# build of the same engine. its model files are placed by the pages, not
# downloaded here - a family this machine has no files for is named as such.

# ---- the one incremental path every entry point shares ----
# build-rel.sh names these same values for its native target, in the same
# roots: the cpu backend of every engine is built for the baseline of its
# architecture with run-time feature detection, never for the machine that
# happened to compile it, and the image engine carries the configure-time
# guard for the pinned rsqrt wrapper. Naming one shape in both entry points
# is what keeps a shared tree incremental: the other shape's flags would
# change every compile command, and a rebuild of unchanged code is a defect.
COMMON="-DCMAKE_BUILD_TYPE=Release -DGGML_NATIVE=OFF -DGGML_OPENMP=OFF -DGGML_AVX512=ON -DGGML_AVX512_VBMI=ON -DGGML_AVX512_VNNI=ON -DGGML_AVX512_BF16=ON -DGGML_FMA=ON -DGGML_F16C=ON"
OLS_GUARD="-DCMAKE_PROJECT_INCLUDE_BEFORE=$PWD/api/ols-sage-guard.cmake"

# ---- build the image engine, skipping any part already built ----
if [ ! -f "$SD_BUILD/.built-$PIN_IMAGE" ]; then
  # a tree that stands here was configured for one backend and one card; asking
  # this shape of another is a full rebuild, so it is refused before cmake is
  # asked anything at all.
  if [ -f "$SD_BUILD/CMakeCache.txt" ]; then
    check_shape "$SD_BUILD/CMakeCache.txt" CMAKE_CUDA_ARCHITECTURES "$CUDA_ARCH"
    check_shape "$SD_BUILD/CMakeCache.txt" GGML_CUDA "ON"
    check_shape "$SD_BUILD/CMakeCache.txt" GGML_VULKAN "OFF"
  fi
  echo "Building the image engine as a shared library with CUDA compute"
  cmake -S api/vendor/stable-diffusion.cpp -B "$SD_BUILD" \
    $COMMON "$CUDA_FLAGS_OPT" $OLS_GUARD \
    -DSD_BUILD_SHARED_LIBS=ON -DSD_BUILD_EXAMPLES=OFF -DSD_BUILD_CLI=OFF -DSD_BUILD_TEST=OFF \
    -DGGML_CUDA=ON -DGGML_VULKAN=OFF \
    -DCMAKE_CUDA_COMPILER=$NVCC $ARCH_OPTS
  cmake --build "$SD_BUILD" -j "$JN" || exit 1
  touch "$SD_BUILD/.built-$PIN_IMAGE"
fi

echo ""

# ---- build open-local-services: configure once, then a quick incremental build ----
# this is the development path: the build names its version dev, and the binary
# carries the moment the compiler ran beside that word. build-rel.sh is the one
# that names a commit, and only after it checked the tree.
if [ ! -f "$API_BUILD/CMakeCache.txt" ]; then
  echo "Configuring the open-local-services build"
  cmake -S api -B "$API_BUILD" \
    $COMMON "$CUDA_FLAGS_OPT" -DCMAKE_CUDA_COMPILER=$NVCC $ARCH_OPTS \
    "-DOLS_SD_BUILD=$SD_BUILD" -DOLS_VERSION=dev || exit 1
else
  # the cache can name a version or an image library another path put in it,
  # so this path reads what the cache answers with and touches the cache only
  # when it answers with another path's names: configuring marks everything
  # the builders hold, and a tree already naming this very version and this
  # very image library needs no touch at all. the names are read before the
  # colon - the cache types these defines as it likes, only the name is stable.
  CACHE_VERSION=$(sed -n 's/^OLS_VERSION[^=]*=\(.*\)/\1/p' "$API_BUILD/CMakeCache.txt" | head -n 1)
  CACHE_SD=$(sed -n 's/^OLS_SD_BUILD[^=]*=\(.*\)/\1/p' "$API_BUILD/CMakeCache.txt" | head -n 1)
  if [ "$CACHE_VERSION" != "dev" ] || [ "$CACHE_SD" != "$SD_BUILD" ]; then
    # the version and the image library are names the link answers with, and
    # restating them re-links what the tree holds; the card generation and the
    # backend are the shape every object was compiled for, and those are asked
    # before cmake is - a tree of another shape would be compiled again in full.
    check_shape "$API_BUILD/CMakeCache.txt" CMAKE_CUDA_ARCHITECTURES "$CUDA_ARCH"
    echo "The api cache names another version or image library; restating it"
    cmake -S api -B "$API_BUILD" \
      $COMMON "$CUDA_FLAGS_OPT" -DCMAKE_CUDA_COMPILER=$NVCC $ARCH_OPTS \
      "-DOLS_SD_BUILD=$SD_BUILD" -DOLS_VERSION=dev || exit 1
  fi
fi
echo "Checking for anything new in api and rebuilding it if needed"
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
  # person is told so rather than left staring at a failed kill.
  KILL="kill"
  if [ "$(id -u)" -ne 0 ] && command -v sudo >/dev/null 2>&1 && sudo -n -v >/dev/null 2>&1; then
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

# ---- install into the runtime tree when this tree is build input only ----
if [ "$RUNTIME" != "$PWD" ]; then
  echo "Installing the server and its libraries into $RUNTIME"
  mkdir -p "$RUNTIME/api"
  cp -f "$API_BUILD/open-local-services" "$RUNTIME/api/open-local-services"
  ldd "$API_BUILD/open-local-services" 2>/dev/null | sed 's/.*=> \([^ ]*\.so[^ ]*\).*/\1/' |
    while read SO; do
      case "$SO" in "$PWD"/*) cp -f "$SO" "$RUNTIME/api/" ;; esac
    done
else
  # a server the service manager restarts on its own keeps the old file busy
  # while we replace it: write next to it, then rename, which settles the
  # directory entry whatever is executing the old one.
  cp -f "$API_BUILD/open-local-services" api/open-local-services.new
  mv -f api/open-local-services.new api/open-local-services
fi

# The voice engine reads its phoneme tables from a directory beside the server,
# and on Linux nothing else puts them there. The build makes them under _deps;
# the run needs them next to the binary it is about to start - the step run.bat
# does on Windows. Without it a speak job answers "Expected an espeak-ng-data
# directory containing phontab", which is what a real speak job on UM780 said on
# 2026-10-28 while the reading engine on the same box worked fine.
ESPEAK_MADE="$API_BUILD/_deps/espeak-static/build/espeak-ng-data"
if [ -f "$ESPEAK_MADE/phontab" ]; then
  for WHERE in "$RUNTIME/api" api; do
    [ -d "$WHERE" ] || continue
    [ -f "$WHERE/espeak-ng-data/phontab" ] && continue
    echo "Placing the phoneme tables where the voice engine looks for them: $WHERE"
    mkdir -p "$WHERE/espeak-ng-data"
    cp -a "$ESPEAK_MADE"/. "$WHERE/espeak-ng-data/"
  done
fi

if [ "$MODE" = build ]; then
  echo ""
  echo "Built. Start the server with: run.sh"
  exit 0
fi

# ---- start ----
echo ""
echo "Starting open-local-services"
echo "The server names the address it answers at on the lines below: the port it"
echo "asks for. If that port is not free, this script reports the refusal; it never"
echo "rolls the server over to another port."
echo "It links to the home page, the image studio, and the voice studio. The server"
echo "runs in this terminal; close it to stop."
echo ""
echo "The first thing the server does is stand at the level this machine's manager"
echo "works at - root, here through sudo - because a boot entry, a unit enabled, a"
echo "task ended are the manager's to take, and a plain person's process is refused"
echo "by its own machine. The line it prints names which level it stands at. A"
echo "machine that cannot reach root this way still runs every door; only the"
echo "manager asks answer differently, and OLS_ELEVATE=off declines the ask."
echo ""
# two ggml copies share this process: the voice engine's, linked into the
# binary, and the one whose plugin libraries sit beside it. ggml stops a
# process that installs its crash handler twice - before it could listen - so
# no copy here installs one, and the server starts. a crash still prints its
# own words, in the same line the failing check names.
export GGML_NO_BACKTRACE=1
exec api/open-local-services
