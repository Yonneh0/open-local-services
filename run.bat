@ECHO OFF
REM run.sh for Windows: build every part of open-local-services if needed,
REM then start the server. Same steps as run.sh, with Windows tool discovery.
REM Pass "build" to build and install without starting the server.
REM The shape this run builds follows the box. A CUDA card named - the card in
REM this box as nvidia-smi names it, an arch=<number> word, or the environment -
REM builds whole on CUDA: every part, the image engine, the server's own device
REM probe, and the voice engine's device code, compiled on nvcc, and nothing of
REM the Vulkan sdk is asked of that machine. A box that names no card builds
REM on the Vulkan sdk it carries, in the plain roots; only a box with no card
REM and no sdk builds with no device backend at all - the shape build-rel.sh
REM names with -DOLS_NO_DEVICE=1 - and there the engines work where the threads are.
REM Pass "vulkan" for the sdk's shape - the one that needs the Vulkan sdk and
REM builds in the plain roots - "cuda" is taken as the default it names, and
REM "nodevice" forces the device-free shape where a card could be named.
REM Optional runtime settings as arguments: key=<access key> rate=<requests per minute>
REM Everything already built is skipped, and a server that is running is stopped
REM only after the build below succeeds, right before its binary is replaced.
SETLOCAL EnableDelayedExpansion

rem ---- server location, same defaults as the C++ side ----
rem the server reads its own .env, so the defaults here are only for this script's
rem own words: they are kept in script-local names, never handed to the server in
rem its environment, where they would outrank the .env line a person edits through
rem the config panel at the next start. a value the environment already carries is
rem a person's own choice and is left in place, the same as run.sh does.
set "SRV_HOST=%OLS_HOST%"
if not defined SRV_HOST set SRV_HOST=0.0.0.0
rem The port defaults to 8765. OLS_PORT may ask for another port from the
rem environment or .env. If that port is in use, the server refuses to start.
set "SRV_PORT=%OLS_PORT%"
if not defined SRV_PORT (
  if exist .env (
    for /f "usebackq tokens=1,* delims==" %%P in (".env") do (
      if /i "%%P"=="OLS_PORT" set "SRV_PORT=%%Q"
    )
  )
)
if not defined SRV_PORT set SRV_PORT=8765
set "BAD_PORT="
for /f "delims=0123456789" %%V in ("!SRV_PORT!") do set "BAD_PORT=1"
if defined BAD_PORT (
  echo Invalid OLS_PORT=!SRV_PORT!: use a number from 1 to 65535.
  exit /b 1
)
if !SRV_PORT! LEQ 0 (
  echo Invalid OLS_PORT=!SRV_PORT!: use a number from 1 to 65535.
  exit /b 1
)
if !SRV_PORT! GTR 65535 (
  echo Invalid OLS_PORT=!SRV_PORT!: use a number from 1 to 65535.
  exit /b 1
)
rem 0.0.0.0 binds every address of the machine; the words people open name one of
rem them, the same link run.sh prints.
set SRV_LINK=!SRV_HOST!
if "!SRV_HOST!"=="0.0.0.0" set SRV_LINK=127.0.0.1
set MODE=run
rem the same two optional settings run.sh takes. in batch the = of key=value is
rem a separator, so the name and its value arrive as two words, and the value
rem is kept on the word that follows the name it names.
set PENDING=
rem fresh-full-rebuild (or --fresh-full-rebuild) is the one word that lets this
rem path restate a tree whose cache names another shape - see the shape check
rem before each configure, which otherwise refuses rather than compiling a tree
rem again in full.
set FRESH_FULL=
FOR %%A IN (%*) DO (
  IF DEFINED PENDING (
    IF /I "!PENDING!"=="key" SET "OLS_API_KEY=%%A"
    IF /I "!PENDING!"=="rate" SET "OLS_RATE_LIMIT=%%A"
    IF /I "!PENDING!"=="arch" SET CUDA_ARCH=%%A
    set PENDING=
  ) ELSE (
    IF /I "%%A"=="build" SET MODE=build
    IF /I "%%A"=="cuda" SET CUDA_WANTED=1
    IF /I "%%A"=="vulkan" SET VULKAN_WANTED=1
    IF /I "%%A"=="nodevice" SET NODEVICE_WANTED=1
    IF /I "%%A"=="key" set PENDING=key
    IF /I "%%A"=="rate" set PENDING=rate
    IF /I "%%A"=="arch" set PENDING=arch
    IF /I "%%A"=="fresh-full-rebuild" set FRESH_FULL=1
    IF /I "%%A"=="--fresh-full-rebuild" set FRESH_FULL=1
  )
)
rem ---- which backend this run builds ----
rem the card in this box is CUDA-first, so CUDA is the default: the shape built
rem whole on nvcc, in the roots named for its card, the same shape run.sh builds
rem on the linux boxes. that path asks nothing of the Vulkan sdk - nvcc, ninja
rem and the studio environment are its whole toolchain. "vulkan" is the one word
rem that asks for the other shape, the cpu-avx512 target build-rel.bat names,
rem which is the sdk's and lives in the plain roots. the "cuda" word is taken
rem too: it names the default, and the start path the laptop down the network
rem uses hands it. naming both words is a contradiction this script does not
rem settle for anyone.
set CUDA_ON=
set NODEVICE=
if defined CUDA_WANTED if defined VULKAN_WANTED (
  echo cuda and vulkan name two backends: name one of them, or neither.
  exit /b 1
)
if defined NODEVICE_WANTED if defined VULKAN_WANTED (
  echo nodevice and vulkan name two shapes: name one of them, or neither.
  exit /b 1
)
if defined NODEVICE_WANTED if defined CUDA_WANTED (
  echo nodevice and cuda name two shapes: name one of them, or neither.
  exit /b 1
)
if defined NODEVICE_WANTED (
  set NODEVICE=1
) else if not defined VULKAN_WANTED (
  set CUDA_ON=1
)
rem cmake picks the job count itself; no need to count cores here.
set PARALLEL=--parallel
rem the output cache needs no words here: the server's own default is the data
rem folder beside it, which is where this script starts it from - the same place
rem %~dp0data names. a value .env or the environment carries still reaches it.

