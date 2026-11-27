@echo off
setlocal enabledelayedexpansion
rem open-local-services release build for Windows.
rem
rem run.bat builds and runs the server on this machine, in the development way:
rem its version is dev, and the binary carries the moment the compiler ran. this
rem script builds the portable artifacts that get committed, one folder per
rem target. the server itself is one shapeless build - it links no engine and
rem no backend - and each target ships it beside one engine library, so every
rem folder is a machine's shape stated at its configure rather than left to
rem the machine that compiled it:
rem
rem   Release\x64-windows\      the server and stable-diffusion-cpu-avx512.dll:
rem                             a cpu engine built up to avx512, with vbmi, vnni
rem                             and bf16 - for a cpu that reports those, like
rem                             the one this box sits on. the server opens that
rem                             library only on a chip that answers those
rem                             instruction sets.
rem
rem   Release\x64-windows-plain\
rem                             the same server and stable-diffusion-cpu.dll,
rem                             the cpu engine kept at the x64 baseline of avx2
rem                             - what an older laptop cpu runs, and what the
rem                             server opens by default for a machine with no
rem                             card.
rem
rem   Release\x64-cuda\         the same server and stable-diffusion-cuda.dll:
rem                             one CUDA engine whose kernels cover every card
rem                             generation this project supports - sm_70 (a
rem                             V100), sm_75 (an RTX 20 series, the card in the
rem                             laptop down the network) and sm_86 (an RTX 30
rem                             series) - the loader inside the library picks
rem                             each card's own kernels when it opens, so one
rem                             library answers every one of those machines;
rem                             the linux side builds the same one.
rem
rem each folder holds open-local-services.exe, the one engine library its
rem machine opens by name, the webui folder, and the runtimes those two really
rem import - nothing is compiled and nothing is fetched on the machine that
rem receives it.
rem
rem this box compiles every one of those targets, and a machine far down the
rem network never compiles anything: what is sent there is a finished folder,
rem which is started and tested as it arrived.
rem
rem what the artifacts guarantee:
rem   - nothing is generated in the folder this script sits in. the engines are
rem     built in the vendor trees this code already keeps, one build root per
rem     target, and every artifact ends up under Release\<target>\.
rem   - no source is copied or duplicated anywhere: the release build compiles
rem     the trees that are here, the ones the running server runs.
rem   - the server carries no engine and no ggml of its own. the image engine
rem     beside it is the one library that carries ggml, and the server opens
rem     it by name when it starts - which is what keeps one process from ever
rem     holding two ggmls. the same rule run.bat and src\CMakeLists.txt follow.
rem   - a build never throws a finished tree away. its roots are kept, so a run
rem     that changed no source recompiles nothing but the one file that carries
rem     the version, and a run that changed one file recompiles that file and
rem     the link. rebuilding code that did not change is a defect, not a cost.
rem   - the version every artifact here carries is the commit its code stands on,
rem     which this script reads only after checking that the code is committed.
rem
rem how a release works here, in plain words - there is no release server, no
rem pipeline, and no version numbers, and that is the whole design:
rem   1. the code is committed first. the release refuses an uncommitted tree,
rem      because a released binary that carries a commit older than its own
rem      code would lie about what it is.
rem   2. this script builds each shape and stamps the binary with that commit.
rem      the commit hash is the version - the only version this project has.
rem   3. each finished folder is written beside the source as one zip, together
rem      with its sha256 in Release\SHA256SUMS-win.txt, and the archives are
rem      committed. the repository is the release: what is on a machine is what
rem      the tree says it is, and nothing else publishes anything.
rem   4. a machine that compiles nothing takes one archive, opens it where it
rem      stands, and runs the binary inside - the cuda runtime it imports and
rem      the microsoft c++ runtime it needs travel beside it, read off the
rem      import tables rather than guessed at. a weights\ folder and a data\
rem      folder beside the binary are the same folders the development tree
rem      uses; a .env beside it is read the same way run.bat reads one.
rem
rem usage: build-rel.bat                build every windows target this box can
rem        build-rel.bat build          the same: this script never starts a server
rem        build-rel.bat cpu-avx512     only that target
rem        build-rel.bat cpu-plain      only the target for a cpu with no avx512
rem        build-rel.bat cuda           only the CUDA target, one library for
rem                             sm_70, sm_75 and sm_86 (cuda=70 and the like are
rem                             still accepted and name that one library)
rem        OLS_MSVC_DIR=<the folder a cl.exe sits in>
rem                             name the compiler this build uses, the way the
rem                             Vulkan SDK is named by VULKAN_ROOT_DIR.
rem        CUDA_HOME=<the folder a CUDA toolkit sits in>
rem                             name a toolkit the scan below would not find.
cd /d "%~dp0"

rem a released version is the commit its code stands on, so the code has to be
rem committed before it can be released: an uncommitted tree would carry a
rem version that is not the code inside the binary.
set DIRTY=
for /f "delims=" %%L in ('git status --porcelain -- src webui 2^>nul') do set DIRTY=%%L
if defined DIRTY (
  echo the code in src and webui is not committed - the lines below name what
  echo changed. a release version is a commit, so this build is refused. commit
  echo the code, then build again.
  git status --short -- src webui
  exit /b 1
)
set CODE_VERSION=
for /f "delims=" %%L in ('git rev-parse HEAD 2^>nul') do set CODE_VERSION=%%L
if not defined CODE_VERSION (
  echo this tree is not a git tree, so the release would carry no version.
  echo commit the code in a git tree first.
  exit /b 1
)
echo the version every artifact here carries: !CODE_VERSION!

