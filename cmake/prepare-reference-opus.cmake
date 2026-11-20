cmake_minimum_required(VERSION 3.20)

file(COPY "${PSXRC_OPUS_SOURCE}/" DESTINATION "${PSXRC_OPUS_DESTINATION}" PATTERN ".git" EXCLUDE)
set(arch_header "${PSXRC_OPUS_DESTINATION}/celt/arch.h")
file(READ "${arch_header}" contents)
set(architecture_dispatch [=[#if defined(__x86_64__) || defined(__LP64__) || defined(_WIN64) || defined (__mips)
#define OPUS_FAST_INT64 1
#else
#define OPUS_FAST_INT64 0
#endif]=])
string(FIND "${contents}" "${architecture_dispatch}" dispatch_offset)
if(dispatch_offset LESS 0)
    message(FATAL_ERROR "The Opus source does not match the pinned PSXRC fixed-point build.")
endif()
# Existing PSXRC images store checksums produced with the 64-bit arithmetic.
string(REPLACE "${architecture_dispatch}" "#define OPUS_FAST_INT64 1" contents "${contents}")
file(WRITE "${arch_header}" "${contents}")