echo.
echo == looking for the toolchain ==
rem bare cmake is on PATH for the steps below; this only checks that it is there
for /f "delims=" %%L in ('where cmake 2^>nul') do set CMAKE_OK=1
if not defined CMAKE_OK (
  echo Could not find cmake on PATH.
  EXIT /B 1
)
rem ---- the SDK the Vulkan shape asks for ----
rem looked for on every run: a box that names no CUDA card builds on it when it
rem stands here, and the CUDA build asks nothing of it - nvcc, ninja and the
rem studio environment are the whole toolchain that path needs. what was found
rem is restated by the shape's own line below; only the vulkan word refuses for
rem a box that carries none.
set VULKAN_DIR=
call :find_vulkan_sdk
IF DEFINED VULKAN_DIR (
  rem the engine builds find the sdk where the loader does: the name every
  rem cmake step reads is this one, restated from the folder actually found
  rem rather than left at whatever stale path the environment carried.
  SET VULKAN_SDK=!VULKAN_DIR!
  SET VULKAN_INCLUDE_DIR=!VULKAN_DIR!\include
  SET VULKAN_LIB_DIR=!VULKAN_DIR!\lib
)
IF DEFINED VULKAN_WANTED IF NOT DEFINED VULKAN_DIR (
  echo No Vulkan SDK found; the Vulkan engines will not build.
  echo Name the SDK in VULKAN_SDK, or drop the vulkan word and let this script
  echo choose the shape this box can stand: its CUDA card, its sdk, or the cpu.
  EXIT /B 1
)
rem MSVC compiler: newest cl.exe on the box, whatever the edition folder says
set MSVC_BIN=
set MSVC_STUDIO=
rem a box that carries more than one toolchain - VS 2022 and a newer VS side by
rem side - has to be able to name which cl.exe a CUDA build may use, since the
rem CUDA host-compiler table allows only the editions of its own release. what is
rem named is checked before any is looked for, and the studio environment is the
rem one beside the compiler that was chosen, so the two always come together.
set MSVC_NAMED=
if defined OLS_MSVC_DIR if exist "%OLS_MSVC_DIR%\cl.exe" set "MSVC_NAMED=%OLS_MSVC_DIR%"
if defined OLS_MSVC_DIR if not defined MSVC_NAMED echo the folder named in OLS_MSVC_DIR has no cl.exe there - looking for one instead.
if defined MSVC_NAMED (
  set "MSVC_BIN=!MSVC_NAMED!"
  for %%S in ("!MSVC_NAMED!\..\..\..\..\..\..\..") do set "MSVC_STUDIO=%%~fS"
  goto :msvc_found
)
for /d %%E in ("C:\Program Files\Microsoft Visual Studio\*" "C:\Program Files \(x86\)\Microsoft Visual Studio\*") do (
  for /d %%R in ("%%~E\*" "%%~E\Current") do (
    if exist "%%~R\VC\Tools\MSVC\*" (
      for /f "delims=" %%V in ('dir /b /ad "%%~R\VC\Tools\MSVC" 2^>nul') do (
        if exist "%%~R\VC\Tools\MSVC\%%V\bin\Hostx64\x64\cl.exe" (
          set "MSVC_BIN=%%~R\VC\Tools\MSVC\%%V\bin\Hostx64\x64"
          set "MSVC_STUDIO=%%~R"
        )
      )
    )
  )
)
:msvc_found
if not defined MSVC_BIN (
  echo No MSVC compiler found.
  exit /b 1
)
echo MSVC cl.exe at !MSVC_BIN!
rem the studio environment a ninja build needs: the root of the compiler chosen
rem above - the folder that holds that cl.exe is the one whose vcvars64.bat a
rem CUDA build calls, so the two always come from the same installed edition.
set VS_ROOT=
if exist "!MSVC_STUDIO!\VC\Auxiliary\Build\vcvars64.bat" set VS_ROOT=!MSVC_STUDIO!
rem ---- the vulkan driver manifest: the newest driver-store package that has one.
rem without it the sdk loader points at a stale directory and reports no drivers.
rem only the Vulkan shape reads a manifest: a CUDA card is answered by its own
rem runtime, and a manifest pointing at nothing is no part of that path. the
rem search waits for the shape below, which a card-less box may still take.
set VK_DRIVER_DIR=
rem ---- the CUDA path, the one this script takes by default ----
rem a card that speaks CUDA builds whole on it: the image engine, the server's own
rem device probe, and the voice engine's device code all take the CUDA backend.
rem the project builder a bare Windows shell reaches cannot be handed nvcc at all,
rem so this path names ninja and the toolkit itself, and takes the compilers
rem through the studio environment - the same lines a CUDA build on any Windows
rem box needs. the roots are named for the backend, so the two builds of one tree
rem never share a cache.
rem the card this code is written for: the arch= word above names it, then the
rem environment, then the card in this box as nvidia-smi names it - the same
rem words on every machine, so a run here writes for the card here and a run on
rem the laptop down the network for the card in that one. what nvidia-smi calls
rem 8.6 is the number 86 to the compiler, so the dot goes out. a box whose
rem nvidia-smi cannot name that field, with nothing named, writes for the oldest
rem shape this project supports. a word, or the environment, has said which card
rem before this block is reached, and what they named is never overwritten here.
rem the comma of the format is quoted: cmd reads it as a separator, and hands
rem nvidia-smi two options where one name was written.
rem a box whose device is gone answers "No devices were found" on the standard
rem output, where the line below reads it - that sentence names no capability,
rem so a word with anything but digits and a dot in it is dropped here, and the
rem shape taken below is the one a box with no card takes.
set SMICAP=
if not defined CUDA_ARCH for /f "delims=" %%L in ('nvidia-smi --query-gpu=compute_cap "--format=csv,noheader" 2^>nul') do (
  if not defined SMICAP set "SMICAP=%%L"
)
if not defined CUDA_ARCH if defined SMICAP (
  for /f "delims=0123456789." %%C in ("!SMICAP!") do set SMICAP=
)
if not defined CUDA_ARCH set CUDA_ARCH=!SMICAP!
if defined CUDA_ARCH set CUDA_ARCH=!CUDA_ARCH:.=!
set CUDA_HOME=
set NINJA_EXE=
for /d %%C in ("C:\Program Files\NVIDIA CUDA*" "C:\Program Files (x86)\NVIDIA CUDA*" "C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\*" "C:\Program Files (x86)\NVIDIA GPU Computing Toolkit\CUDA\*") do (
  if exist "%%~C\bin\nvcc.exe" set CUDA_HOME=%%~C
)
for /f "delims=" %%L in ('where nvcc 2^>nul') do (
  if not defined CUDA_HOME for %%P in ("%%~fL\..") do set CUDA_HOME=%%~fP
)
for /f "delims=" %%L in ('where ninja 2^>nul') do if not defined NINJA_EXE set NINJA_EXE=%%~fL
if not defined NINJA_EXE for /f "delims=" %%L in ('dir /b /s "C:\Program Files\CMake\bin\ninja.exe" 2^>nul') do (
  if not defined NINJA_EXE set NINJA_EXE=%%~fL
)
rem the studio found above carries its own ninja beside its cmake tools - the
rem same installed edition whose compiler this build just chose, so the two
rem always come from one edition.
if not defined NINJA_EXE if defined VS_ROOT if exist "!VS_ROOT!\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe" set NINJA_EXE=!VS_ROOT!\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe
rem the folder ninja sits in, so a nested configure a vendored project runs
rem can find the build program on PATH without anyone naming it again.
set NINJA_DIR=
if defined NINJA_EXE for %%P in ("!NINJA_EXE!\..") do set NINJA_DIR=%%~fP
rem what the shape this run builds asks of the machine, said before any build is
rem asked of it: the CUDA path needs the toolkit, ninja, and the studio
rem environment, and asks nothing of an SDK; the Vulkan path needs its SDK, and
rem refuses for it above. a box that names no card and holds no toolkit is not a
rem CUDA machine: it builds on the Vulkan sdk it carries, the shape the vulkan
rem word names, and only a box carrying no sdk too takes the shape with no
rem device backend, as build-rel.sh names it for such a box. a card named by
rem word or environment with no toolkit under it stays the refusal it always
rem was - that shape was asked for by name, and this script does not answer a
rem named shape with another.
if defined CUDA_ON if not defined CUDA_HOME if not defined CUDA_ARCH (
  set CUDA_ON=
  if defined VULKAN_DIR (
    echo No CUDA toolkit here and nvidia-smi names no card, but the Vulkan sdk
    echo stands here: this build takes the sdk's shape, in the plain roots.
    set VULKAN_WANTED=1
  ) else (
    echo No CUDA toolkit here, nvidia-smi names no card, and no Vulkan sdk stands here:
    echo this build carries no device backend, the shape build-rel.sh names -DOLS_NO_DEVICE=1.
    set NODEVICE=1
  )
)
if defined CUDA_ON (
  if not defined CUDA_HOME (
    echo No CUDA toolkit: nvcc is under neither the Program Files nor PATH.
    echo A CUDA shape was named - the cuda word or an arch= word names it - and
    echo that shape compiles on nvcc; name the sdk with the vulkan word, or let
    echo the card-less shape stand with the nodevice word.
    exit /b 1
  )
  if not defined NINJA_EXE (
    echo No ninja to drive a CUDA build; the project builder here cannot take nvcc.
    exit /b 1
  )
  if not defined VS_ROOT (
    echo No studio environment to hand a CUDA build its compilers.
    exit /b 1
  )
  echo CUDA toolkit at !CUDA_HOME!, ninja at !NINJA_EXE!, card at sm_!CUDA_ARCH!
)
rem the manifest waits for the settled shape: only a build that reads Vulkan
rem through the sdk loader asks the driver store for it.
if defined VULKAN_WANTED call :find_vk_driver
if defined VULKAN_WANTED echo Vulkan compute with the SDK at !VULKAN_DIR!, in the plain roots
if defined NODEVICE echo No device backend: the engines and the server build on the cpu alone
rem the roots each backend builds in, named for it: always a build subfolder
rem inside the source being built - never a short alias in some unrelated place
rem - so a source tree keeps its own build and nothing else is scattered. the
rem CUDA roots - the default - carry the card generation its code is written
rem for, under the same names build-rel.bat uses, so a release build reuses what
rem a development build of that card put in place - and the two backends of one
rem tree never share a cache.
set SD_ROOT=api\vendor\stable-diffusion.cpp\build-c!CUDA_ARCH!
set API_ROOT=api\build-c!CUDA_ARCH!
set BACKEND=CUDA sm_!CUDA_ARCH!
if defined VULKAN_WANTED (
  set SD_ROOT=api\vendor\stable-diffusion.cpp\build
  set API_ROOT=api\build
  set BACKEND=Vulkan
)
if defined NODEVICE (
  set SD_ROOT=api\vendor\stable-diffusion.cpp\build-nd
  set API_ROOT=api\build-nd
  set BACKEND=cpu, no device backend
)
rem ---- the one incremental path every entry point shares ----
rem every shape of this path is named at its configure - the card generation and
rem the cpu instruction sets of the CUDA default, the instruction sets of the
rem Vulkan shape - under the very roots build-rel.bat names that same shape:
rem cuda-<arch> for the card this build writes for, and the cpu-avx512 shape for
rem the one the SDK builds. so a run from either script finds a cache that
rem already answers with these very values and recompiles nothing but the file
rem that carries the version. changing the flags here without changing them
rem there is what poisons a shared tree and rebuilds it whole, for hours, over
rem and over.
set OLS_GUARD=-DCMAKE_PROJECT_INCLUDE_BEFORE="%~dp0api\ols-sage-guard.cmake"
rem The deep build subfolders inside the engine sources go past what the Visual
rem Studio task runner can open its task logs at - inside the vendored ggml the
rem scratch paths for a task log simply stop existing - so the engine trees are
rem built with ninja when it is found: ninja keeps no per-task log, needs no
rem scan, and it is the same builder the .sh side drives. Under ninja the
rem compilers come from the studio environment and ninja's own folder joins
rem PATH, so both this configure and any nested one a vendored project runs
rem choose this machine's MSVC and find ninja - left to itself cmake picks
rem another compiler up the PATH and a nested configure finds no ninja at all.
rem When ninja is not found the trees fall back to the Visual Studio generator
rem and its limits.
set ENGINE_GEN=
if defined NINJA_EXE set ENGINE_GEN=-G Ninja "-DCMAKE_MAKE_PROGRAM=!NINJA_EXE!"
set ENGINE_VC=
if defined NINJA_EXE if defined VS_ROOT set ENGINE_VC=call "!VS_ROOT!\VC\Auxiliary\Build\vcvars64.bat" >nul
set ENGINE_NINJA_PATH=
if defined NINJA_DIR set ENGINE_NINJA_PATH=set "PATH=!NINJA_DIR!;!PATH!"
set SD_CPU_FLAGS=-DGGML_NATIVE=OFF -DGGML_AVX512=ON -DGGML_AVX512_VBMI=ON -DGGML_AVX512_VNNI=ON -DGGML_AVX512_BF16=ON
set NOVEC_FLAGS=-DGGML_NATIVE=OFF -DGGML_AVX512=OFF -DGGML_AVX512_VBMI=OFF -DGGML_AVX512_VNNI=OFF -DGGML_AVX512_BF16=OFF
rem the one word the api build takes for a box with no device backend, the way
rem build-rel.sh names it: the voice engine then carries the cpu backend alone,
rem and none of the server's device code is compiled.
set OLS_NODEVICE=
if defined NODEVICE set OLS_NODEVICE=-DOLS_NO_DEVICE=1
echo == building the parts, skipping anything already built ==

