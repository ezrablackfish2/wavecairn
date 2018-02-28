#include "wavecairn/session.h"

#include <cstddef>
#include <cstdint>
#include <span>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    wavecairn::fuzz_process(std::span<const std::uint8_t>(data, size),
                            wavecairn::ProcessingProfile::routing);
    return 0;
}
