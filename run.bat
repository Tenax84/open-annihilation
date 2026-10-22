@echo off
rem SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
rem SPDX-License-Identifier: GPL-3.0-only

rem Build the current source and launch the native game on Windows with MSVC.
rem The build is incremental: when no source changed, Ninja does nothing and
rem the game starts at once. A failed build stops here and never launches an
rem older executable. Arguments are passed to open-annihilation unchanged.
rem
rem Environment: OA_BUILD_TYPE (default: Release), OA_BUILD_DIR (default:
rem build-<type>, build for Debug, the folders run.sh uses), OA_BUILD_JOBS
rem (default: 6), OA_GAME_DIR (the installation; without it the game uses the
rem folder chosen before, or asks for one).
setlocal EnableExtensions

set "repo_dir=%~dp0"
if "%repo_dir:~-1%"=="\" set "repo_dir=%repo_dir:~0,-1%"
if not defined OA_BUILD_TYPE set "OA_BUILD_TYPE=Release"
if not defined OA_BUILD_DIR set "OA_BUILD_DIR=%repo_dir%\build-%OA_BUILD_TYPE%"
if /i "%OA_BUILD_TYPE%"=="Debug" if "%OA_BUILD_DIR%"=="%repo_dir%\build-Debug" set "OA_BUILD_DIR=%repo_dir%\build"
if not defined OA_BUILD_JOBS set "OA_BUILD_JOBS=6"
set "build_dir=%OA_BUILD_DIR%"

where cmake >nul 2>nul || (
    echo run.bat: CMake 3.24+ is required. 1>&2
    exit /b 1
)

rem Ninja and the compiler need the Visual Studio developer environment.
where cl >nul 2>nul && goto have_compiler
set "vswhere=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%vswhere%" (
    echo run.bat: Visual Studio with the C++ tools is required. 1>&2
    exit /b 1
)
rem vswhere is called by name from its own folder: a quoted path holding
rem "(x86)" breaks the command of a for /f loop.
set "PATH=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer;%PATH%"
set "vs_dir="
for /f "delims=" %%d in ('vswhere.exe -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath') do set "vs_dir=%%d"
if not defined vs_dir (
    echo run.bat: no Visual Studio installation has the C++ tools. 1>&2
    exit /b 1
)
call "%vs_dir%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
:have_compiler

rem A build tree is configured once; afterwards the build reruns CMake by
rem itself when a CMakeLists.txt or .cmake file changes.
if exist "%build_dir%\CMakeCache.txt" goto build
set "sdl_prefix=%repo_dir%\local\deps\sdl-install"
rem SDL installs its package file under cmake on Windows, lib\cmake elsewhere.
set "have_sdl="
if exist "%sdl_prefix%\cmake\SDL3Config.cmake" set "have_sdl=1"
if exist "%sdl_prefix%\lib\cmake\SDL3\SDL3Config.cmake" set "have_sdl=1"
if not defined have_sdl (
    echo Building the pinned SDL dependency under local\deps...
    python "%repo_dir%\tools\bootstrap_sdl.py" || exit /b 1
)
if not exist "%repo_dir%\local\deps\text-fonts\DejaVuSans.ttf" (
    echo Building the pinned FreeType and text fonts under local\deps...
    python "%repo_dir%\tools\bootstrap_text_fonts.py" || exit /b 1
)
set "prefix_path=%sdl_prefix%;%repo_dir%\local\deps\zlib-install"
echo Configuring %build_dir%...
cmake -S "%repo_dir%" -B "%build_dir%" -G Ninja "-DCMAKE_BUILD_TYPE=%OA_BUILD_TYPE%" ^
    -DOA_BUILD_PLATFORM=ON -DOA_BUILD_INTRO_PLAYER=ON "-DCMAKE_PREFIX_PATH=%prefix_path%" || exit /b 1

:build
echo Building current native code in %build_dir%...
cmake --build "%build_dir%" --config "%OA_BUILD_TYPE%" --target oa-game --parallel %OA_BUILD_JOBS% || (
    echo run.bat: the build failed; nothing was launched. 1>&2
    exit /b 1
)

set "executable=%build_dir%\open-annihilation.exe"
if not exist "%executable%" set "executable=%build_dir%\%OA_BUILD_TYPE%\open-annihilation.exe"
if not exist "%executable%" (
    echo run.bat: open-annihilation.exe was not built. 1>&2
    exit /b 1
)
echo Launching open-annihilation from current source...
if defined OA_GAME_DIR (
    "%executable%" --game-dir "%OA_GAME_DIR%" %*
) else (
    "%executable%" %*
)
exit /b %errorlevel%