rem ---- clone the engine sources when they are missing ----
if not exist "api\vendor\stable-diffusion.cpp\CMakeLists.txt" (
  echo Cloning the image engine source, 41 MB
  git clone --depth 1 --single-branch https://github.com/leejet/stable-diffusion.cpp api\vendor\stable-diffusion.cpp
  if errorlevel 1 exit /b 1
)
rem the voice engine is linked into the server itself, so its source is fetched
rem like the others; the model set and the phonemizer it needs are named by the
rem api build below, not by any option here.
if not exist "api\vendor\audio.cpp\CMakeLists.txt" (
  echo Cloning the voice engine source
  git clone --depth 1 --single-branch https://github.com/0xShug0/audio.cpp api\vendor\audio.cpp
  if errorlevel 1 exit /b 1
)
if not exist "api\vendor\nlohmann\json.hpp" (
  echo Fetching the json header
  curl -sSL --fail --retry 3 -o "api\vendor\nlohmann\json.hpp" --create-dirs https://raw.githubusercontent.com/nlohmann/json/develop/single_include/nlohmann/json.hpp
  if errorlevel 1 exit /b 1
)

rem ---- put every engine source on the commit this code is written against:
rem the same commits run.sh names.
rem both machines build the exact same source at the same commit, so what one
rem builds is what the other builds. a tree already on its commit needs
rem no network ----
set PIN_IMAGE=b13a48219d5cdf408edcb93da6cd69b612ed03d3
set PIN_IMAGE_GGML=dc998771e700d49bbdf9a39cede22bb5b3311767
call :pin "api\vendor\stable-diffusion.cpp" "!PIN_IMAGE!" leejet/stable-diffusion.cpp
if errorlevel 1 (
  echo Could not put the image engine on its pinned commit.
  exit /b 1
)
call :pin_sub "api\vendor\stable-diffusion.cpp" ggml "!PIN_IMAGE_GGML!"
if errorlevel 1 (
  echo Could not put the image engine ggml on its pinned commit.
  exit /b 1
)
set PIN_VOICE=41bb3f0ffb6b46b7a3bb3c02fcea52900d88001b
call :pin "api\vendor\audio.cpp" "!PIN_VOICE!" 0xShug0/audio.cpp
if errorlevel 1 (
  echo Could not put the voice engine on its pinned commit.
  exit /b 1
)


