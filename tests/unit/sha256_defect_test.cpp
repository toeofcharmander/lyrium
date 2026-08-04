// Pins a real defect in Sha256 and is EXPECTED TO FAIL until it is fixed.
// Labelled known_defect and excluded from the default test preset.
//
// Do not weaken this assertion to make it pass. Fix Sha256.

#include <array>
#include <cstdint>
#include <string>

#include <gtest/gtest.h>

#include "lyrium/sha256.h"

using lyrium::Sha256;

namespace
{

auto to_hex(const std::array<std::uint8_t, 32u> &digest) -> std::string
{
    static constexpr auto digits = "0123456789abcdef";
    std::string out{};
    out.reserve(64u);
    for (const auto byte : digest)
    {
        out.push_back(digits[byte >> 4]);
        out.push_back(digits[byte & 0x0Fu]);
    }
    return out;
}

}

// DEFECT: finish() computes the digest but never resets state_, length_ or
// buffered_. It leaves buffered_ at 64, so a subsequent update() immediately
// compresses one stale block before doing anything useful, and it carries the
// previous message's chaining state and length. A reused hasher therefore returns
// a wrong digest rather than rehashing.
//
// This is latent rather than live today only because Sha256::hex() constructs a
// fresh hasher for every call. It becomes live the moment anyone reuses one.
//
// Fix: reset the full state at the end of finish().
TEST(Sha256Defect, HasherCanBeReusedAfterFinish)
{
    const auto expected = Sha256::hex("abc", 3u);

    Sha256 hasher{};
    hasher.update("abc", 3u);
    const auto first = to_hex(hasher.finish());
    ASSERT_EQ(first, expected) << "the first pass should be correct";

    hasher.update("abc", 3u);
    const auto second = to_hex(hasher.finish());

    EXPECT_EQ(second, expected) << "reusing the hasher produced a different digest: finish() does not reset its state";
}
