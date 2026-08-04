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
/// Logging helper.  See log.h.

#include "log.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

log_level_t g_log_level = LOGLEVEL_TRACE;

void log_set_level(log_level_t level) {
    g_log_level = level;
}

/// Returns the name string for a log level.
///
/// @param level  Log level to name.
/// @return       Short name string (e.g. "INFO", "WARN").
static const char *level_name(log_level_t level) {
    switch (level) {
        case LOGLEVEL_TRACE:
            return "TRACE";
        case LOGLEVEL_DEBUG:
            return "DEBUG";
        case LOGLEVEL_INFO:
            return "INFO";
        case LOGLEVEL_WARN:
            return "WARN";
        case LOGLEVEL_ERROR:
            return "ERROR";
        default:
            return "?????";
    }
}

/// Extracts up to 9 chars of the source stem (basename without extension) of a
/// __FILE__ path, for a fixed-width column.
///
/// @param path      Source file path.
/// @param out       Output buffer.
/// @param out_size  Size of the output buffer.
static void source_stem(const char *path, char *out, size_t out_size) {
    const char *base = path;
    for (const char *p = path; *p != '\0'; p++) {
        if (*p == '/') {
            base = p + 1;
        }
    }
    size_t len = 0;
    while (base[len] != '\0' && base[len] != '.') {
        len++;
    }
    if (len > 9) {
        len = 9;
    }
    if (len >= out_size) {
        len = out_size - 1;
    }
    memcpy(out, base, len);
    out[len] = '\0';
}

void log_write_v(log_level_t level, const char *file, int line, const char *fmt, va_list args) {
    if (level < g_log_level) {
        return;
    }

    // Format the caller's message first so the trailing-newline test below sees
    // the final text.
    char msg[4096];
    int n = vsnprintf(msg, sizeof(msg), fmt, args);
    size_t msg_len = (n < 0) ? strlen(msg) : (size_t) n;
    if (msg_len >= sizeof(msg)) {
        msg_len = sizeof(msg) - 1;
    }
    const char *newline = (msg_len > 0 && msg[msg_len - 1] == '\n') ? "" : "\n";

    char stem[16];
    source_stem(file, stem, sizeof(stem));

    struct timespec ts;
    struct tm tm;
    clock_gettime(CLOCK_REALTIME, &ts);
    localtime_r(&ts.tv_sec, &tm);

    printf("[%04d-%02d-%02dT%02d:%02d:%02d][%-5s][%-9s:%-3d] %s%s", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
           tm.tm_hour, tm.tm_min, tm.tm_sec, level_name(level), stem, line, msg, newline);
    fflush(stdout);
}

void log_write(log_level_t level, const char *file, int line, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    log_write_v(level, file, line, fmt, args);
    va_end(args);
}