rem ---- build the image engine ----
rem the image engine is built as a shared library in its own vendor tree, under
rem the build subfolder inside that source - like every other engine, nothing
rem is generated at the tree root. the engine trees are built with ninja where
rem it is found: inside the vendored ggml the scratch paths for a Visual
rem Studio task log go past what the task runner can open, and ninja keeps no
rem such logs.
rem a built engine is found where either project builder puts it: the Visual
rem Studio one under a bin\Release folder, ninja a plain bin\ beside the root.
set SD_DONE=
if exist "!SD_ROOT!\bin\Release\stable-diffusion.dll" set SD_DONE=1
if exist "!SD_ROOT!\bin\stable-diffusion.dll" set SD_DONE=1
if not defined SD_DONE (
    rem a root that stands here was configured for one backend and one card
    rem generation: this shape is asked of it before cmake is, so a run that
    rem asks another of them says so instead of compiling the tree again in full.
    if exist "!SD_ROOT!\CMakeCache.txt" (
        call :ask_shape "!SD_ROOT!\CMakeCache.txt" CMAKE_CUDA_ARCHITECTURES "!CUDA_ARCH!" "the image engine"
        if errorlevel 1 exit /b 1
        if defined CUDA_ON (
            call :ask_shape "!SD_ROOT!\CMakeCache.txt" GGML_CUDA "ON" "the image engine"
            if errorlevel 1 exit /b 1
            call :ask_shape "!SD_ROOT!\CMakeCache.txt" GGML_VULKAN "OFF" "the image engine"
            if errorlevel 1 exit /b 1
        )
    )
    echo Building the image engine as a shared library with !BACKEND! compute
    if defined CUDA_ON (
        call "!VS_ROOT!\VC\Auxiliary\Build\vcvars64.bat" >nul
        cmake -G Ninja "-DCMAKE_MAKE_PROGRAM=!NINJA_EXE!" -S api\vendor\stable-diffusion.cpp -B !SD_ROOT! -DCMAKE_BUILD_TYPE=Release -DSD_BUILD_SHARED_LIBS=ON -DSD_BUILD_CLI=OFF -DSD_BUILD_TEST=OFF -DSD_ENABLE_OPENVINO=OFF !NOVEC_FLAGS! -DGGML_VULKAN=OFF -DGGML_HIP=OFF -DGGML_COMMON=OFF -DGGML_OPENMP=OFF -DMATH_LIBRARY= -DUSE_GGML_SUBPROJECT=OFF -DGGML_SRC=ggml -DGGML_CUDA=ON "-DCMAKE_CUDA_COMPILER=!CUDA_HOME!\bin\nvcc.exe" "-DCUDAToolkit_ROOT=!CUDA_HOME!" -DCMAKE_CUDA_ARCHITECTURES=!CUDA_ARCH! !OLS_GUARD!
    ) else if defined NODEVICE (
        !ENGINE_VC!
        !ENGINE_NINJA_PATH!
        cmake !ENGINE_GEN! -S api\vendor\stable-diffusion.cpp -B !SD_ROOT! -DCMAKE_BUILD_TYPE=Release -DSD_BUILD_SHARED_LIBS=ON -DSD_BUILD_CLI=OFF -DSD_BUILD_TEST=OFF -DSD_ENABLE_OPENVINO=OFF !SD_CPU_FLAGS! -DGGML_VULKAN=OFF -DGGML_HIP=OFF -DGGML_COMMON=OFF -DGGML_OPENMP=OFF -DMATH_LIBRARY= -DUSE_GGML_SUBPROJECT=OFF -DGGML_SRC=ggml !OLS_GUARD!
    ) else (
        !ENGINE_VC!
        !ENGINE_NINJA_PATH!
        cmake !ENGINE_GEN! -S api\vendor\stable-diffusion.cpp -B !SD_ROOT! -DCMAKE_BUILD_TYPE=Release -DSD_BUILD_SHARED_LIBS=ON -DSD_BUILD_CLI=OFF -DSD_BUILD_TEST=OFF -DSD_ENABLE_OPENVINO=OFF !SD_CPU_FLAGS! -DGGML_VULKAN=ON -DGGML_HIP=OFF -DGGML_COMMON=OFF -DGGML_OPENMP=OFF -DMATH_LIBRARY= -DUSE_GGML_SUBPROJECT=OFF -DGGML_SRC=ggml !OLS_GUARD!
    )
    if errorlevel 1 (
      echo Could not configure the image engine.
      exit /b 1
    )
    cmake --build !SD_ROOT! --config Release %PARALLEL%
    if errorlevel 1 exit /b 1
) else (
    rem an engine root that stands here is asked for anything new on every run,
    the way the api is asked below: the engine's sources belong to this tree,
    and an edit no build ever reads is an edit that never happened. The same
    studio environment the configure above stands in is stood in here, since
    a bare shell hands the compiler a standard library from another visual
    studio than the one whose clang it is.
    if defined CUDA_ON (
        call "!VS_ROOT!\VC\Auxiliary\Build\vcvars64.bat" >nul
    ) else (
        !ENGINE_VC!
        !ENGINE_NINJA_PATH!
    )
    cmake --build !SD_ROOT! --config Release %PARALLEL%
    if errorlevel 1 exit /b 1
)

