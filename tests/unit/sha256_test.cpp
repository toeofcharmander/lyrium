#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

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

auto hash_of(std::string_view text) -> std::string
{
    return Sha256::hex(text.data(), text.size());
}

// The published NIST test vectors. These are what make this an implementation of
// SHA-256 rather than an implementation of something that resembles it, and they
// matter because dao/targets.h trusts these digests to decide whether it is safe
// to patch the game executable.
constexpr auto empty_digest = "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";
constexpr auto abc_digest = "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";
constexpr auto two_block_digest = "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1";
constexpr auto long_digest = "cf5b16a778af8380036ce59e7b0492370b249b11e8f07a51afac45037afee9d1";

constexpr auto two_block_input = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
constexpr auto long_input = "abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijklmno"
                            "ijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu";

}

TEST(Sha256Test, MatchesNistVectors)
{
    EXPECT_EQ(hash_of(""), empty_digest);
    EXPECT_EQ(hash_of("abc"), abc_digest);
    EXPECT_EQ(hash_of(two_block_input), two_block_digest);
    EXPECT_EQ(hash_of(long_input), long_digest);
}

TEST(Sha256Test, MatchesOneMillionRepeatedCharacters)
{
    const std::string input(1000000u, 'a');
    EXPECT_EQ(hash_of(input), "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}

TEST(Sha256Test, StreamingInChunksMatchesASingleUpdate)
{
    // The whole point of update() is that a caller can feed a file in pieces, as
    // diag/process_info.h does with 64 KB reads. Chunk sizes around the 64 byte
    // block boundary are where an off-by-one in the buffering would show.
    const std::string input(200u, 'x');

    for (const std::size_t chunk : {std::size_t{1}, std::size_t{63}, std::size_t{64}, std::size_t{65}, std::size_t{128}})
    {
        Sha256 hasher{};
        for (std::size_t offset = 0u; offset < input.size(); offset += chunk)
        {
            const auto remaining = input.size() - offset;
            hasher.update(input.data() + offset, remaining < chunk ? remaining : chunk);
        }

        EXPECT_EQ(to_hex(hasher.finish()), hash_of(input)) << "chunk size " << chunk;
    }
}

TEST(Sha256Test, HandlesEveryLengthAroundThePaddingBoundary)
{
    // finish() pads until the buffer holds 56 bytes and then appends the 64 bit
    // length. Inputs whose final block lands at 55, 56 or 57 bytes are the ones
    // that force an extra compression round.
    for (std::size_t length = 50u; length <= 70u; ++length)
    {
        const std::string input(length, 'a');

        Sha256 hasher{};
        hasher.update(input.data(), input.size());

        EXPECT_EQ(to_hex(hasher.finish()), hash_of(input)) << "input length " << length;
    }
}

TEST(Sha256Test, ProducesLowercaseHexOfExactlySixtyFourCharacters)
{
    const auto digest = hash_of("anything");

    ASSERT_EQ(digest.size(), 64u);
    for (const auto character : digest)
    {
        const auto is_lower_hex = (character >= '0' && character <= '9') || (character >= 'a' && character <= 'f');
        EXPECT_TRUE(is_lower_hex) << "unexpected character '" << character << "' in digest";
    }
}

// Regression: finish() used to leave buffered_ at 64 and state_ carrying the
// previous chaining value, so a reused hasher compressed a stale block and
// returned a wrong digest. Latent rather than live at the time only because the
// static hex() helper always constructs a fresh hasher.
TEST(Sha256Test, HasherIsReusableAfterFinish)
{
    const auto expected = Sha256::hex("abc", 3u);

    Sha256 hasher{};
    hasher.update("abc", 3u);
    ASSERT_EQ(to_hex(hasher.finish()), expected);

    hasher.update("abc", 3u);
    EXPECT_EQ(to_hex(hasher.finish()), expected) << "finish() must reset the hasher for the next message";

    // A different message through the same object must also be correct, which
    // catches a reset that clears the buffer but not the length counter.
    hasher.update(two_block_input, 56u);
    EXPECT_EQ(to_hex(hasher.finish()), two_block_digest);
}

TEST(Sha256Test, EmptyUpdateDoesNotChangeTheResult)
{
    Sha256 hasher{};
    hasher.update("", 0u);
    hasher.update("abc", 3u);
    hasher.update("", 0u);

    EXPECT_EQ(to_hex(hasher.finish()), abc_digest);
}
