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

rem First parameter is the solution folder containing the build outputs.
set SLN=%~1
if "%SLN%"=="" (
    echo Usage: run.bat ^<solution-folder^>
    exit /b 1
)

logman stop photonic -ets

del photonic.etl

rem Start WPP tracing for the Photonic driver (see photonic\trace.h).
rem logman is built into Windows (no WDK needed). The GUID, keyword mask 0xff
rem (all flags) and level 5 match WPP_CONTROL_GUIDS.
logman create trace photonic -p {b6a1f3c2-9d4e-4a7b-8f21-3c5e7a9d1b04} 0xff 5 -o photonic.etl -ets

rem Record verbose WPP events (FuncEntry / FuncExit) in the inflight trace
rem recorder and enlarge its buffer to 4 pages so chatty verbose tracing does
rem not wrap before it can be dumped. WPP_INIT_TRACING reads these values at
rem driver load, so set them before the reload. The INF does not set them, so
rem release installs keep the recorder at its default non-verbose level.
reg add HKLM\SYSTEM\CurrentControlSet\Services\photonic\Parameters /v VerboseOn /t REG_DWORD /d 1 /f
reg add HKLM\SYSTEM\CurrentControlSet\Services\photonic\Parameters /v LogPages /t REG_DWORD /d 4 /f

rem Reload the freshly built driver. reload.ps1 picks the INF matching the
rem OS bitness.
powershell -ExecutionPolicy Bypass -File "%~dp0reload.ps1" -Inf "%SLN%\x64\Debug\photonic\photonic.inf" -Inf32 "%SLN%\Debug\photonic\photonic.inf"

:: cd Z:\perso\reveng\photonic\original
:: c:\Python313-32\python.exe test_dualfdi_vhr_acq_save_debug_v17.py
"%SLN%\Debug\test.exe" --test-directshow --dump-dir captured_frames
"%SLN%\Debug\test.exe" --test-ioctl -c com3 --dump-dir captured_frames
"%SLN%\Debug\test.exe" --test-pixelink "%SLN%\Debug\pixelinkapi.dll" --com-port com3 --test acquisition.EndToEndSnap
::"%SLN%\Debug\test.exe" --test-directshow
rem Stop tracing. Decode photonic.etl with: tracefmt photonic.etl -o photonic.txt
logman stop photonic -ets

endlocal
