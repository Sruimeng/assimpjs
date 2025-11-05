@echo off
pushd %~dp0\..

REM Support ReleaseMini, ReleaseAll, ReleaseExporter, all, clean, or individual clean options
set BUILD_TYPE=%1
if "%BUILD_TYPE%"=="" set BUILD_TYPE=ReleaseMini

REM Validate build type
if not "%BUILD_TYPE%"=="ReleaseMini" if not "%BUILD_TYPE%"=="ReleaseAll" if not "%BUILD_TYPE%"=="ReleaseExporter" if not "%BUILD_TYPE%"=="all" if not "%BUILD_TYPE%"=="clean" if not "%BUILD_TYPE%"=="clean-mini" if not "%BUILD_TYPE%"=="clean-all" if not "%BUILD_TYPE%"=="clean-exporter" (
    echo Error: Only ReleaseMini, ReleaseAll, ReleaseExporter, all, clean, clean-mini, clean-all, or clean-exporter are supported
    echo Usage: %0 [ReleaseMini^|ReleaseAll^|ReleaseExporter^|all^|clean^|clean-mini^|clean-all^|clean-exporter]
    goto :error
)

REM Handle clean options
if "%BUILD_TYPE%"=="clean" (
    echo Cleaning all build caches...
    if exist build_wasm_mini rmdir /s /q build_wasm_mini
    if exist build_wasm_all rmdir /s /q build_wasm_all
    if exist build_wasm_exporter rmdir /s /q build_wasm_exporter
    echo All build caches cleaned!
    goto :end
)
if "%BUILD_TYPE%"=="clean-mini" (
    echo Cleaning mini build cache...
    if exist build_wasm_mini rmdir /s /q build_wasm_mini
    echo Mini build cache cleaned!
    goto :end
)
if "%BUILD_TYPE%"=="clean-all" (
    echo Cleaning all-importers build cache...
    if exist build_wasm_all rmdir /s /q build_wasm_all
    echo All-importers build cache cleaned!
    goto :end
)
if "%BUILD_TYPE%"=="clean-exporter" (
    echo Cleaning exporter build cache...
    if exist build_wasm_exporter rmdir /s /q build_wasm_exporter
    echo Exporter build cache cleaned!
    goto :end
)

echo Building AssimpJS for %BUILD_TYPE%...

call emsdk\emsdk_env.bat

if "%BUILD_TYPE%"=="all" (
    echo Building all AssimpJS variants...
    call :build_target ReleaseMini build_wasm_mini || goto :error
    call :build_target ReleaseAll build_wasm_all || goto :error
    call :build_target ReleaseExporter build_wasm_exporter || goto :error
    call :copy_artifacts build_wasm_mini\ReleaseMini\assimpjs-mini.* build_wasm_all\ReleaseAll\assimpjs-all.* build_wasm_exporter\ReleaseExporter\assimpjs-exporter.*
) else (
    REM Map build type to build directory
    set BUILD_DIR=build_wasm
    if "%BUILD_TYPE%"=="ReleaseMini" set BUILD_DIR=build_wasm_mini
    if "%BUILD_TYPE%"=="ReleaseAll" set BUILD_DIR=build_wasm_all
    if "%BUILD_TYPE%"=="ReleaseExporter" set BUILD_DIR=build_wasm_exporter
    
    echo Building single AssimpJS target (%BUILD_TYPE%)...
    call :build_target %BUILD_TYPE% %BUILD_DIR% || goto :error
    call :copy_artifacts %BUILD_DIR%\%BUILD_TYPE%\assimpjs*.*
)

echo Running tests...
call npm run test || goto :error

call :copy_licenses
call :print_summary

echo Build completed!
:end
popd
exit /b 0

:build_target
set BT=%1
set BD=%2
echo Building %BT% in %BD%...
call emcmake cmake -B %BD% -G "Unix Makefiles" -DEMSCRIPTEN=1 -DCMAKE_MAKE_PROGRAM=mingw32-make -DCMAKE_BUILD_TYPE=%BT% . || exit /b 1
call emmake mingw32-make -C %BD% AssimpJS || exit /b 1
exit /b 0

:copy_artifacts
echo Creating distribution...
if not exist dist mkdir dist
if not exist docs\dist mkdir docs\dist
:copy_loop
if "%1"=="" goto :copy_done
copy %1 dist\ 2>nul
copy dist\assimpjs*.* docs\dist\ 2>nul
shift
goto :copy_loop
:copy_done
exit /b 0

:copy_licenses
copy assimp\LICENSE dist\license.assimp.txt 2>nul
copy LICENSE.md dist\license.assimpjs.txt 2>nul
copy dist\license*.txt docs\dist\ 2>nul
exit /b 0

:print_summary
if exist dist\assimpjs*.* (
    echo.
    echo Build Size Summary:
    dir dist\assimpjs*.* /b /s
)
exit /b 0

:error
echo Build Failed with Error %errorlevel%.
popd
exit /b 1
