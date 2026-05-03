@echo off
REM build_and_test.bat
REM Build and run all ymodem unit tests on Windows.
REM Auto-detects GCC or MSVC; add your compiler to PATH if needed.
REM Run from project root (ymodem_receiver/).
REM
REM Usage:
REM   build_and_test.bat           Run all tests
REM   build_and_test.bat receiver  Run receiver tests only
REM   build_and_test.bat sender    Run sender tests only
REM   build_and_test.bat common    Run common (CRC) tests only

setlocal enabledelayedexpansion

set UNITY_CORE=test/unity/unity_core
set MOCK_DIR=test/unity/mocks
set UNIT_DIR=test/unity/unit
set SRC_DIR=src

set UNITY_SRC=%UNITY_CORE%/unity.c
set MOCK_SRC=%MOCK_DIR%/mock_time.c

REM ---- Find C compiler ----
set CC=
set CFLAGS=
set OUT_FLAG=-o

where gcc >nul 2>&1
if %ERRORLEVEL% equ 0 (
    set CC=gcc
    set CFLAGS=-I%SRC_DIR% -I%UNITY_CORE% -Wall -Wextra -Wno-unused-variable
    set OUT_FLAG=-o
    goto :start_build
)

REM Try common MinGW/msys2 locations
for %%D in (
    C:\msys64\mingw64\bin\gcc.exe
    C:\msys64\ucrt64\bin\gcc.exe
    C:\msys64\clang64\bin\gcc.exe
    C:\mingw64\bin\gcc.exe
    C:\MinGW\bin\gcc.exe
) do (
    if exist "%%D" (
        set CC="%%D"
        set CFLAGS=-I%SRC_DIR% -I%UNITY_CORE% -Wall -Wextra -Wno-unused-variable
        set OUT_FLAG=-o
        goto :start_build
    )
)

where cl >nul 2>&1
if %ERRORLEVEL% equ 0 (
    set CC=cl
    set CFLAGS=/I%SRC_DIR% /I%UNITY_CORE% /W3 /nologo /utf-8 /wd4819 /D_CRT_SECURE_NO_WARNINGS
    set OUT_FLAG=/Fe
    set UNITY_SRC=%UNITY_SRC:/=\%
    set MOCK_SRC=%MOCK_SRC:/=\%
    set UNIT_DIR=%UNIT_DIR:/=\%
    set SRC_DIR=%SRC_DIR:/=\%
    set UNITY_CORE=%UNITY_CORE:/=\%
    goto :start_build
)

REM No compiler found
echo [ERROR] No C compiler found.
echo Install MinGW-w64 (gcc) or Visual Studio (MSVC), then add it to PATH.
echo.
echo Quick install options:
echo   1. MinGW-w64:  winget install -e --id GnuWin32.Make
echo   2. MSYS2:       https://www.msys2.org/  (then pacman -S mingw-w64-ucrt-x86_64-gcc)
echo   3. MSVC:        Run from "Developer Command Prompt for VS"
exit /b 1

:start_build
echo Using compiler: %CC%
echo.

set FILTER=all
if not "%1"=="" set FILTER=%1

set FAILED=0

REM ================================================================
REM  test_common (CRC)
REM ================================================================
if /i "%FILTER%"=="all" goto :run_common
if /i "%FILTER%"=="common" goto :run_common
goto :skip_common

:run_common
echo [common] Building test_common...
%CC% %CFLAGS% %UNITY_SRC% %MOCK_SRC% %UNIT_DIR%/test_common.c %SRC_DIR%/ymodem_common.c %OUT_FLAG%"%UNIT_DIR%\test_common.exe"
if !ERRORLEVEL! neq 0 (
    echo [FAIL] test_common build failed
    set FAILED=1
    goto :skip_common
)
echo [common] Running...
"%UNIT_DIR%\test_common.exe"
if !ERRORLEVEL! neq 0 (
    echo [FAIL] test_common FAILED
    set FAILED=1
) else (
    echo [PASS] test_common OK
)
echo.
:skip_common

REM ================================================================
REM  test_sender
REM ================================================================
if /i "%FILTER%"=="all" goto :run_sender
if /i "%FILTER%"=="sender" goto :run_sender
goto :skip_sender

:run_sender
echo [sender] Building test_sender...
%CC% %CFLAGS% %UNITY_SRC% %MOCK_SRC% %UNIT_DIR%/test_sender.c %SRC_DIR%/ymodem_sender.c %SRC_DIR%/ymodem_common.c %OUT_FLAG%"%UNIT_DIR%\test_sender.exe"
if !ERRORLEVEL! neq 0 (
    echo [FAIL] test_sender build failed
    set FAILED=1
    goto :skip_sender
)
echo [sender] Running...
"%UNIT_DIR%\test_sender.exe"
if !ERRORLEVEL! neq 0 (
    echo [FAIL] test_sender FAILED
    set FAILED=1
) else (
    echo [PASS] test_sender OK
)
echo.
:skip_sender

REM ================================================================
REM  test_receiver
REM ================================================================
if /i "%FILTER%"=="all" goto :run_receiver
if /i "%FILTER%"=="receiver" goto :run_receiver
goto :skip_receiver

:run_receiver
echo [receiver] Building test_receiver...
%CC% %CFLAGS% %UNITY_SRC% %MOCK_SRC% %UNIT_DIR%/test_receiver.c %SRC_DIR%/ymodem_receiver.c %SRC_DIR%/ymodem_common.c %OUT_FLAG%"%UNIT_DIR%\test_receiver.exe"
if !ERRORLEVEL! neq 0 (
    echo [FAIL] test_receiver build failed
    set FAILED=1
    goto :skip_receiver
)
echo [receiver] Running...
"%UNIT_DIR%\test_receiver.exe"
if !ERRORLEVEL! neq 0 (
    echo [FAIL] test_receiver FAILED
    set FAILED=1
) else (
    echo [PASS] test_receiver OK
)
echo.
:skip_receiver

if !FAILED! equ 1 (
    echo.
    echo === Some tests FAILED ===
    exit /b 1
)

echo === All tests passed ===
exit /b 0