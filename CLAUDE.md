# Overview
- photonic: this is a device driver that interfaces with a camera over firewire and exposes a DirectShow interface.
- test: this is a test app that checks that all video modes exposed by the camera/driver work properly.
- fake-fw-dev: this is a Linux application that simulates a real camera. The driver must work with it.

The rules below are generic and transposable to any driver. Where a concrete format is needed, the format used in this repository serves as the model. The reasoning behind many of the rules is collected in [docs/lessons-learned.md](docs/lessons-learned.md).

# Project Layout

- Each part of the project (driver, test app, device simulator) lives in its own top-level directory, with documentation in docs/ and helper scripts (build, install, trace capture, release packaging) in scripts/.
- The simulator is a standalone model of the device, described in terms of the device's own protocol, never in terms of any driver's internals.
- Within the driver, one module per source file pair (foo.c and foo.h). When a module outgrows a single file, split it into a subdirectory of files with the same structure.
- When splitting, public functions keep their names so call sites outside the module do not change. Internal functions gain a module prefix.
- Choose file basenames that are unique across the whole project when the build system writes objects or generated files into one flat directory, where same-named files silently overwrite each other.

# License Headers

- Every source file starts with the license header, before anything else, in the file's native line-comment style (`//` for C and C++, `#` for shell, CMake, and Python, `REM` or `::` for batch files). A shebang line stays first, with the header directly below it.
- Third-party files keep their own headers and are never restamped.
- Apply headers with an idempotent script that detects an existing header, so re-running it never duplicates. After a bulk pass, verify with a script, not by eye.

# Code Style

## Coding Convention
- Use braces for all blocks, even if they contain only one statement.
- Use Pascal case for naming for Windows code.
- Do not use hungarian notation.
- For Linux code, use snake_case. Match each platform's conventions rather than imposing one across both.
- Member variables in C++ code should be prefix with m_.
- Use C++-style comments across the whole code base. Inline C-style comments in parameter lists are ok.
- Keep the function return type on the same line as the name.
- Do not spread out a function return type, name, parameter list on multiple lines if it can fit on one line reasonably well.
- Avoid code duplication.
- Follow the file around you. When editing an existing file, match its layout and idiom instead of introducing a second style.
- Discover conventions before inventing them. Error-code pairings, trace levels, and lock idioms are established by the existing handlers: read a sibling before writing a new one.
- Do not add helpers whose body is smaller than their name. A wrapper around a single standard-library call is noise, not abstraction.

## Comments and Documentation
- Write accurate and concise comments. Conciseness does not mean loss of important information and should not make readability worse.
- Comments are accurate first. Verify every claim a comment makes against the code it describes. A comment that promises behavior ("retried later", "cannot happen") must name the mechanism, and the mechanism must exist. Re-derive constants that carry units in their comment (a timeout "in seconds" computed in the wrong unit is a live bug the comment helps find).
- When shortening or rewording a comment, every fact in the old text is either kept or consciously discarded as wrong.
- Do not use "we" in comments. Be specific (e.g., the driver, the caller, etc.).
- Do not overuse semicolons or em-dashes.
- Use doxygen-style comments where appropriate (`///`, `@param`, `@return`). Use proper markers for parameters, etc. Every file starts (after the license header) with a `/// @file` block saying what the module does.
- Comments should be easy to read and sound natural.
- Do not use passive voice when the subject would actually convey additional information.
- In a multi-line comment, do not leave one or two words alone on the last line.
- Do not overuse the word "classic" (e.g., classic cause), "load bearing", "bearing", "seam", "wedged", "latch", and other LLMisms.
- Do not use adjectives and other words that bring no value (e.g., honest).
- Minimize jargon and uncommon words. Non-experts should be able to read and understand everything reasonably well.
- When introducing a new term or abbreviation for the first time, define it, especially if it is driver-specific. Common knowledge does not need to be defined (e.g., any standard driver terminology).
- Use plain English in comments and documentation. Do not coin aphorisms or metaphor slogans. A documentation heading must state its point plainly and be understandable before reading the body.
- You are writing technical documentation, not marketing material.
- Some comments are functional and must keep their exact form: trace preprocessor annotations (`// begin_wpp` blocks), formatter on/off markers, and similar tool-consumed comments. Never restyle them.
- Comments justify designs on technical grounds. They do not narrate history, compare against previous implementations, reference reverse-engineering artifacts, or paraphrase the code.

# Logging and Tracing

The driver must be diagnosable from its logs alone, because the worst bugs appear on machines where no debugger is attached.

