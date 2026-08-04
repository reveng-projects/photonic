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
/// WPP software tracing configuration for the Photonic minidriver.
///
/// WPP (Windows software trace preprocessor) turns the Trace* macros below into
/// lightweight ETW events at build time. Tracing is always compiled in (debug and
/// release) and has negligible cost when no one is listening.
///
/// Two ways to see the traces:
///
///   1. WinDbg, no trace session needed. The driver is built with WPP recorder
///      mode, so every event is also written to an in-memory circular buffer.
///      Dump it from the debugger with:
///          !rcdrkd.rcdrlogdump photonic
///      This also works on a crash dump -- the last traces are right there.
///
///   2. Live capture without a debugger, e.g. with TraceView or tracelog:
///          tracelog -start photonic -guid #b6a1f3c2-9d4e-4a7b-8f21-3c5e7a9d1b04 ^
///                   -f photonic.etl -flag 0x7F -level 5
///          tracelog -stop photonic
///          tracefmt photonic.etl -o photonic.txt   (needs photonic.pdb)
///
/// The decode (PDB / TMF) maps the %!FUNC! token to the calling function name,
/// which is exactly the "see its functions called" view to look for. The token
/// must appear in each USEPREFIX below for tracefmt to emit it (see the wpp
/// config block) -- %!STDPREFIX! alone does not carry the function name.

#ifndef PHOTONIC_TRACE_H
#define PHOTONIC_TRACE_H

/// TRACE_LEVEL_* (TRACE_LEVEL_INFORMATION, _VERBOSE, ...) used by the macros.
#include <evntrace.h>

/// Trace provider control GUID and the trace flag bits. Each flag is a separate
/// keyword that can be enabled independently when capturing:
///   TRACE_FLAG_DRIVER   (0x1)  -- load / unload, DriverEntry
///   TRACE_FLAG_DISPATCH (0x2)  -- device-level SRB dispatch (dispatch.c)
///   TRACE_FLAG_STREAM   (0x4)  -- stream control / data SRBs (stream/)
///   TRACE_FLAG_1394     (0x8)  -- 1394 bus transactions (p1394.c)
///   TRACE_FLAG_DCAM     (0x10) -- DCAM reset / mode enumeration (dcam.c)
///   TRACE_FLAG_ISO      (0x20) -- isochronous capture engine (capture/)
///   TRACE_FLAG_IOCTL    (0x40) -- Photonic IOCTL dispatch (ioctl/)
///
/// {B6A1F3C2-9D4E-4A7B-8F21-3C5E7A9D1B04}
#define WPP_CONTROL_GUIDS                                                                          \
    WPP_DEFINE_CONTROL_GUID(PhotonicTraceGuid, (B6A1F3C2, 9D4E, 4A7B, 8F21, 3C5E7A9D1B04),         \
                            WPP_DEFINE_BIT(TRACE_FLAG_DRIVER) WPP_DEFINE_BIT(TRACE_FLAG_DISPATCH)  \
                                WPP_DEFINE_BIT(TRACE_FLAG_STREAM) WPP_DEFINE_BIT(TRACE_FLAG_1394)  \
                                    WPP_DEFINE_BIT(TRACE_FLAG_DCAM) WPP_DEFINE_BIT(TRACE_FLAG_ISO) \
                                        WPP_DEFINE_BIT(TRACE_FLAG_IOCTL))

/// Glue mapping the (LEVEL, FLAGS) and (FLAGS, LEVEL) control-parameter orders
/// the macros below use onto WPP's logger/enabled checks.
#define WPP_LEVEL_FLAGS_LOGGER(lvl, flags)  WPP_LEVEL_LOGGER(flags)
#define WPP_LEVEL_FLAGS_ENABLED(lvl, flags) (WPP_LEVEL_ENABLED(flags) && WPP_CONTROL(WPP_BIT_##flags).Level >= lvl)
#define WPP_FLAG_LEVEL_LOGGER(flag, level)  WPP_LEVEL_LOGGER(flag)
#define WPP_FLAG_LEVEL_ENABLED(flag, level) (WPP_LEVEL_ENABLED(flag) && WPP_CONTROL(WPP_BIT_##flag).Level >= level)

/// The block below is parsed by the WPP preprocessor (not the C compiler) to
/// define the trace functions the driver calls:
///
///   FuncEntry(FLAGS)            -- "--> FunctionName"
///   FuncExit(FLAGS)             -- "<-- FunctionName"
///   TraceEvents(LEVEL, FLAGS, MSG, ...) -- arbitrary formatted event
///
/// FuncEntry/FuncExit carry no caller-supplied text. The function name is NOT
/// part of %!STDPREFIX! -- that token only expands to the standard
/// cpu/process/thread/time/component prefix -- so each prefix below must request
/// it explicitly via %!FUNC!. Without it, tracefmt has no function token to emit
/// and the entry/exit lines decode to a bare "-->" / "<--". A bare
/// FuncEntry(TRACE_FLAG_DISPATCH); is enough to log a call.
// begin_wpp config
// FUNC FuncEntry{LEVEL=TRACE_LEVEL_VERBOSE}(FLAGS);
// USEPREFIX (FuncEntry, "%!STDPREFIX![%!LEVEL!] --> %!FUNC!");
// FUNC FuncExit{LEVEL=TRACE_LEVEL_VERBOSE}(FLAGS);
// USEPREFIX (FuncExit, "%!STDPREFIX![%!LEVEL!] <-- %!FUNC!");
// FUNC TraceEvents(LEVEL, FLAGS, MSG, ...);
// USEPREFIX (TraceEvents, "%!STDPREFIX![%!LEVEL!] %!FUNC!: ");
// end_wpp

#endif // PHOTONIC_TRACE_H
