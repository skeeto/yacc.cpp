// Win32 platform layer for yacc.cpp.  Compiled only on Windows.
//
// The OS uses UTF-16 (wide chars) for paths and console output, while
// our core works in UTF-8.  This file converts at the boundary using
// MultiByteToWideChar / WideCharToMultiByte so that filenames with
// non-ASCII characters and console output both work correctly.

#include "platform.hpp"

#ifndef _WIN32
#  error "platform_win32.cpp must only be compiled on Windows"
#endif

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shellapi.h>

#include <vector>

namespace yacc {

static std::wstring utf8_to_wide(std::string_view s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    if (n <= 0) return {};
    std::wstring out(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), out.data(), n);
    return out;
}

static std::string wide_to_utf8(const wchar_t* w, int wlen = -1) {
    if (!w) return {};
    if (wlen < 0) wlen = (int)wcslen(w);
    if (wlen == 0) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w, wlen, nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, wlen, out.data(), n, nullptr, nullptr);
    return out;
}

static bool write_handle_raw(HANDLE h, const void* data, size_t len) {
    const char* p = static_cast<const char*>(data);
    while (len > 0) {
        DWORD n = 0;
        DWORD chunk = (len > 0x40000000u) ? 0x40000000u : (DWORD)len;
        if (!WriteFile(h, p, chunk, &n, nullptr)) return false;
        if (n == 0) return false;
        p   += n;
        len -= n;
    }
    return true;
}

// For consoles, route through WriteConsoleW to render UTF-8 correctly
// regardless of the active code page.  For pipes/files, pass raw bytes.
static bool write_console_or_file(HANDLE h, std::string_view s) {
    DWORD mode;
    if (GetConsoleMode(h, &mode)) {
        std::wstring wide = utf8_to_wide(s);
        const wchar_t* p = wide.data();
        DWORD remaining = (DWORD)wide.size();
        while (remaining > 0) {
            DWORD written = 0;
            if (!WriteConsoleW(h, p, remaining, &written, nullptr))
                return false;
            p         += written;
            remaining -= written;
        }
        return true;
    }
    return write_handle_raw(h, s.data(), s.size());
}

ReadResult read_file(std::string_view path) {
    ReadResult r;
    std::wstring wpath = utf8_to_wide(path);
    HANDLE h = CreateFileW(wpath.c_str(), GENERIC_READ, FILE_SHARE_READ,
                           nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return r;

    LARGE_INTEGER size{};
    if (GetFileSizeEx(h, &size) && size.QuadPart > 0) {
        r.content.resize(static_cast<size_t>(size.QuadPart));
        size_t off = 0;
        while (off < r.content.size()) {
            DWORD chunk = (DWORD)((r.content.size() - off > 0x40000000u)
                                  ? 0x40000000u : (r.content.size() - off));
            DWORD n = 0;
            if (!ReadFile(h, &r.content[off], chunk, &n, nullptr)) {
                CloseHandle(h);
                return r;
            }
            if (n == 0) { r.content.resize(off); break; }
            off += n;
        }
    } else {
        char buf[4096];
        for (;;) {
            DWORD n = 0;
            if (!ReadFile(h, buf, sizeof(buf), &n, nullptr)) break;
            if (n == 0) break;
            r.content.append(buf, n);
        }
    }
    CloseHandle(h);
    r.ok = true;
    return r;
}

bool write_file(std::string_view path, std::string_view data) {
    std::wstring wpath = utf8_to_wide(path);
    HANDLE h = CreateFileW(wpath.c_str(), GENERIC_WRITE, 0,
                           nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    bool ok = write_handle_raw(h, data.data(), data.size());
    if (!CloseHandle(h)) ok = false;
    return ok;
}

void write_stdout(std::string_view s) {
    write_console_or_file(GetStdHandle(STD_OUTPUT_HANDLE), s);
}

void write_stderr(std::string_view s) {
    write_console_or_file(GetStdHandle(STD_ERROR_HANDLE), s);
}

} // namespace yacc

#ifndef YACC_CPP_NO_MAIN
int main(int /*argc*/, char** /*argv*/) {
    // Ignore the CRT-supplied argv (which is built from the active ANSI
    // code page on legacy CRTs and may mojibake non-ASCII).  Use the
    // OS-native UTF-16 command line and convert ourselves.
    int argc = 0;
    wchar_t** wargv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!wargv) return 1;

    std::vector<std::string> args_storage;
    args_storage.reserve(static_cast<size_t>(argc));
    for (int i = 0; i < argc; i++)
        args_storage.push_back(yacc::wide_to_utf8(wargv[i]));
    LocalFree(wargv);

    std::vector<char*> argv_ptrs;
    argv_ptrs.reserve(static_cast<size_t>(argc) + 1);
    for (auto& a : args_storage) argv_ptrs.push_back(a.data());
    argv_ptrs.push_back(nullptr);

    return yacc::yacc_main(argc, argv_ptrs.data());
}
#endif