rem ---- build open-local-services: configure once, then a quick incremental build ----
rem this is the development path: the build names its version dev, and the
rem binary carries the moment the compiler ran beside that word. build-rel.bat
rem is the one that names a commit, and only after it checked the tree.
if not exist "!API_ROOT!\CMakeCache.txt" (
    echo Configuring the open-local-services build
    if defined CUDA_ON (
        call "!VS_ROOT!\VC\Auxiliary\Build\vcvars64.bat" >nul
        cmake -G Ninja "-DCMAKE_MAKE_PROGRAM=!NINJA_EXE!" -S api -B !API_ROOT! -DCMAKE_BUILD_TYPE=Release -DOLS_VERSION=dev -DOLS_CUDA=ON -DOLS_SD_BUILD=!SD_ROOT! "-DCMAKE_CUDA_COMPILER=!CUDA_HOME!\bin\nvcc.exe" "-DCUDAToolkit_ROOT=!CUDA_HOME!" -DCMAKE_CUDA_ARCHITECTURES=!CUDA_ARCH!
    ) else (
        cmake -S api -B !API_ROOT! -DCMAKE_BUILD_TYPE=Release -DOLS_VERSION=dev -DOLS_SD_BUILD=!SD_ROOT! !OLS_NODEVICE!
    )
    if errorlevel 1 exit /b 1
) else (
    rem the cache can hold a name the release path put in it, and the image
    rem library another target's shape pointed this tree at, so this path
    rem checks what the cache answers with and touches the cache only when
    rem it answers with another path's names: reconfiguring is what makes the
    rem builders re-link everything they hold, and a tree already naming this
    rem very version and this very image library needs no touch at all. a
    rem binary never prints a name it was not built from.
    set CACHE_VERSION=
    set CACHE_SD=
    rem the names are matched on their prefix, not on a type suffix: these
    rem defines are read-not-declared on the CMake side, so the cache names
    rem them with whatever type it likes, and only the name before the colon is stable.
    for /f "usebackq tokens=1,* delims==" %%A in ("!API_ROOT!\CMakeCache.txt") do (
      set CACHE_KEY=%%A
      if /I "!CACHE_KEY:~0,11!"=="OLS_VERSION" set CACHE_VERSION=%%B
      if /I "!CACHE_KEY:~0,12!"=="OLS_SD_BUILD" set CACHE_SD=%%B
    )
    set NEEDS_RENAME=
    if not "!CACHE_VERSION!"=="dev" set NEEDS_RENAME=1
    if not "!CACHE_SD!"=="!SD_ROOT!" set NEEDS_RENAME=1
    if defined NEEDS_RENAME (
      rem the version and the image library are names the link answers with, so
      rem restating them re-links what the tree holds; the card generation is
      rem the shape every object was compiled for, and that is asked before the
      rem cache is restated - a tree of another shape would be compiled again in
      rem full, which this path refuses unless fresh-full-rebuild was given.
      call :ask_shape "!API_ROOT!\CMakeCache.txt" CMAKE_CUDA_ARCHITECTURES "!CUDA_ARCH!" open-local-services
      if errorlevel 1 exit /b 1
      if defined CUDA_ON (
        rem the same names the configure above answers with, restated whole: the
        rem card generation, the toolkit and the builder are this tree's shape,
        rem and a restate that names them plainly is the one that cannot quietly
        rem write a cuda cache with the sdk's names in it.
        call "!VS_ROOT!\VC\Auxiliary\Build\vcvars64.bat" >nul
        cmake -G Ninja "-DCMAKE_MAKE_PROGRAM=!NINJA_EXE!" -S api -B !API_ROOT! -DCMAKE_BUILD_TYPE=Release -DOLS_VERSION=dev -DOLS_CUDA=ON -DOLS_SD_BUILD=!SD_ROOT! "-DCMAKE_CUDA_COMPILER=!CUDA_HOME!\bin\nvcc.exe" "-DCUDAToolkit_ROOT=!CUDA_HOME!" -DCMAKE_CUDA_ARCHITECTURES=!CUDA_ARCH!
      ) else (
        cmake -S api -B !API_ROOT! -DOLS_VERSION=dev -DOLS_SD_BUILD=!SD_ROOT! !OLS_NODEVICE!
      )
      if errorlevel 1 exit /b 1
    )
)
rem The api build is MSVC-compiled. If this shell did not stand the studio
rem environment before run.bat, the compiler cannot open its own headers and
rem ninja reports errors like 'atomic' or 'string' missing. The incremental
rem build path below needs the same environment the configure path used above.
if defined VS_ROOT call "!VS_ROOT!\VC\Auxiliary\Build\vcvars64.bat" >nul
rem cmake recompiles nothing when everything is current, so this is quick.
rem a build that does not finish is a build that says so: a source that
rem does not compile, or a download the network refused, leaves the tree
rem exactly as it was - its cache already answers with these very names,
rem checked just above - so nothing here throws the tree away and nothing here
rem can turn a one-file mistake into a rebuild of everything it holds. a
rem deliberate scratch build is made by hand: delete the tree, and this path
rem configures it again below.
echo Checking for anything new in api and rebuilding it if needed
cmake --build !API_ROOT! --config Release %PARALLEL%
if errorlevel 1 (
    echo The build did not finish. The tree keeps its shape and its cache.
    exit /b 1
)
rem the phonemizer data kokoro reads is built with the voice engine and kept
rem beside the server binary: the statically linked engine looks under the
rem executable's own folder for espeak-ng-data.
if not exist "api\espeak-ng-data\phontab" (
    xcopy /Y /E /Q "!API_ROOT!\_deps\espeak-static\build\espeak-ng-data" "api\espeak-ng-data\" >nul
)
rem the build above succeeded: a fresh binary exists, so stop the running
rem server now, right before its binary is replaced
call :stop_process open-local-services.exe
if errorlevel 2 echo The running server stands at the manager's level; this shell left it running, and the replace below waits for it to let go.
if not errorlevel 1 echo The running server was stopped to replace its binary.
rem the built engine is read from where either project builder puts it - the
rem Visual Studio one under a bin\Release folder, ninja a plain bin\.
set SD_BIN=!SD_ROOT!\bin\Release
if not exist "!SD_BIN!\stable-diffusion.dll" set SD_BIN=!SD_ROOT!\bin
set API_EXE=!API_ROOT!\Release\open-local-services.exe
if not exist "!API_EXE!" set API_EXE=!API_ROOT!\open-local-services.exe
call :replace "!API_EXE!" "api\"
if errorlevel 1 (
    echo Could not replace api\open-local-services.exe - check the api folder is writable.
    exit /b 1
)
rem the server carries its own ggml inside it - the voice engine runs on it and
rem the device probe answers from it - so nothing is copied beside it for the
rem server to load as a plugin: a backend already compiled in is registered
rem twice that way, which is what MSI's cuda build failed on.
copy /Y "!SD_BIN!\stable-diffusion.dll" "api\" >nul

