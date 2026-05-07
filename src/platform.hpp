// platform.hpp - small platform abstraction for yacc.cpp.
//
// All strings crossing this boundary are UTF-8.  The platform layer is the
// only place that talks to the OS; the core (yacc.cpp) uses neither <cstdio>
// nor any iostream header.  On Windows, the implementation converts to/from
// UTF-16 at the syscall boundary so file paths and console output behave
// correctly with non-ASCII names.
//
// One platform_*.cpp is selected by the build system; no #ifdefs in callers.
#pragma once

#include <string>
#include <string_view>

namespace yacc {

// File I/O.  Treats path as UTF-8.  ok==true on success.
struct ReadResult {
    std::string content;     // UTF-8 bytes (binary-safe)
    bool ok = false;
};
ReadResult read_file(std::string_view path);
bool write_file(std::string_view path, std::string_view data);

// Console I/O.  Bytes are UTF-8; the platform converts for the active console.
// Pipes/files receive raw UTF-8.  Always writes the full buffer.
void write_stdout(std::string_view s);
void write_stderr(std::string_view s);

// Entry point for the program.  The platform's main() converts argv
// to UTF-8 (on Windows) and calls into this function.
int yacc_main(int argc, char** argv);

} // namespace yacc
