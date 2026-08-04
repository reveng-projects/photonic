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

@echo on
setlocal

rem Build the distribution folder layout in the target folder (default "dist"):
rem   <target>\x64\photonic.[sys|inf|cat|pdb]
rem   <target>\x86\photonic.[sys|inf|cat|pdb]
rem   <target>\test.[exe|pdb]
rem   <target>\reload.ps1
rem   <target>\test.bat
rem   <target>\original\*.dll (original driver's binaries)
rem
rem Usage: make_dist.bat <debug|release> <pixelink-folder> <solution-folder> <target-folder>
rem   pixelink-folder: location of pixelinkapi.dll and its runtime libraries
rem   solution-folder: root of the solution, containing the build outputs
rem
rem All parameters are required.

if "%~4"=="" (
    call :Usage
    exit /b 1
)

set CONFIG=%~1
if /i "%CONFIG%"=="debug" (
    set CONFIG=Debug
) else if /i "%CONFIG%"=="release" (
    set CONFIG=Release
) else (
    echo Unknown configuration "%CONFIG%".
    call :Usage
    exit /b 1
)

set PIXELINK=%~2
set SLN=%~3
set DIST=%~4

set SRC=%SLN%\x64\%CONFIG%\photonic
set PDB=%SLN%\x64\%CONFIG%\photonic.pdb
set SRC32=%SLN%\%CONFIG%\photonic
set PDB32=%SLN%\%CONFIG%\photonic.pdb
set TEST=%SLN%\%CONFIG%\test.exe

if not exist "%PIXELINK%\pixelinkapi.dll" (
    echo Pixelink binaries not found in "%PIXELINK%".
    exit /b 1
)

if not exist "%SRC%\photonic.sys" (
    echo Driver not built: "%SRC%\photonic.sys" not found. Build the driver first.
    exit /b 1
)

if not exist "%SRC32%\photonic.sys" (
    echo 32-bit driver not built: "%SRC32%\photonic.sys" not found. Build the driver first.
    exit /b 1
)

if not exist "%TEST%" (
    echo Test app not built: "%TEST%" not found. Build the solution first.
    exit /b 1
)

echo Creating distribution in "%DIST%" ...
if exist "%DIST%" rmdir /s /q "%DIST%"
if not exist "%DIST%\x64" mkdir "%DIST%\x64"
if not exist "%DIST%\x86" mkdir "%DIST%\x86"
if not exist "%DIST%\original" mkdir "%DIST%\original"

copy /y "%SRC%\photonic.sys" "%DIST%\x64\" || exit /b 1
copy /y "%SRC%\photonic.inf" "%DIST%\x64\" || exit /b 1
copy /y "%SRC%\photonic.cat" "%DIST%\x64\" || exit /b 1
copy /y "%PDB%"              "%DIST%\x64\" || exit /b 1

copy /y "%SRC32%\photonic.sys" "%DIST%\x86\" || exit /b 1
copy /y "%SRC32%\photonic.inf" "%DIST%\x86\" || exit /b 1
copy /y "%SRC32%\photonic.cat" "%DIST%\x86\" || exit /b 1
copy /y "%PDB32%"              "%DIST%\x86\" || exit /b 1

copy /y "%TEST%"                    "%DIST%\" || exit /b 1
copy /y "%SLN%\%CONFIG%\test.pdb"   "%DIST%\" || exit /b 1
copy /y "%SLN%\scripts\reload.ps1"  "%DIST%\" || exit /b 1
copy /y "%SLN%\scripts\test.bat"    "%DIST%\" || exit /b 1

rem Original driver's binaries, needed by the pixelink API test.
copy /y "%PIXELINK%\pixelinkapi.dll" "%DIST%\original\" || exit /b 1
copy /y "%PIXELINK%\MFC71.DLL"       "%DIST%\original\" || exit /b 1
copy /y "%PIXELINK%\MSVCR71.dll"     "%DIST%\original\" || exit /b 1

echo Done.
endlocal
exit /b 0

:Usage
echo Usage: make_dist.bat ^<debug^|release^> ^<pixelink-folder^> ^<solution-folder^> ^<target-folder^>
exit /b 0
