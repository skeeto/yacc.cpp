// Fuzz target: feed bytes as a .y file through yacc.cpp's pipeline.
// Compiled with libFuzzer + ASan + UBSan.

#include <cstddef>
#include <cstdint>
#include "yacc.cpp"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    yacc::fuzz_run_buffer(reinterpret_cast<const char*>(data), size);
    return 0;
}
