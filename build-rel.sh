#!/bin/sh
# open-local-services release build for Linux.
#
# run.sh builds and runs the server on this box, in the development way: its
# version is dev, and the binary carries the moment the compiler ran. this
# script builds the portable artifacts that get committed to the repo, one
# folder per target:
#
#   Release/linux-x86_64/   native on this box: the server with the cpu engine
#                           beside it, since this script names no Vulkan
#                           library for linux
#   Release/linux-x86_64-cuda/
#                           the same server with one CUDA engine beside it -
#                           kernels for every card generation this project
#                           supports: sm_70 (a V100), sm_75 (an RTX 20 series,
#                           the card in the laptop down the network) and sm_86
#                           (an RTX 30 series). the loader inside the library
#                           picks each card's own kernels when the server
#                           opens it, so one library answers every one of
#                           those machines; the windows side builds the same.
#   Release/linux-arm64/    cross-built for 64-bit arm with the aarch64
#                           toolchain (installed through the package manager if
#                           it is not here; see the note at the bottom of the
#                           summary this script prints)
#
# what the artifacts guarantee, and what they do not:
#   - nothing is generated in the folder this script sits in, and no source is
#     copied or duplicated: the engines are built in the vendor trees this code
#     keeps. the roots are named for each backend: the cpu engine takes the
#     unqualified roots - the same ones run.sh develops in - the cuda engine
#     takes the -cuda roots beside them, and the cross engine takes the
#     -arm64 roots, because one build tree holds one backend, never two
#     shapes one after the other. the server build is shapeless: it links no
#     engine, so one native tree answers for every native target.
#   - the version every artifact here carries is the commit its code stands on,
#     which this script reads only after checking that the code is committed.
#   - the cpu backend is built for the baseline ISA of its architecture and
#     chooses avx, avx2 or avx512 at run time from what the cpu reports. the
#     machine that built an artifact is therefore not the machine that has to
#     run it, and a cpu without avx512 is not broken by an artifact compiled on
#     one that has it.
#   - the server links no engine and no ggml of its own. the image engine
#     beside it is the one library that carries ggml, and the server opens it
#     by name when it starts - stable-diffusion-cpu.so or its cuda twin -
#     which is what keeps one process from ever holding two ggmls. the same
#     rule run.sh and src/CMakeLists.txt follow.
#   - the arm64 artifact carries the cpu backend only: this box has no CUDA
#     runtime for arm64 and no Vulkan library for it, and that is stated rather
#     than left to be discovered at run time.
#
# the trees are shared: building the release way leaves the engine trees built
# with the release settings, and the next run.sh build puts them back the
# development way. nothing is wrong about that - each entry point states the
# kind of binary it makes.
#
# how a release works here, in plain words - there is no release server, no
# pipeline, and no version numbers, and that is the whole design:
#   1. the code is committed first. the release refuses an uncommitted tree,
#      because a released binary that carries a commit older than its own
#      code would lie about what it is.
#   2. this script builds each shape and stamps the binary with that commit.
#      the commit hash is the version - the only version this project has.
#   3. each finished folder is written beside the source as one tar.gz,
#      together with its sha256 in Release/SHA256SUMS-linux.txt, and the archives
#      are committed. the repository is the release: what is on a machine is
#      what the tree says it is, and nothing else publishes anything.
#   4. a machine that compiles nothing takes one archive, opens it where it
#      stands, and runs the binary inside - the cuda runtime it imports
#      travels beside it, read off what the loader really asks for. weights/
#      and data/ folders and a .env beside the binary are the same folders
#      and file the development tree uses.
#   5. the cpu backend of every artifact is the baseline of its architecture
#      with run-time detection on top: an artifact built on a machine with
#      avx512 runs untouched on one that has never seen it. the glibc floor
#      of each artifact is read off the binaries and printed beside them -
#      which is honest only because linux artifacts are built on the same
#      ubuntu release as the machines that will run them.
#
# usage: build-rel.sh            every linux target this box can build
#        build-rel.sh x86        the cpu one only
#        build-rel.sh arm64      the cross-built one only
#        build-rel.sh cuda       the CUDA target only - one engine for every
#                             supported generation; cuda=70 and the like still
#                             arrive and narrow that engine to the generations
#                             named (what the toolkit cannot write is said)
cd "$(dirname "$0")"

REL=Release
mkdir -p "$REL"