Windows driver (WPP tracing):
- Use WPP for tracing. Tracing is always compiled in, debug and release, with negligible cost when no session is listening. Additionally use recorder mode (an in-memory circular buffer) so the last events are recoverable from a crash dump.
- Each function call shall be traced and print the relevant parameters / state. Entry/exit events carry the function name in the decoded output.
- Give each subsystem its own trace flag so subsystems can be enabled independently during capture.
- Use severity levels consistently: errors for failures the caller sees, warnings for recovered or suspicious conditions, information for state changes, verbose for per-operation detail.
- Verbose tracing is a capture-time choice, never an installed default. Installer files must not ship debug verbosity enabled.

Linux components use a small logging helper where each call emits exactly one line with a timestamp, severity, and source location:

    [2026-06-24T12:34:56][INFO ][dcam     :554] DCAM CSR region allocated

Rules for message content on both platforms:
- No leading spaces in log messages.
- Log values, not just events: the register written, the size computed, the status received. A trace that says "configure failed" without the parameters cannot support a diagnosis.
- A status value that is only logged is not checked. Every logged failure status must also be acted on.
- When fixing a bug, state the expected trace output of the fixed path, so the next run verifies the fix mechanically.

# Error Handling and Hardening

- Design the driver to minimize risk of crashes, resource leaks, race conditions, infinite loops, etc.
- Validate input and reject it with an error. Never silently correct, clamp, or ignore a bad value, whether it comes from the client or the device. A check that logs a warning and then proceeds is not a check.
- Harden against broken hardware. Treat every device-supplied value as hostile: check it against register field widths, use overflow-checked arithmetic including the final narrowing cast, be careful with pointer arithmetic derived from hardware input, and validate related fields against each other (a reported "current" must lie within the reported "maximum").
- Every wait and every polling loop is bounded by a timeout, or its unboundedness is justified in a comment and the wait logs on every period so a hang is diagnosable. The justified case exists: when timing out would free memory the hardware can still write, hanging is safer than corrupting.
- Follow every failure path through to process exit, device removal, and driver unload. The OS enforces rules at those points (locked pages, pending I/O, remove locks) that turn quiet leaks into crashes.
- Error recovery should be good but not excessive. It massively increases the driver complexity and at some point becomes overkill. It is OK to fail capture and report to user space, which will deal with it as necessary. The driver should only ensure basic correctness (no crashes, resource leaks, race conditions, infinite loops, etc.).
- Before writing a retry, ask what actually improves on the next attempt. If nothing improves, one attempt is the design.

# Concurrency and Blocking

- The driver must not get stuck too long in kernel mode. Ideally, every operation that blocks must be done asynchronously. Do not block in high-priority or framework-callback context.
- Serialize control-plane work on one executor or one lock instead of coordinating concurrent paths with flags. Before adding a flag that is read from more than one context, ask which second writer makes it necessary, and whether that writer can be moved onto the executor.
- Order start-up by events, not by delays or checks: turn on the data source only from the completion event that proves the receive side is ready.
- Functions establish their postconditions on every path, so callers never have to compensate. Clear all per-session state at session start.
- Fixes for races must remove the racing decision, not close one interleaving. If two fixes for the same symptom have failed, the diagnosis is too narrow.

# Documentation

The project maintains a small, current documentation set:
- An architecture document describing how the driver works and why, justified on technical grounds.
- An interface contract for the driver's client-facing interface (IOCTLs, properties), precise enough to reimplement against.
- A future-work document listing every known gap between the documented design and the code. A gap is either fixed or listed here. Documentation never describes intended behavior as current behavior.
- Review findings live in dated report documents with numbered findings and a status header that records what was fixed, deferred, or rejected.

Every behavior-changing commit updates the documentation and comments it invalidates, in the same change. When correcting a wrong statement, grep for the phrase: duplicated claims drift independently across docs, headers, and comments.

# Testing

- The test application exercises every mode and feature combination the driver exposes and verifies delivered data, not just success codes.
- The simulator renders test patterns that can be reconstructed mathematically, with per-frame metadata and checksums embedded in the data, so corrupted output can be decoded into a precise measurement of what was lost.
- The simulator supports fault injection through its normal command channel, so driver error paths can be exercised deterministically.
- Concurrency tests assert liveness (every thread finishes within a bound) and post-test health (a full normal cycle still works afterwards), not per-operation outcomes, which are timing-dependent.
- The simulator and all test tools are held to the same hardening bar as the driver: bounded loops, honored timeouts, clean teardown. Debugging depends on them precisely when everything else is failing.
- Diagnostic tools reject or honor unexpected arguments, never ignore them silently.
