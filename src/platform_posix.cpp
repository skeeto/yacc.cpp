// POSIX platform layer for yacc.cpp.  Compiled only on non-Windows hosts.
//
// Uses raw POSIX syscalls — no stdio, no iostream.  Path strings and
// argv are already UTF-8 on every modern POSIX system, so no conversion
// is needed.

#include "platform.hpp"

#include <cerrno>
#include <cstring>

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace yacc {

static std::string null_terminated(std::string_view sv) {
    return std::string(sv);
}

static bool write_all(int fd, const void* buf, size_t len) {
    const char* p = static_cast<const char*>(buf);
    while (len > 0) {
        ssize_t n = ::write(fd, p, len);
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        p   += n;
        len -= static_cast<size_t>(n);
    }
    return true;
}

ReadResult read_file(std::string_view path) {
    ReadResult r;
    std::string p = null_terminated(path);
    int fd = ::open(p.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return r;
    struct stat st;
    if (::fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0) {
        r.content.resize(static_cast<size_t>(st.st_size));
        size_t off = 0;
        while (off < r.content.size()) {
            ssize_t n = ::read(fd, &r.content[off], r.content.size() - off);
            if (n < 0) {
                if (errno == EINTR) continue;
                ::close(fd);
                return r;
            }
            if (n == 0) { r.content.resize(off); break; }
            off += static_cast<size_t>(n);
        }
    } else {
        // Pipe or unknown size - read until EOF.
        char buf[4096];
        for (;;) {
            ssize_t n = ::read(fd, buf, sizeof(buf));
            if (n < 0) {
                if (errno == EINTR) continue;
                ::close(fd);
                return r;
            }
            if (n == 0) break;
            r.content.append(buf, static_cast<size_t>(n));
        }
    }
    ::close(fd);
    r.ok = true;
    return r;
}

bool write_file(std::string_view path, std::string_view data) {
    std::string p = null_terminated(path);
    int fd = ::open(p.c_str(),
                    O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC,
                    0644);
    if (fd < 0) return false;
    bool ok = write_all(fd, data.data(), data.size());
    if (::close(fd) != 0) ok = false;
    return ok;
}

void write_stdout(std::string_view s) {
    write_all(STDOUT_FILENO, s.data(), s.size());
}

void write_stderr(std::string_view s) {
    write_all(STDERR_FILENO, s.data(), s.size());
}

} // namespace yacc

#ifndef YACC_CPP_NO_MAIN
int main(int argc, char** argv) {
    return yacc::yacc_main(argc, argv);
}
#endif