# a released version is the commit its code stands on, so the code has to be
# committed before it can be released: an uncommitted tree would carry a version
# that is not the code inside the binary.
CODE_STATE=$(git status --porcelain -- src webui 2>/dev/null)
if [ -n "$CODE_STATE" ]; then
  echo "the code in src and webui is not committed - it names the files that"
  echo "changed. a release version is a commit, so this build is refused. commit"
  echo "the code, then build again."
  git status --short -- src webui 2>/dev/null | head -20
  exit 1
fi
CODE_VERSION=$(git rev-parse HEAD 2>/dev/null)
if [ -z "$CODE_VERSION" ]; then
  echo "this tree is not a git tree: the release would carry no version, which"
  echo "is what the pages would then say. commit the code in a git tree first."
  exit 1
fi
echo "the version every artifact here carries: $CODE_VERSION"
VERSION7=$(echo "$CODE_VERSION" | cut -c1-7)

# each run writes the archives of the artifacts and the hashes of those
# archives: one file per target to push over the network, and one file that
# says what the bytes of every one of them are.
rm -f Release/SHA256SUMS-linux.txt

# the exact vendor commits this code is written against - the same names run.sh
# carries, so an artifact is the source the running server runs.
PIN_IMAGE=b13a48219d5cdf408edcb93da6cd69b612ed03d3
PIN_IMAGE_GGML=dc998771e700d49bbdf9a39cede22bb5b3311767

# the shape of a tree that stands here is asked before it is touched: the names
# below decide the compile commands of every object in that tree, so a target
# that asks another of them would make cmake rewrite those commands and ask the
# compiler for the whole tree again - a full rebuild, which this path refuses to
# do silently. the caller who means it says fresh-full-rebuild; run.sh carries
# the same check, word for word, so both entry points ask one shape.
read_cache() {
  # $1 = cache file, $2 = define name; the cache types these with whatever
  # suffix it likes, so only the name before the first = is matched.
  sed -n "s/^$2[^=]*=\(.*\)/\1/p" "$1" | head -n 1
}
refuses_a_full_rebuild() {
  echo "[$TARGET] this tree holds the shape of another build: its cache names"
  echo "  $SHAPE_NAME = $SHAPE_HAS, and this target asks for $SHAPE_WANT."
  echo "  restating it would recompile everything the tree holds. nothing here"
  echo "  clears a tree; to rebuild that shape on purpose run again with"
  echo "  fresh-full-rebuild, or build the shape this tree already holds."
  return 1
}
check_shape() {
  # $1 = cache file, $2 = define name, $3 = the value this shape asks for.
  # a name this shape does not ask for belongs to the other backend alone, and
  # a name no tree of this shape ever carried is left alone too.
  SHAPE_CACHE=$1
  SHAPE_NAME=$2
  SHAPE_WANT=$3
  [ -n "$SHAPE_WANT" ] || return 0
  SHAPE_HAS=$(read_cache "$SHAPE_CACHE" "$SHAPE_NAME")
  [ -n "$SHAPE_HAS" ] || return 0
  if [ "$SHAPE_HAS" != "$SHAPE_WANT" ]; then
    if [ "$FRESH_FULL" = 1 ]; then
      echo "[$TARGET] fresh-full-rebuild was given: restating $SHAPE_NAME."
      return 0
    fi
    refuses_a_full_rebuild
    return 1
  fi
  return 0
}

# which targets to build: nothing named means every target this box can
# honestly build - the cpu ones always, a card generation only when the
# toolkit above can write for it.
WANT=""
WANT_CUDA=""
CUDA_ASKED=""
FRESH_FULL=
for opt in "$@"; do
  case "$opt" in
    x86|x86_64) WANT="cpu $WANT" ;;
    arm64|aarch64) WANT="arm64 $WANT" ;;
    all) WANT="cpu arm64" ; WANT_CUDA="70 75 86" ;;
    cuda) WANT_CUDA="70 75 86" ; CUDA_ASKED=1 ;;
    cuda=*) WANT_CUDA="${WANT_CUDA} ${opt#cuda=}" ; CUDA_ASKED=1 ;;
    fresh-full-rebuild | --fresh-full-rebuild) FRESH_FULL=1 ;;
  esac
done
if [ -z "$WANT" ]; then
  if [ -z "$WANT_CUDA" ]; then
    WANT="cpu arm64"
    WANT_CUDA="70 75 86"
  fi
