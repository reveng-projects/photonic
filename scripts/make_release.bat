REM Copyright (c) 2026 Vitaly Chipounov
REM
REM Permission is hereby granted, free of charge, to any person obtaining a copy
REM of this software and associated documentation files (the "Software"), to deal
REM in the Software without restriction, including without limitation the rights
REM to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
REM copies of the Software, and to permit persons to whom the Software is
REM furnished to do so, subject to the following conditions:
REM
REM The above copyright notice and this permission notice shall be included in all
REM copies or substantial portions of the Software.
REM
REM THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
REM IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
REM FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
REM AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
REM LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
REM OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
REM SOFTWARE.

setlocal

rem Name the release folder yyyy-mm-dd-hhmmss-githash-config.
for /f %%i in ('powershell -NoProfile -Command "Get-Date -Format yyyy-MM-dd-HHmmss"') do set RELEASE_DATE=%%i
for /f %%i in ('git rev-parse --short HEAD') do set GIT_HASH=%%i

if "%RELEASE_DATE%"=="" (
    echo Failed to get the current date.
    exit /b 1
)

if "%GIT_HASH%"=="" (
    echo Failed to get the git hash. Run this script from the git repository.
    exit /b 1
)

rem Optional first parameter selects the build configuration, Debug by default.
set CONFIG=%~1
if "%CONFIG%"=="" (
    set CONFIG=Debug
) else if /i "%CONFIG%"=="debug" (
    set CONFIG=Debug
) else if /i "%CONFIG%"=="release" (
    set CONFIG=Release
) else (
    echo Unknown configuration "%CONFIG%".
    echo Usage: make_release.bat [debug^|release] [output-folder]
    exit /b 1
)

rem Optional second parameter overrides the folder that receives the release,
rem "dist" by default.
set PREFIX=%~2
if "%PREFIX%"=="" (
    set PREFIX=dist
)

set DIST=%PREFIX%\%RELEASE_DATE%-%GIT_HASH%-%CONFIG%

where msbuild >nul 2>nul
if errorlevel 1 (
    echo msbuild not found. Run this script from a Visual Studio developer command prompt.
    exit /b 1
)

rem Build the solution for both architectures. make_dist.bat picks up the
rem 64-bit driver from x64\%CONFIG% and the 32-bit driver and test app
rem from %CONFIG%.
msbuild ..\photonic.sln /m /p:Configuration=%CONFIG% /p:Platform=x64 || exit /b 1
msbuild ..\photonic.sln /m /p:Configuration=%CONFIG% /p:Platform=x86 || exit /b 1

call make_dist.bat %CONFIG% ..\pixelink ..\ "%DIST%" || exit /b 1

rem The prebuild step embeds the git hash in each binary's version resource.
rem Verify it matches the hash of the current checkout, otherwise a stale
rem build output was packaged.
call :CheckVersion "%DIST%\x64\photonic.sys" || exit /b 1
call :CheckVersion "%DIST%\x86\photonic.sys" || exit /b 1
call :CheckVersion "%DIST%\test.exe" || exit /b 1

echo Creating "%DIST%.zip" ...
powershell -NoProfile -Command "Compress-Archive -Path '%DIST%\*' -DestinationPath '%DIST%.zip' -Force" || exit /b 1
endlocal
exit /b 0

:CheckVersion
powershell -NoProfile -Command "$v = (Get-Item '%~1').VersionInfo.ProductVersion; if ($v -notmatch '-%GIT_HASH%$') { Write-Host \"Version mismatch in %~1: got '$v', expected hash %GIT_HASH%.\"; exit 1 }; Write-Host \"%~1: $v\"" || exit /b 1
exit /b 0