set REL=Release
if not exist "%REL%" mkdir "%REL%"
rem each run writes the archives of the artifacts and the hashes of those
rem archives: one file per target to push over the network, and one file that
rem says what the bytes of every one of them are.
if exist "%REL%\SHA256SUMS-win.txt" del "%REL%\SHA256SUMS-win.txt"

rem the exact vendor commits this code is written against - the same names run.bat
rem carries, so an artifact is the source the running server runs.
set PIN_IMAGE=b13a48219d5cdf408edcb93da6cd69b612ed03d3
set PIN_IMAGE_GGML=dc998771e700d49bbdf9a39cede22bb5b3311767

echo.
echo == looking for the toolchain ==
set CMAKE_OK=
for /f "delims=" %%L in ('where cmake 2^>nul') do set CMAKE_OK=1
if not defined CMAKE_OK (
  echo Could not find cmake on PATH.
  exit /b 1
)
rem prefer the sdk the machine actually has, then the registry, then the manifest
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
rem the CUDA targets ask nothing of the Vulkan sdk - nvcc, ninja and the studio
rem environment are their whole toolchain. only a target that names the Vulkan
rem backend needs the sdk, and such a target refuses for itself when it is asked.
if defined VULKAN_DIR (
  set VULKAN_INCLUDE_DIR=%VULKAN_DIR%\include
  set VULKAN_LIB_DIR=%VULKAN_DIR%\lib
  echo Vulkan SDK found at !VULKAN_DIR!
) else (
  echo No Vulkan SDK here; the CUDA targets build without one, a Vulkan target will refuse for itself.
)
set MSVC_BIN=
rem the compiler can be named the way the sdk can: a box with more than one
rem toolchain picks the one its last release was built with, and what is named
rem is checked before it is used.
if defined OLS_MSVC_DIR if exist "%OLS_MSVC_DIR%\cl.exe" set "MSVC_BIN=%OLS_MSVC_DIR%"
if defined OLS_MSVC_DIR if not defined MSVC_BIN echo the compiler named in OLS_MSVC_DIR has no cl.exe there - looking for one instead.
if defined MSVC_BIN goto :msvc_found
for /d %%E in ("C:\Program Files\Microsoft Visual Studio\*" "C:\Program Files \(x86\)\Microsoft Visual Studio\*") do (
  for /d %%R in ("%%~E\*" "%%~E\Current") do (
    if exist "%%~R\VC\Tools\MSVC\*" (
      for /f "delims=" %%V in ('dir /b /ad "%%~R\VC\Tools\MSVC" 2^>nul') do (
        if exist "%%~R\VC\Tools\MSVC\%%V\bin\Hostx64\x64\cl.exe" set "MSVC_BIN=%%~R\VC\Tools\MSVC\%%V\bin\Hostx64\x64"
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
rem a CUDA build is driven by ninja, and ninja needs the studio environment; the
rem studio root is the folder seven levels above the compiler this box just found,
rem named here by the vcvars64.bat that sits beside its VC folder - a bin\Hostx64
rem\x64 folder sits under VC\Tools\MSVC\<version> under it, six steps, and the
rem edition root is the one above that.
set VS_ROOT=
for %%P in ("!MSVC_BIN!") do for %%Q in ("%%~fP\..\..\..\..\..\..\..") do if exist "%%~fQ\VC\Auxiliary\Build\vcvars64.bat" set VS_ROOT=%%~fQ
set CUDA_HOME=
if defined CUDA_HOME if not exist "%CUDA_HOME%\bin\nvcc.exe" (
  echo the toolkit named in CUDA_HOME has no nvcc in it - looking for one instead.
  set CUDA_HOME=
)
for /d %%C in ("C:\Program Files\NVIDIA CUDA*" "C:\Program Files (x86)\NVIDIA CUDA*" "C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\*" "C:\Program Files (x86)\NVIDIA GPU Computing Toolkit\CUDA\*") do (
  if exist "%%~C\bin\nvcc.exe" set CUDA_HOME=%%~C
)
for /f "delims=" %%L in ('where nvcc 2^>nul') do (
  if not defined CUDA_HOME for %%P in ("%%~fL\..") do set CUDA_HOME=%%~fP
)
set NINJA_EXE=
for /f "delims=" %%L in ('where ninja 2^>nul') do if not defined NINJA_EXE set NINJA_EXE=%%~fL
if not defined NINJA_EXE for /f "delims=" %%L in ('dir /b /s "C:\Program Files\CMake\bin\ninja.exe" 2^>nul') do (
  if not defined NINJA_EXE set NINJA_EXE=%%~fL
)
rem the studio that carries the compiler found above carries its own ninja
rem beside its cmake tools - the one edition this box's toolchain comes from.
if not defined NINJA_EXE if defined VS_ROOT if exist "!VS_ROOT!\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe" set NINJA_EXE=!VS_ROOT!\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe
rem the folder ninja sits in, so a nested configure a vendored project runs
rem can find the build program on PATH without anyone naming it again.
set NINJA_DIR=
if defined NINJA_EXE for %%P in ("!NINJA_EXE!\..") do set NINJA_DIR=%%~fP
if defined CUDA_HOME if defined NINJA_EXE (
  echo CUDA toolkit at !CUDA_HOME!, ninja at !NINJA_EXE!: a CUDA target can be built here.
) else (
  echo No CUDA toolkit, or no ninja to drive one: the CUDA targets cannot be built on this box.
)

rem the engine sources have to be here, and on the commits this code is written
rem against: run.bat fetches and pins them, and records each pin in the .pin
rem file of the tree. this build reads that record rather than taking another
rem copy of the source - the source it compiles is the source that is here.
for %%S in (vendor\stable-diffusion.cpp) do (
  if not exist "%%~S\CMakeLists.txt" (
    echo This tree has no engine sources. run.bat fetches and pins them first.
    exit /b 1
  )
)
call :check_pin "vendor\stable-diffusion.cpp" "!PIN_IMAGE!"
if errorlevel 1 exit /b 1
call :check_pin "vendor\stable-diffusion.cpp\ggml" "!PIN_IMAGE_GGML!"
if errorlevel 1 exit /b 1

rem which targets to build. named ones are built alone; nothing named means
rem every target this box can honestly build - the two cpu flavours always, and
rem a card target only when the toolkit and the builder the CUDA path needs were
rem both found above.
set REQUESTED=
set CUDA_ASKED=
set BAD_ARG=
rem the one word that lets a target restate a tree whose cache names another
rem shape - the shape check below refuses without it.
set FRESH_FULL=
rem the names hold an = (cuda=75), and the command interpreter splits a word on
rem = before this script sees it - a for-in list and the arguments of a call
rem split it the same way. so the names are taken one at a time with shift, the
rem card family and the number that followed it are put back together, and the
rem whole name is kept in CUR_ARG for the reader below.
:next_arg
if "%~1"=="" goto :args_read
if /I "%~1"=="cuda" if not "%~2"=="" (
  set "CUR_ARG=%~1=%~2"
  call :parse_arg
  shift
  shift
  goto :next_arg
)
set "CUR_ARG=%~1"
call :parse_arg
shift
goto :next_arg
:args_read
if defined BAD_ARG (
  echo this script does not know a target name - the names are cpu-avx512,
  echo cpu-plain, and cuda ^(one CUDA engine for sm_70, sm_75 and sm_86^).
  exit /b 1
)
if not defined REQUESTED (
  set REQUESTED=cpu-avx512 cpu-plain
  if defined CUDA_HOME if defined NINJA_EXE set "REQUESTED=!REQUESTED! cuda"
)
if defined CUDA_ASKED (
  if not defined CUDA_HOME (
    echo a CUDA target was asked for and no toolkit is here - install one from its
    echo .exe rather than carrying an extracted tree over the network.
    exit /b 1
  )
  if not defined NINJA_EXE (
    echo a CUDA target was asked for and there is no ninja to drive it.
    exit /b 1
  )
)
rem the shape this box itself runs, read off the box: the staged room bin\ must
rem hold what the serve standing here reads. a card answers with the cuda
rem shape - the one library covers its generation whatever that generation is;
rem without a card this box runs the plain cpu shape - never the avx512 one,
rem which asks more of the machine than plain code does.
set BOX_CAP=
for /f "delims=" %%C in ('nvidia-smi --query-gpu=compute_cap --format=csv,noheader 2^>nul') do if not defined BOX_CAP set BOX_CAP=%%C
if defined BOX_CAP set BOX_CAP=!BOX_CAP:.=!
if defined BOX_CAP (set BOX_SHAPE=cuda) else set BOX_SHAPE=cpu-plain
if defined BOX_CAP echo this box runs the cuda shape: its card answers compute !BOX_CAP!, and one cuda library covers it
rem the card generations this project supports, checked against what nvcc here
rem says it knows rather than assumed: the one cuda engine carries the kernels
rem for every one of these the toolkit can write, and a generation it cannot
rem write for is named in this summary rather than left out quietly.
set CUDA_WANTED_LIST=70 75 86
set CUDA_ARCH_LIST=
set CUDA_HAVE=
if defined CUDA_HOME for /f "delims=" %%L in ('"!CUDA_HOME!\bin\nvcc.exe" --list-gpu-arch 2^>nul') do set "CUDA_HAVE=!CUDA_HAVE! %%~fL"
if defined CUDA_HOME for %%A in (!CUDA_WANTED_LIST!) do (
  echo !CUDA_HAVE! | findstr /L "compute_%%A" >nul
  if not errorlevel 1 (
    if defined CUDA_ARCH_LIST (set "CUDA_ARCH_LIST=!CUDA_ARCH_LIST!;%%A") else set CUDA_ARCH_LIST=%%A
  ) else (
    set "CUDA_MISSING=!CUDA_MISSING! sm_%%A"
    echo this CUDA toolkit cannot write sm_%%A: that generation will not be in the cuda engine.
  )
)
if not defined CUDA_ARCH_LIST echo this CUDA toolkit can write none of the supported generations: the cuda target has nothing to carry.
echo the cuda engine of this run will carry: sm_!CUDA_ARCH_LIST:;=.  sm_!
rem the server builds once for the whole matrix: it links no engine, so no
rem target changes a single compile command in its tree.
set SERVER_ROOT=build
set SERVER_EXE=!SERVER_ROOT!\Release\open-local-services.exe
if exist "!SERVER_ROOT!\open-local-services.exe" set SERVER_EXE=!SERVER_ROOT!\open-local-services.exe
call :build_server
if errorlevel 1 (
  echo the server itself did not build.
  exit /b 1
)
echo the targets this run builds: !REQUESTED!

for %%T in (!REQUESTED!) do (
  set TARGET=%%T
  call :pick_target
  if errorlevel 1 (
    echo that target cannot be built on this box.
    exit /b 1
  )
  call :build_target
  if errorlevel 1 (
    echo the target !TARGET! did not build.
    exit /b 1
  )
)

rem what the artifacts carry, in the note the README points at
(
  echo the release artifacts of open-local-services, built by build-rel.bat
  echo version: the code of every artifact here is the commit !CODE_VERSION!
  echo with cmake and the msvc cl.exe at !MSVC_BIN!, and the vulkan sdk at !VULKAN_DIR!
  echo.
  echo each folder below is one machine shape, built here and started as it arrived:
  echo   x64-windows: the server with the cpu engine built up to avx512 with vbmi,
  echo     vnni and bf16 - the engine is stable-diffusion-cpu-avx512.dll, for a
  echo     cpu that reports those instruction sets.
  echo   x64-windows-plain: the same server with the cpu engine kept at the x64
  echo     baseline of avx2 - stable-diffusion-cpu.dll, for a cpu with no
  echo     avx512 at all.
  echo   x64-cuda: the same server and one CUDA engine - stable-diffusion-cuda.dll -
  echo     whose kernels carry every card generation this project supports:
  echo     sm_70 for the compute 7.0 cards ^(a V100^), sm_75 for the compute 7.5
  echo     cards ^(an RTX 20 series, the card in the laptop down the network^) and
  echo     sm_86 for the compute 8.6 cards ^(an RTX 30 series^). the loader inside
  echo     the library picks each card's own kernels when the server opens it,
  echo     so one library answers every one of those machines; the linux side
  echo     builds the same one.
  echo inside each folder: open-local-services.exe, which links no engine at all,
  echo and the image engine beside it under the name the server opens it by.
  echo the engine carries the ggml the pictures are drawn with, and one process
  echo cannot hold two ggmls - so the server holds none.
  echo the build roots of every target are kept between runs: a run that changed
  echo no source recompiles nothing but the file that carries the version.
  echo card generations this CUDA toolkit could not write: !CUDA_MISSING!
  echo the card generations read back off the cuda engine are printed by the
  echo run, not trusted from what was asked, beside the folders it built.
  echo the engines were compiled in the vendor trees this code keeps, on the
  echo pinned commits its .pin files record - no source was copied for this
  echo build. the sources that built every artifact here:
  echo     image engine: vendor\stable-diffusion.cpp at !PIN_IMAGE!
  echo     image ggml: vendor\stable-diffusion.cpp\ggml at !PIN_IMAGE_GGML!
  echo.
  echo the release of each target is one zip beside this file, named for the
  echo commit its binary carries - open-local-services-!CODE_VERSION:~0,7!-win-^<folder^>.zip -
  echo and the sha256 of every zip stands in Release\SHA256SUMS-win.txt. inside
  echo each folder: the cuda runtime the binaries really import and the
  echo microsoft c++ runtime, carried beside the server rather than left to
  echo the machine that receives it, and a webui folder served live the way
  echo the development tree's is. open such a folder where it stands and run
  echo it; nothing is compiled and nothing is fetched on that machine.
) > "%REL%\build-info.txt"

echo.
echo Release\build-info.txt now says what every artifact here carries.
rem the repository is the release, so an archive that stands here uncommitted
rem is a release that has not happened yet. the script never commits anything -
rem it reads the archive list off git and names the ones the tree has not been
rem shown, so no finished binary is ever left only on this machine.
set UNCOMMITTED=
for /f "delims=" %%F in ('git status --porcelain -- Release 2^>nul') do (
  set UNCOMMITTED=1
  echo   %%F
)
if defined UNCOMMITTED (
  echo the archives above are built but not in the repository yet - commit them,
  echo the same commit discipline as the code: what is committed is what runs.
) else (
  echo every archive under Release\ is committed: the release is the repository.
)
echo The engine trees are now built the release way; run.bat puts them back the
echo development way when it runs.
exit /b 0

rem ---- the server, once for the whole matrix ----
rem it links no engine and no backend, so one tree answers for every target;
rem the version it carries is the commit this script checked. a cache that
rem already names this very version needs no touch, and a cache left over from
rem the days the server linked the engine wants restating too.
:build_server
call :stand_studio
if not exist "!SERVER_ROOT!\CMakeCache.txt" (
  cmake -S src -B !SERVER_ROOT! -DCMAKE_BUILD_TYPE=Release -DOLS_VERSION=!CODE_VERSION!
  if errorlevel 1 (
    echo Could not configure the server build.
    exit /b 1
  )
) else (
  set CACHE_VERSION=
  for /f "usebackq tokens=1,* delims==" %%A in ("!SERVER_ROOT!\CMakeCache.txt") do (
    set CACHE_KEY=%%A
    if /I "!CACHE_KEY:~0,11!"=="OLS_VERSION" set CACHE_VERSION=%%B
  )
  set NEEDS_RENAME=
  if not "!CACHE_VERSION!"=="!CODE_VERSION!" set NEEDS_RENAME=1
  findstr /I /C:"OLS_SD_BUILD" "!SERVER_ROOT!\CMakeCache.txt" >nul 2>&1 && set NEEDS_RENAME=1
  if defined NEEDS_RENAME (
    echo the server cache names another version or the old engine link; restating it
    cmake -S src -B !SERVER_ROOT! -DCMAKE_BUILD_TYPE=Release -DOLS_VERSION=!CODE_VERSION!
    if errorlevel 1 (
      echo Could not configure the server build.
      exit /b 1
    )
  )
)
cmake --build !SERVER_ROOT! --config Release --parallel
if errorlevel 1 (
  echo The server build failed.
  exit /b 1
)
if not exist "!SERVER_EXE!" (
  echo open-local-services.exe is not there - the server build above did not produce it.
  exit /b 1
)
exit /b 0

rem ---- the shape of one engine target: its build roots are named for it, so
rem two shapes of one engine tree never share a cache, and its instruction
rem sets and its backend are stated here rather than taken from the machine
rem that built it. the engine library it produces is named for its shape: the
rem server opens it by that name, so a box can hold more than one and the
rem engine it runs is a choice the machine makes at start ----
:pick_target
set SD=vendor\stable-diffusion.cpp
rem the pinned image engine declares rsqrt as a device function where the CUDA 12.9
rem headers have already made that name a macro. the guard steps that declaration
rem aside at configure time when the macro is there and leaves it plain when it is
rem not, so the same source builds on both. run.sh and build-rel.sh have passed it
rem since the UM780 showed what happens without it (pitfall 18 of local_environment)
set OLS_GUARD=-DCMAKE_PROJECT_INCLUDE_BEFORE="%CD%\src\ols-sage-guard.cmake"
rem the engine trees carry their build roots deep inside the vendored ggml, past
rem the point where the Visual Studio task runner can open its task logs - so
rem they are configured with ninja where it is found, the same builder the
rem CUDA shape and the .sh side already drive. without ninja the trees fall
rem back to the Visual Studio generator and its limits.
set ENGINE_GEN=
if defined NINJA_EXE set ENGINE_GEN=-G Ninja "-DCMAKE_MAKE_PROGRAM=!NINJA_EXE!"
rem the studio environment is stood once per run by :stand_studio below, not
rem by every place that once reached for vcvars: each call prepends vcvars'
rem folders to PATH again, and by the third target the PATH line passes the
rem eight thousand characters cmd draws the line at - the release of a whole
rem matrix died on exactly that. a ninja builder needs no PATH entry either:
rem the configure lines hand it over whole by -DCMAKE_MAKE_PROGRAM, and the
rem build reads that absolute name from the cache.
set SD_ROOT=
set OUT=
set ENGINE_NAME=
set CPU_FLAGS=
set AVX512_WANT=
set CUDA_ARCH=
set CUDA_ON=
if /I "!TARGET!"=="cpu-avx512" (
  set SD_ROOT=vendor\stable-diffusion.cpp\build
  set OUT=%REL%\x64-windows
  set ENGINE_NAME=stable-diffusion-cpu-avx512.dll
  rem the instruction-set ceiling for this shape: everything the newest machines
  rem carry except avx512-bf16, which is not a CPU question at all - the bf16
  rem kernels of the vendored engines cast through MSVC's strict __m512bh type
  rem and only compile under gcc. the runtime dispatch still asks the CPU every
  rem remaining question; this only says what the compiler here is able to answer.
  set CPU_FLAGS=-DGGML_NATIVE=OFF -DGGML_AVX512=ON -DGGML_AVX512_VBMI=ON -DGGML_AVX512_VNNI=ON -DGGML_AVX512_BF16=OFF
  set AVX512_WANT=ON
  set BACKEND=cpu up to avx512
  exit /b 0
)
if /I "!TARGET!"=="cpu-plain" (
  set SD_ROOT=vendor\stable-diffusion.cpp\build-p
  rem this engine's ggml writes the target's instruction sets into the objects it
  rem compiles - so the plain shape cannot share a build tree with the avx512
  rem one, and its root takes the same -p name the image engine's plain root has
  rem always carried.
  set OUT=%REL%\x64-windows-plain
  set ENGINE_NAME=stable-diffusion-cpu.dll
  set CPU_FLAGS=-DGGML_NATIVE=OFF -DGGML_AVX512=OFF -DGGML_AVX512_VBMI=OFF -DGGML_AVX512_VNNI=OFF -DGGML_AVX512_BF16=OFF
  set AVX512_WANT=OFF
  set BACKEND=cpu at the x64 baseline
  exit /b 0
)
if /I not "!TARGET!"=="cuda" (
  rem the old per-generation names still arrive - cuda-70 and the like - and all
  rem of them name the one CUDA engine this project builds.
  if not "!TARGET:~0,5!"=="cuda-" exit /b 1
  echo cuda-!TARGET:~5! names the one cuda engine: it carries sm_70, sm_75 and sm_86.
)
set SD_ROOT=vendor\stable-diffusion.cpp\build-cuda
set OUT=%REL%\x64-cuda
set ENGINE_NAME=stable-diffusion-cuda.dll
set CPU_FLAGS=-DGGML_NATIVE=OFF -DGGML_AVX512=OFF -DGGML_AVX512_VBMI=OFF -DGGML_AVX512_VNNI=OFF -DGGML_AVX512_BF16=OFF
set CUDA_ON=1
set BACKEND=CUDA sm_!CUDA_ARCH_LIST:;= sm_!
exit /b 0

:build_target
echo.
echo == !TARGET!: building the image engine as a shared library, !BACKEND! ==
rem a root that stands here holds one backend - and for the cuda engine one
rem set of card generations - so this target's shape is asked of it before
rem cmake is: asking another would rewrite the compile commands of everything
rem the tree holds.
if exist "!SD_ROOT!\CMakeCache.txt" (
    if defined CUDA_ON (
        call :shape_ok "!SD_ROOT!\CMakeCache.txt" CMAKE_CUDA_ARCHITECTURES "!CUDA_ARCH_LIST!"
        if errorlevel 1 (
            if not defined FRESH_FULL (
                call :refuse_full_rebuild "the image engine" CMAKE_CUDA_ARCHITECTURES
                exit /b 1
            )
            echo fresh-full-rebuild was given: restating the shape on this tree.
        )
    )
)
if defined CUDA_ON (
  call :stand_studio
  cmake -G Ninja "-DCMAKE_MAKE_PROGRAM=!NINJA_EXE!" -S "%SD%" -B !SD_ROOT! -DCMAKE_BUILD_TYPE=Release -DSD_BUILD_SHARED_LIBS=ON -DSD_BUILD_CLI=OFF -DSD_BUILD_TEST=OFF -DSD_ENABLE_OPENVINO=OFF !CPU_FLAGS! -DGGML_VULKAN=OFF -DGGML_HIP=OFF -DGGML_COMMON=OFF -DGGML_OPENMP=OFF -DMATH_LIBRARY= -DUSE_GGML_SUBPROJECT=OFF -DGGML_SRC=ggml -DGGML_CUDA=ON "-DCMAKE_CUDA_COMPILER=!CUDA_HOME!\bin\nvcc.exe" "-DCUDAToolkit_ROOT=!CUDA_HOME!" "-DCMAKE_CUDA_ARCHITECTURES=!CUDA_ARCH_LIST!" !OLS_GUARD!
) else (
  call :stand_studio
  cmake !ENGINE_GEN! -S "%SD%" -B !SD_ROOT! -DCMAKE_BUILD_TYPE=Release -DSD_BUILD_SHARED_LIBS=ON -DSD_BUILD_CLI=OFF -DSD_BUILD_TEST=OFF -DSD_ENABLE_OPENVINO=OFF !CPU_FLAGS! -DGGML_VULKAN=ON -DGGML_HIP=OFF -DGGML_COMMON=OFF -DGGML_OPENMP=OFF -DMATH_LIBRARY= -DUSE_GGML_SUBPROJECT=OFF -DGGML_SRC=ggml !OLS_GUARD!
)
if errorlevel 1 (
  echo Could not configure the image engine for !TARGET!.
  exit /b 1
)
cmake --build !SD_ROOT! --config Release --parallel
if errorlevel 1 (
  echo The image engine build failed for !TARGET!.
  exit /b 1
)

rem ---- collect the artifacts and the libraries they really need ----
rem the engine is read from where either project builder puts it: the Visual
rem Studio one under a bin\Release folder, ninja a plain bin\. the server exe
rem came from the one shapeless server build above; it goes into every folder
rem whole, and the engine beside it is what makes this folder a machine shape.
set SD_BIN=!SD_ROOT!\bin\Release
if not exist "!SD_BIN!\stable-diffusion.dll" set SD_BIN=!SD_ROOT!\bin
if not exist "!OUT!" mkdir "!OUT!"
copy /Y "!SERVER_EXE!" "!OUT!\open-local-services.exe" >nul
if not exist "!OUT!\open-local-services.exe" (
  echo open-local-services.exe is not there - the server build above did not produce it.
  exit /b 1
)
copy /Y "!SD_BIN!\stable-diffusion.dll" "!OUT!\!ENGINE_NAME!" >nul
if not exist "!OUT!\!ENGINE_NAME!" (
  echo !ENGINE_NAME! is not there - the image engine build did not produce it.
  exit /b 1
)
rem the staged room, bin\, holds what this box runs: the server and the engine
rem of its shape. the last target of a full sweep is not necessarily that
rem shape, so staging asks the box's own shape alone; a person rebuilding this
rem box builds here, posts the hand, and the running server moves in what
rem stands staged while it is down - the binary and every engine library named
rem for the engine, together.
if not exist "bin" mkdir "bin"
if /I "!TARGET!"=="!BOX_SHAPE!" (
  copy /Y "!SERVER_EXE!" "bin\open-local-services.exe" >nul
  rem the server carries no ggml of its own; the engine library it will open
  goes beside it under its shape-name.
  copy /Y "!SD_BIN!\stable-diffusion.dll" "bin\!ENGINE_NAME!" >nul
  echo staged into bin\ for /api/update: the shape of this box, !TARGET!.
) else (
  echo !TARGET! is not the shape this box runs ^(!BOX_SHAPE!^): bin\ is left as it is.
)
rem the pages travel with the binary: a webui folder beside the server is served
rem from disk, so a release folder holds the live structure the development
rem folder holds - and the copy embedded in the binary only answers when the
rem folder is moved somewhere without one.
if exist "!OUT!\webui" rmdir /s /q "!OUT!\webui"
xcopy /Y /E /Q /I "webui" "!OUT!\webui\" >nul
rem the image library and the server share one process, so both have to be built
rem against the same runtime. an image library built for debugging imports the
rem debug CRT (ucrtbased.dll) while the server carries the release one, and a
rem buffer the debug heap handed out is not the release heap's to hand back: what
rem that leaves behind is a fail-fast (0xC0000409, printed as -1073740791) at the
rem moment a finished picture is handed over, which reads as a fault in the code
rem rather than in the flags a build was configured with. So the runtime is read
rem off the artifact here - the same way the card architecture below is - and a
rem debug-shaped image library stops the release rather than shipping in it.
"!MSVC_BIN!\dumpbin.exe" /DEPENDENTS "!OUT!\open-local-services.exe" > "!SD_ROOT!\ols-deps.txt" 2>nul
"!MSVC_BIN!\dumpbin.exe" /DEPENDENTS "!OUT!\!ENGINE_NAME!" >> "!SD_ROOT!\ols-deps.txt" 2>nul
findstr /L /C:"ucrtbased.dll" "!SD_ROOT!\ols-deps.txt" >nul
if not errorlevel 1 (
  echo !ENGINE_NAME! in !OUT! was built for debugging: it imports ucrtbased.dll.
  echo the image engine build must name -DCMAKE_BUILD_TYPE=Release; its build root is !SD_ROOT!,
  echo whose CMakeCache.txt states the build type that root was last configured with.
  exit /b 1
)
rem a cuda artifact that resolves its runtime from this box's toolkit is not a
rem release yet: the machine down the network holds a driver and nothing else.
rem what the two binaries really import is read off them - and every name the
rem toolkit can supply is laid beside the server, once each, as it is named.
rem the driver's own cuda.dll belongs to the machine that owns the card and is
rem never carried.
if defined CUDA_ON (
  set RUNTIME_SEEN=!SD_ROOT!\ols-runtime.txt
  if exist "!RUNTIME_SEEN!" del "!RUNTIME_SEEN!"
  for /f "usebackq tokens=*" %%L in ("!SD_ROOT!\ols-deps.txt") do (
    for %%W in (%%L) do (
      if /I "%%~xW"==".dll" (
        rem the toolkit lays its runtime dlls in bin and its import libraries in
        rem bin\x64 - the reverse of what this loop used to ask - so both places
        rem are offered, and the first that holds a name is the one that carries.
        if exist "!CUDA_HOME!\bin\x64\%%W" (
          (findstr /X /C:"%%W" "!RUNTIME_SEEN!" >nul 2>nul) || (
            echo %%W>> "!RUNTIME_SEEN!"
            copy /Y "!CUDA_HOME!\bin\x64\%%W" "!OUT!\" >nul
            echo   carried: %%W
          )
        ) else if exist "!CUDA_HOME!\bin\%%W" (
          (findstr /X /C:"%%W" "!RUNTIME_SEEN!" >nul 2>nul) || (
            echo %%W>> "!RUNTIME_SEEN!"
            copy /Y "!CUDA_HOME!\bin\%%W" "!OUT!\" >nul
            echo   carried: %%W
          )
        )
        if exist "!CUDA_HOME!\lib\x64\%%W" (
          (findstr /X /C:"%%W" "!RUNTIME_SEEN!" >nul 2>nul) || (
            echo %%W>> "!RUNTIME_SEEN!"
            copy /Y "!CUDA_HOME!\lib\x64\%%W" "!OUT!\" >nul
            echo   carried: %%W
          )
        )
      )
    )
  )
)
rem a machine that never held visual studio has no microsoft c++ runtime of its
rem own, and this release runs where nothing was installed - so the runtime
rem dlls come along beside the binary. the studio's own redist folder is asked
rem first, newest layout wins; where a studio installed its redist into the
rem system instead of carrying a folder - which is exactly what this box's
rem v145 studio did - the system's own copy answers, and it is the same
rem version these binaries were compiled against by that very compiler.
set REDIST_CRT=
for /d %%D in ("!VS_ROOT!\VC\Redist\MSVC\*") do (
  if exist "%%~D\MSVC\x64\Microsoft.VC145.CRT\msvcp140.dll" set REDIST_CRT=%%~D\MSVC\x64\Microsoft.VC145.CRT
  if exist "%%~D\MSVC\x64\Microsoft.VC143.CRT\msvcp140.dll" set REDIST_CRT=%%~D\MSVC\x64\Microsoft.VC143.CRT
  if exist "%%~D\redist\x64\Microsoft.VCRedist.143\msvcp140.dll" set REDIST_CRT=%%~D\redist\x64\Microsoft.VCRedist.143
)
for %%R in ("vcruntime140.dll" "vcruntime140_1.dll" "msvcp140.dll" "msvcp140_codecvt_ids.dll" "vcomp140.dll") do (
  if exist "!REDIST_CRT!\%%~R" (
    copy /Y "!REDIST_CRT!\%%~R" "!OUT!\" >nul
    echo   carried: %%~R
  ) else if exist "C:\Windows\System32\%%~R" (
    copy /Y "C:\Windows\System32\%%~R" "!OUT!\" >nul
    echo   carried: %%~R from the system runtime the compiler installed
  )
)
rem the carrying is not done until it is true: every import that a release
rem machine cannot be assumed to hold must now stand in the folder, checked
rem after the copies rather than hoped for - a zip that silently lacked its
rem cublas once went out believing it had one, and only the first machine
rem without a toolkit noticed.
for %%R in ("msvcp140.dll" "vcruntime140.dll" "vcruntime140_1.dll" "vcomp140.dll") do (
  if not exist "!OUT!\%%~R" (
    echo the release folder holds no %%~R - the redist of !VS_ROOT! did not supply it.
    exit /b 1
  )
)
if defined CUDA_ON for %%R in ("cudart64_12.dll" "cublas64_12.dll" "cublasLt64_12.dll" "cufft64_11.dll") do (
  if not exist "!OUT!\%%~R" (
    echo the cuda release folder holds no %%~R and would not start away from this box.
    exit /b 1
  )
)
rem what card generations a cuda engine really holds is read back off it
rem rather than trusted from what was asked: the tool that reads it sits beside
rem nvcc in the same toolkit, and it names every embedded cubin as
rem <name>.<n>.sm_<arch>.cubin - so the list is written to a file first,
rem since a command quoted inside a loop reads as nothing, and the distinct
rem generations are collected from those names, each one stated once. the
rem server itself holds no kernels - only the engine can answer this.
if defined CUDA_ON (
  echo the card generations read back from the engine of this folder:
  set ARCH_READ=
  "!CUDA_HOME!\bin\cuobjdump.exe" -lelf "!OUT!\!ENGINE_NAME!" > "!SD_ROOT!\ols-elf-list.txt" 2>nul
  for /f "usebackq tokens=2 delims=:" %%L in ("!SD_ROOT!\ols-elf-list.txt") do (
    for /f "tokens=3 delims=." %%S in ("%%L") do (
      echo !ARCH_READ! | findstr /L /C:"%%S" >nul 2>nul
      if errorlevel 1 set "ARCH_READ=!ARCH_READ! %%S"
    )
  )
  echo   !ENGINE_NAME!:!ARCH_READ!
)

rem the release of this target is one archive: the folder just above, named for
rem the commit its binary carries. that file is what gets committed beside the
rem source, what gets put on a machine, and what that machine opens where it
rem stands and runs - no builder, no installer, nothing fetched from anywhere.
rem its sha256 goes into the release's own SHA256SUMS-win.txt, so a copy that
rem arrived changed reads as changed before anyone runs it.
for %%P in ("!OUT!") do set OUT_NAME=%%~nP
set ARCHIVE=%REL%\open-local-services-!CODE_VERSION:~0,7!-win-!OUT_NAME!.zip
if exist "!ARCHIVE!" del "!ARCHIVE!"
tar -a -c -f "!ARCHIVE!" -C "%REL%" "!OUT_NAME!"
if errorlevel 1 (
  echo could not write the archive for !TARGET! - the folder stands, unarchived.
  exit /b 1
)
set ARCH_HASH=
certutil -hashfile "!ARCHIVE!" SHA256 2>nul | findstr /R "^[0-9A-Fa-f][0-9A-Fa-f]*$" > "!SD_ROOT!\ols-hash.txt"
for /f "usebackq delims=" %%H in ("!SD_ROOT!\ols-hash.txt") do set ARCH_HASH=%%H
if not defined ARCH_HASH (
  echo could not read the sha256 of !ARCHIVE! - certutil answered with nothing.
  exit /b 1
)
echo !ARCH_HASH!  open-local-services-!CODE_VERSION:~0,7!-win-!OUT_NAME!.zip>> "%REL%\SHA256SUMS-win.txt"
echo wrote !ARCHIVE!

exit /b 0

rem stand the studio environment in, once. every call to vcvars64.bat prepends
rem its folders to PATH again, so a release that stood it in once per target
rem arrives at the later targets with a PATH longer than cmd can carry in one
rem line - the whole matrix would have died at the first vulkan target. the
rem first caller stands it in for every caller after it.
:stand_studio
if defined OLS_STOOOD exit /b 0
if not defined VS_ROOT exit /b 0
call "!VS_ROOT!\VC\Auxiliary\Build\vcvars64.bat" >nul
set OLS_STOOOD=1
exit /b 0

rem one requested target name. the name is read from CUR_ARG, which the loop
rem above keeps whole: an argument that holds an = is split by a call as it is
rem split by a for-in list, so a name for a card architecture could not be
rem passed in as an argument at all. an unknown name only sets a flag, so every
rem bad word gets named before the run refuses.
:parse_arg
set "A=!CUR_ARG!"
if /I "%A%"=="build" exit /b 0
if /I "%A%"=="fresh-full-rebuild" (
  set FRESH_FULL=1
  exit /b 0
)
if /I "%A%"=="--fresh-full-rebuild" (
  set FRESH_FULL=1
  exit /b 0
)
if /I "%A%"=="cpu-avx512" (
  set "REQUESTED=!REQUESTED! cpu-avx512"
  exit /b 0
)
if /I "%A%"=="cpu-plain" (
  set "REQUESTED=!REQUESTED! cpu-plain"
  exit /b 0
)
if /I "%A%"=="cuda" (
  set "REQUESTED=!REQUESTED! cuda"
  set CUDA_ASKED=1
  exit /b 0
)
if /I "%A:~0,5%"=="cuda=" (
  rem the per-generation names from the old matrix: they all name the one CUDA
  rem engine now, which carries every supported generation.
  set "REQUESTED=!REQUESTED! cuda"
  set CUDA_ASKED=1
  exit /b 0
)
set BAD_ARG=1
exit /b 0

:check_pin
rem %1 = the tree, %2 = the commit this code expects it on. run.bat records the
rem pin it fetched in the .pin file of that tree; a tree that was not pinned
rem here carries no record of what it holds, and this build says so rather than
rem compiling a source it cannot name.
set PINNED=
for /f "usebackq delims=" %%P in ("%~f1\.pin") do set PINNED=%%P
if not "!PINNED!"=="%~2" (
  echo %~1 is not on %~2 - what is recorded there is !PINNED!, or nothing at all.
  echo run.bat fetches and pins the engine sources this code is written against;
  echo run that first, so this build compiles the source the server runs.
  exit /b 1
)
exit /b 0

:shape_ok
rem %1 = a CMakeCache.txt, %2 = the name a shape is decided by, %3 = the value
rem this shape asks for. the shape is asked before cmake is, so a tree of
rem another shape is refused rather than compiled again in full.
set SHAPE_HAS=
if "%~3"=="" exit /b 0
for /f "usebackq tokens=1,* delims==" %%A in ("%~f1") do (
  set "SHAPE_KEY=%%A"
  for /f "tokens=1 delims=:" %%K in ("!SHAPE_KEY!") do (
    if /I "%%K"=="%~2" set SHAPE_HAS=%%B
  )
)
if not defined SHAPE_HAS exit /b 0
if "%SHAPE_HAS%"=="%~3" exit /b 0
exit /b 1

:refuse_full_rebuild
rem %1 = what the tree builds, %2 = the name whose values did not match.
echo !TARGET!: this tree holds the shape of another build - its cache names
echo   %2 = !SHAPE_HAS! and this run asks for !CUDA_ARCH_LIST!.
echo Restating it would recompile everything the tree holds, a full rebuild,
echo which this path does not do silently. Nothing here clears a tree.
echo To rebuild that shape on purpose, run again with fresh-full-rebuild.
exit /b 1