fi

if ! command -v cmake >/dev/null 2>&1; then
  echo "cmake was not found on this machine. Install it, then run this again."
  exit 1
fi
for SUB in vendor/stable-diffusion.cpp; do
  if [ ! -f "$SUB/CMakeLists.txt" ]; then
    echo "This tree has no engine sources. run.sh fetches and pins them first."
    exit 1
  fi
done

# what this box can target
NVCC=""
for D in /usr/local/cuda-*/bin/nvcc /usr/local/cuda/bin/nvcc; do
  [ -x "$D" ] && NVCC="$D"
done
CROSS_GXX=$(command -v aarch64-linux-gnu-g++)
CROSS_GCC=$(command -v aarch64-linux-gnu-gcc)
JN=$(nproc)

# the same bin dir holds cuobjdump: the report below reads the architectures the
# artifacts carry off the toolkit's own copy, so it works when that tool is not
# on the command path.
CU=""
[ -n "$NVCC" ] && CU="$(dirname "$NVCC")/cuobjdump"
[ -x "$CU" ] || CU=""

# the card generations this project supports: the compute 7.0 cards, the
# compute 7.5 cards, and the compute 8.6 cards - one binary per generation.
# what the toolkit here can write for is read off nvcc rather than assumed,
# and a generation it cannot write for is named in the summary rather than
# left out of the run quietly.
CUDA_OK=""
CUDA_SKIPPED=""
CUDA_WANTED=$WANT_CUDA
if [ -z "$CUDA_WANTED" ]; then
  CUDA_WANTED="70 75 86"
fi
if [ -n "$NVCC" ]; then
  HAVE_ARCHS=$("$NVCC" --list-gpu-arch | sed 's/compute_//' | tr '\n' ' ')
  for A in $CUDA_WANTED; do
    case " $HAVE_ARCHS " in
      *" $A "*) CUDA_OK="$CUDA_OK $A" ;;
      *) CUDA_SKIPPED="$CUDA_SKIPPED $A" ;;
    esac
  done
else
  CUDA_SKIPPED="$CUDA_WANTED"
fi
CUDA_OK=$(echo $CUDA_OK)
if [ -n "$CUDA_SKIPPED" ]; then echo "this toolkit cannot write code for the card generations: $CUDA_SKIPPED"; fi
if [ -n "$CUDA_OK" ]; then echo "this box will build the card generations: $CUDA_OK"; fi
# a current glibc declares rsqrtf for the host before nvcc gets its device
# intrinsic, same shim run.sh writes — the same path, so both entry points
# hand a shared tree the exact same -include and nothing is poisoned.
CUDA_SHIM="$PWD/data/.rsqrtf-device-decl.h"
mkdir -p "$PWD/data"
printf '%s\n' '#pragma once' '#ifdef __CUDACC__' \
  'extern "C" __device__ float rsqrtf(float);' '#endif' > "$CUDA_SHIM"

# a cross build cannot ask a cross compiler for the ISA of the machine doing the
# building, and one engine adds exactly that flag to its release flags. the
# engine's own source is not touched - it stays the pinned source it is - so the
# release build calls the cross compiler through a filter that drops that one
# flag and passes every other argument on untouched. the name keeps the
# compiler's own name in it so cmake still reads the compiler as what it is.
cross_filter() {
  # $1 = the real compiler, $2 = the file to write it into
  # the real compiler is baked in here, at generation time: cmake calls this
  # file itself as the compiler, so the compiler's name could never arrive as
  # an argument to it - a wrapper that read $1 as the compiler would exec a
  # compile flag. the here-doc is unquoted so $1 lands as the baked value;
  # every runtime dollar is escaped to survive the writing.
  cat > "$2" <<CROSSFILTER
#!/bin/sh
# the real cross compiler, with -march=native taken out of the list: a cross
# compiler does not know the machine that is building, and that flag names it.
REAL="$1"
n=\$#
while [ "\$n" -gt 0 ]; do
  a=\$1
  shift
  if [ "\$a" = '-march=native' ]; then
    :
  else
    set -- "\$@" "\$a"
  fi
  n=\$((n - 1))
done
exec "\$REAL" "\$@"
CROSSFILTER
  chmod +x "$2"
}

