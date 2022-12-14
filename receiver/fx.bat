@echo off

:GETOPTS
if /I "%1" == "cmake-conf" call^
 cmake --no-warn-unused-cli^
 -DCMAKE_TOOLCHAIN_FILE:STRING=C:/tools/vcpkg/scripts/buildsystems/vcpkg.cmake^
 -DCMAKE_EXPORT_COMPILE_COMMANDS:BOOL=TRUE^
 -B"build" -G"Visual Studio 17 2022"^
 -A"x64"

if /I "%1" == "build" goto BUILD
if /I "%1" == "help" goto HELP
goto EXIT

:BUILD
set ARGS=/p:CL_MPCOUNT=%NUMBER_OF_PROCESSORS% /m:%NUMBER_OF_PROCESSORS% /v:minimal /nologo

set PROJ_PATH="%3"
if "%~3" == "" set PROJ_PATH="build\view_data.vcxproj" 

if /I "%2" == ""        call msbuild %PROJ_PATH% /p:Configuration=Release          %ARGS%
if /I "%2" == "release" call msbuild %PROJ_PATH% /p:Configuration=Release          %ARGS%
if /I "%2" == "debug"   call msbuild %PROJ_PATH% /p:Configuration=Debug            %ARGS%
if /I "%2" == "reldeb"  call msbuild %PROJ_PATH% /p:Configuration=RelWithDebInfo   %ARGS% 
if /I "%2" == "minsize" call msbuild %PROJ_PATH% /p:Configuration=MinSizeRel       %ARGS%
goto EXIT

:HELP
echo Available commands
echo cmake-conf        - Run cmake configure
echo build             - Run msbuild (Usage: build [.vcprojx] [build_type])
goto EXIT

:EXIT
