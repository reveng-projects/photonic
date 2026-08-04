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
/// Shared logging helper.  See log.h.

#include "log.h"

#include <windows.h> // SYSTEMTIME / GetLocalTime

#include <cstdarg>
#include <cstdio>
#include <cstring>

LogLevel g_LogLevel = LOGLEVEL_TRACE;

static FILE *g_logFile = nullptr;

void SetLogLevel(LogLevel level) {
    g_LogLevel = level;
}

bool LogOpen(const wchar_t *path) {
    if (path == nullptr || path[0] == L'\0') {
        return true;
    }
    g_logFile = _wfopen(path, L"w, ccs=UTF-8");
    return g_logFile != nullptr;
}

void LogClose() {
    if (g_logFile != nullptr) {
        fclose(g_logFile);
        g_logFile = nullptr;
    }
}

void LogFlush() {
    fflush(stdout);
    if (g_logFile != nullptr) {
        fflush(g_logFile);
    }
}

static const wchar_t *LevelName(LogLevel level) {
    switch (level) {
        case LOGLEVEL_TRACE:
            return L"TRACE";
        case LOGLEVEL_DEBUG:
            return L"DEBUG";
        case LOGLEVEL_INFO:
            return L"INFO";
        case LOGLEVEL_WARN:
            return L"WARN";
        case LOGLEVEL_ERROR:
            return L"ERROR";
        default:
            return L"?????";
    }
}

/// Up to 9 chars of the source stem (basename without extension) of a (narrow)
/// __FILE__ path, for a fixed-width column.
static void SourceStem(const char *path, char *out, size_t outSize) {
    const char *base = path;
    for (const char *p = path; *p != '\0'; p++) {
        if (*p == '\\' || *p == '/') {
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
    if (len >= outSize) {
        len = outSize - 1;
    }
    memcpy(out, base, len);
    out[len] = '\0';
}

void LogWriteV(LogLevel level, const char *file, int line, const wchar_t *fmt, va_list args) {
    if (level < g_LogLevel) {
        return;
    }

    // Format the caller's message first so it can be teed to several sinks
    // without re-walking the va_list, and so the trailing-newline test below
    // sees the final text.
    wchar_t msg[4096];
    int n = vswprintf(msg, _countof(msg), fmt, args);
    msg[_countof(msg) - 1] = L'\0';
    size_t msgLen = (n < 0) ? wcslen(msg) : (size_t) n;
    const wchar_t *newline = (msgLen > 0 && msg[msgLen - 1] == L'\n') ? L"" : L"\n";

    char stem[16];
    SourceStem(file, stem, sizeof(stem));

    SYSTEMTIME st;
    GetLocalTime(&st);

    wchar_t out[4160];
    swprintf(out, _countof(out), L"[%04d-%02d-%02dT%02d:%02d:%02d][%-5s][%-9hs:%-3d] %s%s", st.wYear, st.wMonth,
             st.wDay, st.wHour, st.wMinute, st.wSecond, LevelName(level), stem, line, msg, newline);
    out[_countof(out) - 1] = L'\0';

    fputws(out, stdout);
    if (g_logFile != nullptr) {
        fputws(out, g_logFile);
    }

    LogFlush();
}

void LogWrite(LogLevel level, const char *file, int line, const wchar_t *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    LogWriteV(level, file, line, fmt, args);
    va_end(args);
}