# the file the cross build calls instead of the cross compiler, and the folder
# it stands in. the name is the compiler's own name, so cmake reads the
# compiler as what it is; a build tree is only given a full path to a
# compiler, so this is an absolute path.
CROSS_DIR=$REL/.build/cross-cxx
CROSS_FILTER=""
if [ -n "$CROSS_GXX" ]; then
  mkdir -p "$PWD/$CROSS_DIR"
  cross_filter "$CROSS_GXX" "$PWD/$CROSS_DIR/aarch64-linux-gnu-g++"
  CROSS_FILTER="$PWD/$CROSS_DIR/aarch64-linux-gnu-g++"
fi

build_one() {
  TARGET=$1
  KIND=$2
  CUDA=$3
  OUT=$REL/$TARGET

  rm -rf "$OUT"
  mkdir -p "$OUT"

  # the engine's roots are named for its backend - the cpu engine builds where
  # run.sh develops it, the cuda engine in its own multi-generation tree, a
  # cross engine beside them under its architecture's name - because one build
  # tree holds one backend, never two shapes one after the other. the server
  # build is shapeless: it links no engine, so the same x86_64 tree answers
  # for the cpu target and the cuda target, and only the cross server stands
  # apart.
  SUFFIX=""
  COMP_OPTS=""
  ENGINE_SHAPE=cpu
  if [ "$CUDA" = on ]; then
    SUFFIX="-cuda"
    ENGINE_SHAPE=cuda
  fi
  if [ "$KIND" = cross ]; then
    SUFFIX="-arm64"
    # telling cmake the target architecture is what makes the engine take the
    # arm branches: ggml picks its cpu backend from the target cmake is told
    # about, and a cross compiler left unnamed still reports the host's. the
    # C++ compiler is the filter written above, which stands under the
    # compiler's own name so cmake reads it as that compiler, and takes the one
    # ISA flag an engine adds for the machine doing the building out of the
    # list - a cross compiler does not know that machine.
    COMP_OPTS="-DCMAKE_C_COMPILER=$CROSS_GCC -DCMAKE_CXX_COMPILER=$CROSS_FILTER -DCMAKE_SYSTEM_NAME=Linux -DCMAKE_SYSTEM_PROCESSOR=aarch64"
  fi

  SD=vendor/stable-diffusion.cpp
  SD_BUILD="$SD/build$SUFFIX"
  if [ "$KIND" = cross ]; then API_BUILD="build-arm64"; else API_BUILD="build"; fi
  ENGINE_OUT_NAME="libstable-diffusion-$ENGINE_SHAPE.so"
  # the generations the cuda engine carries, as one semicolon list for cmake:
  # the loader inside the library picks each card's own kernels when the
  # server opens it, so this one file answers every card of these generations.
  CUDA_LIST=$(echo "$CUDA_OK" | tr ' ' ';')

  # the guard that steps the pinned image engine's rsqrt wrapper aside where the
  # box's CUDA header maps that name (see src/ols-sage-guard.cmake) - asked of
  # every image-engine configure, on every box, so the pinned source reads the same
  # way wherever it is compiled
  OLS_GUARD="-DCMAKE_PROJECT_INCLUDE_BEFORE=$(pwd)/src/ols-sage-guard.cmake"

  # the build trees of this target are kept, never cleared: they were named
  # for this target from the start - its own architecture, its own backend -
  # and a release configure asks the same questions every time, so what did
  # not change is not recompiled. a build that rebuilt unchanged code was a
  # defect, and a no-change run of the development path reads 25 seconds
  # against 482 from a tree thrown away. the source trees are untouched.

  # the cpu backend of every engine is built for the floor of its architecture,
  # never for the machine that happened to compile it: GGML_NATIVE=OFF leaves
  # the engines no -march shortcut. What those flags are NOT is a run-time
  # promise. -DGGML_AVX512=ON hands -mavx512f to every object of the cpu
  # backend, the compiler writes zmm instructions wherever it wants them -
  # including where no dispatch guard stands - and ggml_cpu_has_avx512()
  # answers from the macro those flags set, so the built library never finds
  # out that the chip below it has none: it dies of SIGILL at its first touch.
  # Measured 2026-11-27 on a kaby lake nuc building this line as it stood, and
  # the same trap is why the engine's own name says what it claims. The ggml
  # setting that does dispatch on the chip is GGML_CPU_ALL_VARIANTS, which asks
  # for GGML_BACKEND_DL - backends as separate libraries this project does not
  # stage - so an avx512 build is an avx512 artifact, named for it, and the
  # plain artifact asks for avx2, fma and f16c: the floor this fleet stands on.
  COMMON="-DCMAKE_BUILD_TYPE=Release -DGGML_NATIVE=OFF -DGGML_OPENMP=OFF -DGGML_AVX=ON -DGGML_AVX2=ON -DGGML_FMA=ON -DGGML_F16C=ON -DGGML_AVX512=OFF -DGGML_AVX512_VBMI=OFF -DGGML_AVX512_VNNI=OFF -DGGML_AVX512_BF16=OFF"
  # shared libraries find each other by the folder they stand in, not by the
  # folder they were built in: an artifact unpacked on a machine that never
  # held this tree has no build roots to fall back on, so every library gets
  # a run path of its own folder - and every runtime library it needs is
  # carried into that folder beside it, below. the server needs no run path
  # of the engine at all: it opens the library by name when it starts, and
  # the loader looks in the folder the server itself stands in.
  RPATH_SD="-DCMAKE_BUILD_WITH_INSTALL_RPATH=ON -DCMAKE_INSTALL_RPATH=\$ORIGIN"
  RPATH_API="-DCMAKE_BUILD_WITH_INSTALL_RPATH=ON -DCMAKE_INSTALL_RPATH=\$ORIGIN"

  CUDA_FLAG=OFF
  [ "$CUDA" = on ] && CUDA_FLAG=ON

  # the image engine keeps its own ggml, so it is built as a library.
  # a tree that stands here was configured for one backend - and the cuda
  # tree for one set of card generations: this shape is asked of it before
  # cmake is, so a target that asks another of them says so instead of
  # recompiling the tree in full.
  if [ -f "$SD_BUILD/CMakeCache.txt" ]; then
    if [ "$CUDA" = on ]; then
      check_shape "$SD_BUILD/CMakeCache.txt" CMAKE_CUDA_ARCHITECTURES "$CUDA_LIST" || return 1
    fi
    check_shape "$SD_BUILD/CMakeCache.txt" GGML_CUDA "$CUDA_FLAG" || return 1
  fi
  if [ "$CUDA" = on ]; then
    if ! cmake -S "$SD" -B "$SD_BUILD" $COMMON $RPATH_SD $COMP_OPTS $OLS_GUARD \
      -DSD_BUILD_SHARED_LIBS=ON -DSD_BUILD_EXAMPLES=OFF -DSD_BUILD_CLI=OFF -DSD_BUILD_TEST=OFF \
      -DGGML_CUDA=ON -DGGML_VULKAN=OFF \
      "-DCMAKE_CUDA_COMPILER=$NVCC" \
      "-DCMAKE_CUDA_ARCHITECTURES=$CUDA_LIST" \
      "-DCMAKE_CUDA_FLAGS=-include $CUDA_SHIM"; then
      echo "[$TARGET] could not configure the image engine"; return 1
    fi
  elif ! cmake -S "$SD" -B "$SD_BUILD" $COMMON $RPATH_SD $COMP_OPTS $OLS_GUARD \
    -DSD_BUILD_SHARED_LIBS=ON -DSD_BUILD_EXAMPLES=OFF -DSD_BUILD_CLI=OFF -DSD_BUILD_TEST=OFF \
    -DGGML_CUDA=OFF -DGGML_VULKAN=OFF; then
    echo "[$TARGET] could not configure the image engine"; return 1
  fi
  echo "[$TARGET] building the image engine"
  cmake --build "$SD_BUILD" -j "$JN" || { echo "[$TARGET] the image engine build failed"; return 1; }

  # the server links no engine and no backend: one configure answers for every
  # x86_64 target of this run, and the version is the only name it carries.
  # the same gate run.sh and the .bat paths use: the server is configured only
  # when its cache answers with another version - or with the old engine link
  # this build no longer has - than this run names; a tree already naming this
  # very version needs no touch at all. names are read before the colon, the
  # one stable part - the cache types these defines as it likes.
  NEEDS_RENAME=1
  if [ -f "$API_BUILD/CMakeCache.txt" ]; then
    CACHE_VERSION=$(sed -n 's/^OLS_VERSION[^=]*=\(.*\)/\1/p' "$API_BUILD/CMakeCache.txt" | head -n 1)
    if [ "$CACHE_VERSION" = "$CODE_VERSION" ] &&
       ! grep -q '^OLS_SD_BUILD' "$API_BUILD/CMakeCache.txt" 2>/dev/null &&
       [ -f "$API_BUILD/cmake_install.cmake" ]; then
      NEEDS_RENAME=""
    fi
  fi
  if [ -n "$NEEDS_RENAME" ]; then
    if ! cmake -S src -B "$API_BUILD" $COMP_OPTS -DCMAKE_BUILD_TYPE=Release \
      "-DOLS_VERSION=$CODE_VERSION" $RPATH_API; then
      echo "[$TARGET] could not configure open-local-services"; return 1
    fi
  fi
  echo "[$TARGET] building open-local-services"
  cmake --build "$API_BUILD" -j "$JN" || { echo "[$TARGET] the src build failed"; return 1; }

  # the binary and every library it really needs, nothing else
  cp -f "$API_BUILD/open-local-services" "$OUT/open-local-services"
  # the staged room, bin/, holds what this box runs: the server and the engine
  # of its shape. the last target of a full list is not necessarily that
  # shape, so staging asks the box's own shape alone - and a person rebuilding
  # this box builds here, posts the hand, and the running server moves in what
  # stands staged while it is down.
  mkdir -p bin
  if [ "$TARGET" = "$BOX_SHAPE" ]; then
    cp -f "$API_BUILD/open-local-services" bin/open-local-services
  fi
  # the engine library goes beside the server under the name the server opens
  # it by; the collection reads it from where the engine build put it.
  ENGINE_FILE=$(find "$SD_BUILD" -maxdepth 3 -name 'libstable-diffusion.so*' ! -name '*.so.*' 2>/dev/null | head -n 1)
  [ -n "$ENGINE_FILE" ] || ENGINE_FILE=$(find "$SD_BUILD" -maxdepth 3 -name 'libstable-diffusion.so.*' 2>/dev/null | head -n 1)
  if [ -z "$ENGINE_FILE" ]; then
    echo "[$TARGET] the engine build left no libstable-diffusion.so under $SD_BUILD"
    return 1
  fi
  cp -f "$ENGINE_FILE" "$OUT/$ENGINE_OUT_NAME"
  # the engine's own engine-name stands in its build folder; the server never
  # sees that name, and neither does the release.
  if [ "$TARGET" = "$BOX_SHAPE" ]; then
    cp -f "$ENGINE_FILE" "bin/$ENGINE_OUT_NAME"
  fi
  # the pages travel with the binary: a webui folder beside the server is
  # served from disk, so the release folder holds the live structure the
  # development folder holds, and the copy embedded in the binary answers
  # only when it is moved somewhere without one.
  rm -rf "$OUT/webui"
  cp -r webui "$OUT/webui"
  # a cuda artifact that resolves its runtime from this box's toolkit is not
  # a release yet: the machine down the network holds a driver and nothing
  # else. what the loader really asks for is read off the artifacts, and every
  # name the toolkit resolves is carried beside the binary as its real file.
  # the driver's own libcuda belongs to the machine that owns the card and is
  # never carried - the filter only ever names files inside the toolkit.
  if [ "$CUDA" = on ]; then
    for F in "$OUT/open-local-services" "$OUT"/*.so*; do
      [ -f "$F" ] || continue
      ldd "$F" 2>/dev/null | awk '$2 == "=>" && $3 ~ /^\/usr\/local\/cuda/ {print $1" "$3}'
    done | sort -u > "$OUT/.ols-cuda-needs"
    while read -r LIB_REAL; do
      LIB=${LIB_REAL%% *}
      REAL=${LIB_REAL#* }
      [ -n "$LIB" ] || continue
      [ -f "$OUT/$LIB" ] && continue
      echo "[$TARGET] carried: $LIB"
      cp -L -f "$REAL" "$OUT/$LIB"
    done < "$OUT/.ols-cuda-needs"
    rm -f "$OUT/.ols-cuda-needs"
  fi
  return 0
}

say_target() {
  TARGET=$1
  OUT=$REL/$TARGET
  echo ""
  echo "== $OUT: what the binary actually carries =="
  file "$OUT/open-local-services" 2>/dev/null
  readelf -h "$OUT/open-local-services" 2>/dev/null | grep -i 'machine\|type'
  echo "runtime libraries the folder really asks for, beyond the system ones:"
  ldd "$OUT/open-local-services" 2>/dev/null | grep -E 'cudart|cublas|libcuda' || true
  for SO in "$OUT"/libstable-diffusion-*.so; do
    [ -f "$SO" ] || continue
    echo "  engine $(basename "$SO"):"
    ldd "$SO" 2>/dev/null | grep -E 'cudart|cublas|libcuda' | sed 's/^/    /'
  done
  # the newest glibc any part of this asks a symbol from is the floor of
  # systems it can run on: stated off the binaries, never assumed. built on a
  # box of the same ubuntu release as its landing machine, it reads as that
  # release's own version - which is the whole point of building linux
  # artifacts on the crash-test dummy.
  FLOOR=$(objdump -T "$OUT/open-local-services" "$OUT"/lib* 2>/dev/null |
          sed -n 's/.*\(GLIBC_[0-9][0-9.]*\).*/\1/p' | sort -uV | tail -n 1)
  [ -n "$FLOOR" ] && echo "the newest glibc symbol any of it asks for: $FLOOR"
  if [ "$CUDA" = on ] && [ -n "$NVCC" ]; then
    echo "cuda generations the tool reads back from the engine:"
    for SO in "$OUT"/libstable-diffusion-*.so; do
      [ -f "$SO" ] || continue
      printf '  %s: ' "$(basename "$SO")"
      if [ -n "$CU" ]; then
        "$CU" -lelf "$SO" 2>/dev/null | grep -o 'sm_[0-9]*' | sort -u | tr '\n' ' '
      else
        echo "the tool that reads them is not in this toolkit; the compile asked for sm_$(echo $CUDA_OK | tr ' ' ';')"
      fi
      echo ""
    done
  fi
  echo ""
}

