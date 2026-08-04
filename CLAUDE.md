# Overview
- photonic: this is a device driver that interfaces with a camera over firewire and exposes a DirectShow interface.
- test: this is a test app that checks that all video modes exposed by the camera/driver work properly.
- fake-fw-dev: this is a Linux application that simulates a real camera. The driver must work with it.

# Architecture

- Use WPP for tracing.
- Each function call shall be traced and print the relevant parameters / state.
- Avoid code duplication.
- Harden against broken hardware (all polling must have a timeout, careful with pointer arithmetic derived from hardware input, etc.).
- Design the driver to minimize risk of crashes, resource leaks, race conditions, infinite loops, etc.
- The driver must not get stuck too long in kernel mode. Ideally, every operation that blocks must be done asynchronously.
- Error recovery should be good but not excessive. It massively increases the driver complexity and at some point becomes overkill.
  It is OK to fail capture and report to user space, which will deal with it as necessary. The driver should only ensure basic
  correctness (no crashes, resource leaks, race conditions, infinite loops, etc.).

# Code Style

## Coding Convention
- Use braces for all blocks, even if they contain only one statement.
- Use Pascal case for naming for Windows code.
- Do not use hungarian notation.
- For Linux code, use snake_case.
- Member variables in C++ code should be prefix with m_.
- Use C++-style comments across the whole code base. Inline C-style comments in parameter lists are ok.
- No leading spaces in log messages.
- Keep the function return type on the same line as the name.
- Do not spread out a function return type, name, parameter list on multiple lines if it can fit on one line reasonably well.

## Comments
- Write accurate and concise comments. Concise does not mean loss of important information.
- Do not use "we" in comments. Be specific (e.g., the driver, the caller, etc.).
- Do not overuse semicolons or em-dashes.
- Use doxygen-style comments where appropriate. Use proper markers for parameters, etc.
- Comments should be easy to read and sound natural.
- Do not use passive voice when the subject would actually convey additional information.
- In a multi-line comment, do not leave one or two words alone on the last line.
- Do not overuse the word "classic" (e.g., classic cause).
- When introducing a new term or abbreviation for the first time, define it, especially if it is driver-specific. Common knowledge does not need to be defined (e.g., any standard driver terminology).