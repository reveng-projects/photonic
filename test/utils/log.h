// Copyright (c) 2026 Vitaly Chipounov
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

/// @file
/// Tiny shared logging helper used by every test suite in this project.  Use it
/// as:
///
///     Log(INFO,  L"device count: %lu", count);
///     Log(ERROR, L"BindToObject failed (hr=0x%08lX)", hr);
///
/// Each call emits exactly one line, prefixed with a timestamp, the severity
/// and the originating source location:
///
///     [2026-06-14T12:34:56][INFO ][directsho:200] device count: 3
///
/// Output goes to the console and, when a results file has been opened with
/// LogOpen, is teed to that file as well.  std::format / std::source_location
/// are not available on the v140_xp toolset, so the format string stays
/// printf-style (wide) and the call site is captured with __FILE__ / __LINE__.
#pragma once

#include <cstdarg>

/// Severity levels.  Named with a LOGLEVEL_ prefix so the Log() macro can
/// token-paste the short name (INFO, WARN, ERROR, ...).  The paste also stops
/// the preprocessor from expanding the argument, so Log(ERROR, ...) keeps
/// working even though <windows.h> defines ERROR as a macro.
enum LogLevel {
    LOGLEVEL_TRACE = 0,
    LOGLEVEL_DEBUG,
    LOGLEVEL_INFO,
    LOGLEVEL_WARN,
    LOGLEVEL_ERROR,
};

/// Messages below this level are dropped.  Defaults to LOGLEVEL_TRACE.
extern LogLevel g_LogLevel;
void SetLogLevel(LogLevel level);

/// Primary entry point.  Usage:  Log(INFO, L"value = %lu", n);
#define Log(level, ...) LogWrite(LOGLEVEL_##level, __FILE__, __LINE__, __VA_ARGS__)

/// printf-style line writer behind the Log() macro.  Emits one prefixed line
/// (see the header comment) to the console and the results file (if open).  A
/// trailing newline is appended only when <fmt> does not already end in one, so
/// existing call sites that include "\n" keep producing a single line.  Call
/// this directly when the level is computed at run time (the macro requires a
/// literal level name); pass __FILE__ / __LINE__ yourself.
///
/// @param level  Severity level of the message.
/// @param file   Source file name, typically __FILE__.
/// @param line   Source line number, typically __LINE__.
/// @param fmt    printf-style wide format string.
void LogWrite(LogLevel level, const char *file, int line, const wchar_t *fmt, ...);
/// Variadic-argument variant of LogWrite.
///
/// @param level  Severity level of the message.
/// @param file   Source file name, typically __FILE__.
/// @param line   Source line number, typically __LINE__.
/// @param fmt    printf-style wide format string.
/// @param args   Argument list initialized by the caller.
void LogWriteV(LogLevel level, const char *file, int line, const wchar_t *fmt, va_list args);

/// Open an optional results file that subsequent output is teed to (in addition
/// to the console).  Pass nullptr or an empty string to log to the console only.
/// Returns false if a non-empty path could not be opened, in which case logging
/// silently continues to the console only.
///
/// @param path    Path to the results file, or nullptr/empty to disable file output.
/// @return true on success, false if the path is non-empty and the file could not be opened.
bool LogOpen(const wchar_t *path);

/// Close the results file (if any).  Safe to call when none is open.
void LogClose();

/// Flush stdout and the results file (if open).
void LogFlush();