# the plan: one line per target this run builds, its name, how it is built,
# and whether its engine takes a device backend. the cuda target is one
# engine carrying every supported generation, not one per generation.
PLAN=""
for T in $WANT; do
  case "$T" in
    cpu) PLAN="$PLAN linux-x86_64:host:off" ;;
    arm64) PLAN="$PLAN linux-arm64:cross:off" ;;
  esac
done
if [ -n "$CUDA_OK" ]; then
  PLAN="$PLAN linux-x86_64-cuda:host:on"
fi

FAILED=""
# the shape this box itself runs, read off the box: the staged room bin/ must
# hold what the serve standing here reads, and a cross-built target never
# does - the card, if one answers, names the cuda shape; without a card this
# box runs the plain cpu shape.
BOX_SHAPE=linux-x86_64
[ "$(uname -m)" = aarch64 ] && BOX_SHAPE=linux-aarch64
BOX_CAP=$(nvidia-smi --query-gpu=compute_cap --format=csv,noheader 2>/dev/null | head -1 | tr -d .)
if [ -n "$BOX_CAP" ] && [ "$(uname -m)" = x86_64 ]; then
  BOX_SHAPE=linux-x86_64-cuda
fi
for SPEC in $PLAN; do
  TARGET=${SPEC%%:*}
  REST=${SPEC#*:}
  KIND=${REST%%:*}
  CUDA=${REST#*:}
  case "$TARGET" in
    linux-x86_64)
      KIND=host; CUDA=
      echo ""
      echo "== linux-x86_64, this box, the cpu engine =="
      ;;
    linux-arm64)
      if [ -z "$CROSS_GXX" ]; then
        echo ""
        echo "linux-arm64: the aarch64 cross compiler is not installed here."
        echo "crossbuild-essential-arm64 through the package manager provides it;"
        echo "without it this target is not built, and no arm64 artifact exists."
        FAILED="$FAILED linux-arm64"
        continue
      fi
      KIND=cross; CUDA=
      echo ""
      echo "== linux-arm64, cross-built with $CROSS_GXX =="
      ;;
    linux-x86_64-cuda)
      KIND=host; CUDA=on
      echo ""
      echo "== linux-x86_64-cuda, this box: one CUDA engine with kernels for sm_$(echo $CUDA_OK | tr ' ' ';') =="
      ;;
  esac
  build_one "$TARGET" "$KIND" "$CUDA" || FAILED="$FAILED $TARGET"
  say_target "$TARGET"
  # the release of this target is one archive: the folder just above it,
  # named for the commit its binary carries. that file is what gets committed
  # beside the source, what is put on a machine, and what that machine opens
  # where it stands and runs - no builder, no installer, nothing fetched from
  # anywhere. its sha256 joins the release's own SHA256SUMS-linux.txt, so a copy
  # whose bytes changed on the way reads as changed before anyone runs it.
  if [ -f "$REL/$TARGET/open-local-services" ]; then
    # the target already names itself with its os word - linux-x86_64-cuda-70 -
    # so the archive says it once, the way the windows archives say win once
    # beside their own folder names.
    ARCHIVE_NAME=open-local-services-$VERSION7-$TARGET.tar.gz
    rm -f "$REL/$ARCHIVE_NAME"
    if tar -c -z -f "$REL/$ARCHIVE_NAME" -C "$REL" "$TARGET"; then
      ( cd "$REL" && sha256sum "$ARCHIVE_NAME" >> SHA256SUMS-linux.txt )
      echo "wrote Release/$ARCHIVE_NAME"
    else
      echo "could not write the archive for $TARGET - the folder stands, unarchived."
      FAILED="$FAILED $TARGET"
    fi
  fi