if /i not "%MODE%"=="build" goto start
echo.
echo Built. Start the server with: run.bat
exit /b 0

:start
rem ---- start the server, unless one is already running ----
tasklist /FI "IMAGENAME eq open-local-services.exe" | findstr /I "open-local-services.exe" >nul
if not errorlevel 1 (
    echo The server is already running; leaving it as it is.
    exit /b 0
)
echo.
echo == starting the server ==

rem the card's own runtime: the image library beside the server imports the CUDA
rem runtime by bare name - cublas64_12, cudart64_12 - and a bare shell's PATH
rem carries the toolkit's bin folder on some boxes and not on others, so a
rem server started where it is not carried died the instant it started, reading
rem nothing at all. the folder goes in front of this script's own PATH, so it
rem reaches the server started below and is handed to nobody else. the Vulkan
rem shape names its driver manifest, and asks nothing of a toolkit.
if defined CUDA_ON if defined CUDA_HOME set "PATH=!CUDA_HOME!\bin;!PATH!"
if defined VK_DRIVER_DIR set "VK_DRIVER_FILES=!VK_DRIVER_DIR!"
rem two ggml copies share this process: the voice engine's, linked into the
rem binary, and the one whose plugin libraries sit beside it. ggml stops a
rem process that installs its crash handler twice - before it could listen - so
rem no copy here installs one, and the server starts. a crash still prints its
rem own words, in the same line the failing check names.
set "GGML_NO_BACKTRACE=1"
echo The server names the address it answers at as it starts - the line that reads
echo "API server on http://0.0.0.0:[port]". The configured port is !SRV_PORT!; if another
echo server already holds it, this one says so and does not start.
echo.
echo The first thing the server does is stand at the level this machine's manager
echo works at, because a boot entry, a task ended, a service started are the
echo manager's to take. Started by a plain person, that is one prompt - the
echo machine's own - and the person sitting at it answers it. A machine that
echo declines still runs every door and names the level it stands at; OLS_ELEVATE=off
echo declines the prompt on a machine that would otherwise ask.
api\open-local-services.exe %*
set SERVER_EXIT=%ERRORLEVEL%
rem this script hands the server to the window, so reaching here means the server
rem stopped: its own lines above name why, and the line it printed as it started
rem names where its pages were.
echo.
echo The server stopped here, and handed back %SERVER_EXIT%. Its address was the
echo line it printed as it started - "API server on http://0.0.0.0:[port]".
tasklist /FI "IMAGENAME eq open-local-services.exe" | findstr /I "open-local-services.exe" >nul
if not errorlevel 1 echo An open-local-services.exe is still in the task list: that other one may still be serving.
echo The page links to the home page, the image studio, and the voice
echo studio.
echo The server keeps running in this window; close the window to stop it.
ENDLOCAL
exit /b 0

