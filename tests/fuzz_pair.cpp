// libFuzzer target for what a client may send before it is known: the
// pairing line, and the name cleaning behind it.
#include "pairing.h"

#include <cstddef>
#include <cstdint>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t *data, std::size_t size)
{
    const std::string_view line(reinterpret_cast<const char *>(data), size);
    if (const auto request = pvai::parsePair(line)) {
        if (request->proof.size() != 64 || request->name.size() > pvai::kDeviceNameMax)
            __builtin_trap();
    }
    // base32 both ways: what decodes encodes back to the same text.
    if (const auto bytes = pvai::base32Decode(line)) {
        if (pvai::base32(reinterpret_cast<const unsigned char *>(bytes->data()), bytes->size()) != line)
            __builtin_trap();
    }
    const std::string name = pvai::cleanName(line);
    if (name.empty() || name.size() > pvai::kDeviceNameMax)
        __builtin_trap();
    return 0;
}