done

# what the tree holds, and what it does not: the note the README points at.
{
  echo "the release artifacts of open-local-services, built by build-rel.sh"
  echo "version: the code of every artifact here is the commit $CODE_VERSION"
  echo "on $(uname -s) $(uname -r), cmake $(cmake --version | head -1), $(gcc --version | head -1)"
  echo ""
  echo "the sources every artifact here was built from, as their .pin files"
  echo "record them in this tree:"
  echo "  image engine: vendor/stable-diffusion.cpp at $PIN_IMAGE"
  echo "  image ggml: vendor/stable-diffusion.cpp/ggml at $PIN_IMAGE_GGML"
  echo ""
  echo "the release of each target is one archive beside this file, named for"
  echo "the commit its binary carries - open-local-services-$VERSION7-linux-<target>.tar.gz -"
  echo "and the sha256 of every archive stands in SHA256SUMS-linux.txt. inside each:"
  echo "the binary, the image engine beside it, the cuda runtime it really"
  echo "imports, and the webui folder it serves from disk."
  echo "open such a folder where it stands and run it; nothing is compiled and"
  echo "nothing is fetched on the machine that receives it."
  echo ""
  for SPEC in $PLAN; do
    N=${SPEC%%:*}
    case "$N" in
      linux-x86_64) CUDA="the cpu engine alone (the instruction sets are reached through runtime detection, never baked for the machine that built it)";;
      linux-arm64) CUDA="the cpu engine only: this box has no CUDA runtime for arm64 and no Vulkan library for it, so those backends are not in this artifact";;
      linux-x86_64-cuda) CUDA="the CUDA engine, one library with device kernels for sm_$(echo $CUDA_OK | tr ' ' ';') - the loader inside it picks each card's own when the server opens it";;
    esac
    if [ -f "$REL/$N/open-local-services" ]; then
      echo "$N: open-local-services - engine: $CUDA"
      echo "  the image engine stands beside the server as libstable-diffusion-cpu.so"
      echo "  or libstable-diffusion-cuda.so; the server links no engine and no ggml,"
      echo "  and opens the library by name when it starts."
    else
      echo "$N: not built - see the lines above"
    fi
  done
  if [ -n "$CUDA_SKIPPED" ]; then
    echo "card generations this toolkit could not write: $CUDA_SKIPPED"
  fi
  for N in $FAILED; do echo "$N did not finish"; done
} > "$REL/build-info.txt"

for N in $FAILED; do echo "$N did not finish."; done
echo ""
echo "Release/build-info.txt now says what every artifact here carries."
# the repository is the release, so an archive that stands here uncommitted
# is a release that has not happened yet. this script never commits anything -
# it reads the archive list off git and names the ones the tree has not been
# shown, so no finished binary is ever left only on this machine.
UNCOMMITTED=$(git status --porcelain -- Release 2>/dev/null)
if [ -n "$UNCOMMITTED" ]; then
  echo "the archives below are built but not in the repository yet - commit"
  echo "them with the same discipline as the code: what is committed is what"
  echo "runs."
  echo "$UNCOMMITTED" | sed 's/^/  /'
else
  echo "every archive under Release/ is committed: the release is the repository."
fi
echo "the engine trees are now built the release way; run.sh puts them back the"
echo "development way when it runs."