:pin
rem %1 = engine folder, %2 = the commit it must be on, %3 = owner/name on
rem github. a folder already recorded at that commit is done; a git tree that
rem can see the commit is checked out; anything else takes that exact snapshot,
rem so both machines compile the same engine source.
if not exist "%~f1\.pin" goto :pin_norecord
for /f "usebackq delims=" %%P in ("%~f1\.pin") do set PINNED=%%P
if defined PINNED if "!PINNED!"=="%~2" exit /b 0
set PINNED=
:pin_norecord
if not exist "%~f1\.git" goto :pin_snapshot
git -C "%~f1" rev-parse --quiet --verify "%~2" >nul 2>nul
if errorlevel 1 goto :pin_fetch
git -C "%~f1" checkout --quiet --detach "%~2"
if errorlevel 1 goto :pin_fetch
goto :pin_done
:pin_fetch
git -C "%~f1" fetch --quiet --depth 1 origin "%~2"
if errorlevel 1 goto :pin_snapshot
git -C "%~f1" checkout --quiet --detach "%~2"
if errorlevel 1 goto :pin_snapshot
goto :pin_done
:pin_snapshot
echo Taking the exact source of %~2 for %~1
if not exist "%~f1" mkdir "%~f1"
curl -sSL --fail --retry 2 -o "%~f1\snapshot.tgz" "https://codeload.github.com/%~3/tar.gz/%~2"
if errorlevel 1 exit /b 1
tar -xzf "%~f1\snapshot.tgz" -C "%~f1" --strip-components=1
if errorlevel 1 exit /b 1
del /q "%~f1\snapshot.tgz"
:pin_done
echo %~2> "%~f1\.pin"
exit /b 0

:pin_sub
rem %1 = the repository that carries this directory as a submodule, %2 = the
rem directory's name inside it, %3 = the commit the submodule must stand on. a
rem submodule is a pointer in the parent's tree, so git is what puts it there:
rem it takes the one source that pointer names, at the commit the parent records.
rem a tarball of the same source is a second copy that only unpacks as well as
rem the operating system lets it, which is not enough for a tree holding names
rem this system cannot make.
if exist "%~f1\%~2\.pin" (
  for /f "usebackq delims=" %%P in ("%~f1\%~2\.pin") do set PINNED=%%P
  if defined PINNED if "!PINNED!"=="%~3" exit /b 0
  set PINNED=
)
rem a directory that is not a checkout of that submodule - a half-unpacked copy
rem left by another attempt - is taken down first, so git can put the real one
rem there rather than refusing a non-empty folder.
if not exist "%~f1\%~2\.git" rmdir /s /q "%~f1\%~2" 2>nul
git -C "%~f1" submodule update --init --checkout --quiet -- "%~2"
if errorlevel 1 (
  echo The repository %~1 carries no submodule at %~2 that git can reach.
  exit /b 1
)
git -C "%~f1\%~2" rev-parse --quiet --verify "%~3" >nul 2>nul
if errorlevel 1 (
  echo The submodule at %~2 cannot be put on %~3.
  exit /b 1
)
git -C "%~f1\%~2" checkout --quiet --detach "%~3"
if errorlevel 1 (
  echo The submodule at %~2 cannot be put on %~3.
  exit /b 1
)
echo %~3> "%~f1\%~2\.pin"
exit /b 0

:stop_process
rem %1 = an image name to stop. a process that was just asked to stop can keep
rem its image for a while - the dlls it mapped have to let go first - and a
rem name whose image still lives cannot be replaced. so this asks once, then
rem waits until the process is really gone; it answers 0 when it stopped one
rem and 1 when there was nothing to stop. the wait is bounded: a process that
rem will not let go is left alone, and the replace's own waits decide after.
tasklist /FI "IMAGENAME eq %1" | findstr /I "%1" >nul
if errorlevel 1 exit /b 1
taskkill /F /IM %1 >nul 2>nul
set LEFT=0
:stop_wait
tasklist /FI "IMAGENAME eq %1" | findstr /I "%1" >nul
if errorlevel 1 exit /b 0
set /A LEFT=LEFT+1
if %LEFT% geq 60 (
    echo This shell cannot stop that server: it stands at the level this machine's
    echo manager works at, and stopping it is the manager's - the page's stop hand,
    echo the machine's own schtasks /end, or a shell that carries the same leave.
    exit /b 2
)
ping -n 5 >nul
goto stop_wait

