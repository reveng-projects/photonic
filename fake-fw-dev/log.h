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
/// Tiny logging helper for the fake camera, modelled on the test suite's helper
/// (test/log.cpp).  Use it as:
///
///     LOG(INFO,  "device opened: %s", path);
///     LOG(ERROR, "open failed: %s", strerror(errno));
///
/// Each call emits exactly one line, prefixed with a timestamp, the severity and
/// the originating source location:
///
///     [2026-06-24T12:34:56][INFO ][dcam     :554] DCAM CSR region allocated
///
/// A trailing newline is appended only when the format does not already end in
/// one, so call sites that include "\n" still produce a single line.
#ifndef FAKE_FW_DEV_LOG_H
#define FAKE_FW_DEV_LOG_H

#include <stdarg.h>

/// Severity levels.  Named with a LOGLEVEL_ prefix so the LOG() macro can
/// token-paste the short name (INFO, WARN, ERROR, ...).
typedef enum log_level {
    LOGLEVEL_TRACE = 0,
    LOGLEVEL_DEBUG,
    LOGLEVEL_INFO,
    LOGLEVEL_WARN,
    LOGLEVEL_ERROR,
} log_level_t;

/// Messages below this level are dropped.  Defaults to LOGLEVEL_TRACE.
extern log_level_t g_log_level;

/// Sets the minimum log level; messages below it are silently dropped.
///
/// @param level  New minimum level.
void log_set_level(log_level_t level);

/// Primary entry point.  Usage: LOG(INFO, "value = %u", n);
#define LOG(level, ...) log_write(LOGLEVEL_##level, __FILE__, __LINE__, __VA_ARGS__)

/// printf-style line writer behind the LOG() macro.  Emits one prefixed line
/// to stdout.  Call this directly when the level is computed at run time (the
/// macro requires a literal level name); pass __FILE__ / __LINE__ yourself.
///
/// @param level  Severity level.
/// @param file   Source file path (__FILE__).
/// @param line   Source line number (__LINE__).
/// @param fmt    printf-style format string.
void log_write(log_level_t level, const char *file, int line, const char *fmt, ...)
    __attribute__((format(printf, 4, 5)));

/// va_list variant of log_write; same semantics.
///
/// @param level  Severity level.
/// @param file   Source file path.
/// @param line   Source line number.
/// @param fmt    printf-style format string.
/// @param args   Argument list.
void log_write_v(log_level_t level, const char *file, int line, const char *fmt, va_list args);

#endif // FAKE_FW_DEV_LOG_H