:replace
rem %1 = the file the build produced, %2 = the runtime folder that holds it.
rem a process that was just stopped can still hold its image for a while -
rem with the Vulkan and ggml dlls mapped it can take tens of seconds to let
rem go - so the copy is refused while that image lives. the name is taken
rem away first - unlinking it works while the old image is still open - and
rem the fresh file is put in its place, so the wait is only for the moment
rem when even that is refused.
set TRY=0
:replace_try
set /A TRY=TRY+1
copy /Y %1 %2 >nul
if not errorlevel 1 exit /b 0
del /q "%2%~nx1" 2>nul
copy /Y %1 %2 >nul
if not errorlevel 1 exit /b 0
if %TRY% geq 20 exit /b 1
ping -n 5 >nul
goto replace_try

:find_vulkan_sdk
rem the SDK the Vulkan shape asks for: prefer the one the machine actually has,
rem then the registry, then the manifest; last resort the root that sits one
rem level above the bin folder of its glslang tool. what is found is left in
rem VULKAN_DIR; nothing is said for a machine that carries none, because the
rem caller is the one with the words that stop a run.
set VULKAN_DIR=
if defined VULKAN_ROOT_DIR if exist "%VULKAN_ROOT_DIR%\Include\*" set VULKAN_DIR=%VULKAN_ROOT_DIR%
if not defined VULKAN_DIR if defined VULKAN_SDK if exist "%VULKAN_SDK%\Include\*" set VULKAN_DIR=%VULKAN_SDK%
if not defined VULKAN_DIR (
  for %%R in ("C:\VulkanSDK" "C:\Program Files\VulkanLoader" "C:\Program Files\Vulkan SDK") do (
    for /f "delims=" %%V in ('dir /ad /o:n /b "%%~R" 2^>nul') do (
      if exist "%%~R\%%V\Include\*" set VULKAN_DIR=%%~R\%%V
    )
  )
)
if not defined VULKAN_DIR (
  for /f "delims=" %%L in ('reg query "HKLM\SOFTWARE\Khronos\Vulkan SDK" /v Versions 2^>nul') do (
    for /f "tokens=2 delims=  " %%V in ("%%L") do if not defined VULKAN_DIR if exist "%%V\Include\*" set VULKAN_DIR=%%V
  )
)
rem the sdk root is one level above the bin folder of its glslang tool
for /f "usebackq tokens=*" %%i in (`where glslangValidator 2^>nul`) do (
  for %%S in ("%%~fi\..\Include\*") do if exist "%%~fS" for %%P in ("%%~fi\..") do set VULKAN_DIR=%%~fP
)
exit /b 0

:find_vk_driver
rem the newest driver-store package that carries a vulkan manifest: without it
rem the SDK loader points at a stale directory and reports no drivers.
set VK_DRIVER_DIR=
for /f "delims=" %%D in ('dir /b /ad /o-d "%SystemRoot%\System32\DriverStore\FileRepository\*inf_*" 2^>nul') do (
  if not defined VK_DRIVER_DIR if exist "%SystemRoot%\System32\DriverStore\FileRepository\%%D\amd-vulkan64.json" (
    set "VK_DRIVER_DIR=%SystemRoot%\System32\DriverStore\FileRepository\%%D"
  )
)
if defined VK_DRIVER_DIR (
  echo Vulkan driver manifest: !VK_DRIVER_DIR!
) else (
  echo No vulkan driver manifest in the driver store; the gpu chip will read no gpu driver.
)
exit /b 0

:ask_shape
rem %1 = a CMakeCache.txt, %2 = the name a shape is decided by, %3 = the value
rem this shape asks for, %4 = what the tree builds. :shape_ok answers whether that
rem cache agrees; one answering with another value was configured for another
rem shape, and restating that shape rewrites the compile command of every object
rem the tree holds - a full rebuild, which this path does not do silently. so
rem this answers 0 when the tree already holds the shape asked of it, and when
rem fresh-full-rebuild was given and the restatement is what the caller meant,
rem and 1 when the run has to stop; the refusal's words name the tree and the
rem two values that do not agree.
call :shape_ok %1 %2 %3
if not errorlevel 1 exit /b 0
if defined FRESH_FULL (
  echo fresh-full-rebuild was given: restating %~2 on this tree.
  exit /b 0
)
call :refuse_full_rebuild %4 %~2
exit /b 1

:shape_ok
rem %1 = a CMakeCache.txt, %2 = the name a shape is decided by, %3 = the value
rem this shape asks for. a name this shape does not ask for - an empty value -
rem belongs to the other backend alone, and a name no tree of this shape ever
rem carried says nothing about this one, so either answers "matches". the answer
rem of a name that both sides hold is whether they are the same value: a cache
rem that answers with another value was configured for another shape, and
rem restating that shape changes the compile commands of every object the tree
rem holds. the value found is left in SHAPE_HAS for the words that refuse it.
rem the cache types these names as it likes, and a name stands before the first
rem colon, so the name is matched whole rather than by a prefix.
set SHAPE_HAS=
if "%~3"=="" exit /b 0
for /f "usebackq tokens=1,* delims==" %%A in ("%~f1") do (
  for /f "tokens=1 delims=:" %%K in ("%%A") do (
    if /I "%%K"=="%~2" set SHAPE_HAS=%%B
  )
)
if not defined SHAPE_HAS exit /b 0
if "%SHAPE_HAS%"=="%~3" exit /b 0
exit /b 1

:refuse_full_rebuild
rem %1 = what the tree builds, %2 = the name whose values did not match. the
rem words name both values so the caller can see which shape stands here.
echo %1 would be rebuilt from scratch: its cache names
echo   %2 = !SHAPE_HAS! and this run asks another value.
echo Restating it recompiles everything the tree holds, which is a full rebuild
echo - this path does not do one silently, and nothing here clears a tree.
echo To rebuild that shape on purpose, run again with fresh-full-rebuild; to
echo keep what is here, name the shape this tree already holds.
exit /b 1
