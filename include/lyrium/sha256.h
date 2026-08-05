#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

namespace lyrium
{

class Sha256
{
  public:
    Sha256()
        : state_{0x6A09E667u, 0xBB67AE85u, 0x3C6EF372u, 0xA54FF53Au, 0x510E527Fu, 0x9B05688Cu, 0x1F83D9ABu, 0x5BE0CD19u}
        , buffer_{}
        , length_{}
        , buffered_{}
    {
    }

    auto update(const void *data, std::size_t size) -> Sha256 &
    {
        const auto *bytes = static_cast<const std::uint8_t *>(data);
        length_ += size;

        while (size > 0)
        {
            const auto chunk = std::min(sizeof(buffer_) - buffered_, size);
            std::memcpy(buffer_.data() + buffered_, bytes, chunk);
            buffered_ += chunk;
            bytes += chunk;
            size -= chunk;

            if (buffered_ == sizeof(buffer_))
            {
                compress(buffer_.data());
                buffered_ = 0u;
            }
        }

        return *this;
    }

    auto finish() -> std::array<std::uint8_t, 32u>
    {
        const auto bit_length = static_cast<std::uint64_t>(length_) * 8u;

        static constexpr auto padding_byte = std::uint8_t{0x80u};
        update(&padding_byte, 1u);
        static constexpr auto zero = std::uint8_t{0u};
        while (buffered_ != 56u)
        {
            update(&zero, 1u);
        }

        for (auto shift = 56; shift >= 0; shift -= 8)
        {
            buffer_[buffered_++] = static_cast<std::uint8_t>((bit_length >> shift) & 0xFFu);
        }
        compress(buffer_.data());

        auto digest = std::array<std::uint8_t, 32u>{};
        for (auto i = 0u; i < 8u; ++i)
        {
            digest[i * 4u + 0u] = static_cast<std::uint8_t>((state_[i] >> 24) & 0xFFu);
            digest[i * 4u + 1u] = static_cast<std::uint8_t>((state_[i] >> 16) & 0xFFu);
            digest[i * 4u + 2u] = static_cast<std::uint8_t>((state_[i] >> 8) & 0xFFu);
            digest[i * 4u + 3u] = static_cast<std::uint8_t>(state_[i] & 0xFFu);
        }

        // finish() ends a message, not the object. Without this the padding leaves
        // buffered_ at 64 and state_ carrying the previous chaining value, so a
        // reused hasher compresses a stale block and returns a wrong digest.
        *this = Sha256{};

        return digest;
    }

    static auto hex(const void *data, std::size_t size) -> std::string
    {
        auto hasher = Sha256{};
        hasher.update(data, size);
        const auto digest = hasher.finish();

        static constexpr auto digits = "0123456789abcdef";
        auto out = std::string{};
        out.reserve(64u);
        for (const auto byte : digest)
        {
            out.push_back(digits[byte >> 4]);
            out.push_back(digits[byte & 0x0Fu]);
        }
        return out;
    }

  private:
    static auto rotr(std::uint32_t value, int amount) -> std::uint32_t
    {
        return (value >> amount) | (value << (32 - amount));
    }

    auto compress(const std::uint8_t *block) -> void
    {
        static constexpr std::uint32_t k[64] = {
            0x428A2F98u, 0x71374491u, 0xB5C0FBCFu, 0xE9B5DBA5u, 0x3956C25Bu, 0x59F111F1u, 0x923F82A4u, 0xAB1C5ED5u,
            0xD807AA98u, 0x12835B01u, 0x243185BEu, 0x550C7DC3u, 0x72BE5D74u, 0x80DEB1FEu, 0x9BDC06A7u, 0xC19BF174u,
            0xE49B69C1u, 0xEFBE4786u, 0x0FC19DC6u, 0x240CA1CCu, 0x2DE92C6Fu, 0x4A7484AAu, 0x5CB0A9DCu, 0x76F988DAu,
            0x983E5152u, 0xA831C66Du, 0xB00327C8u, 0xBF597FC7u, 0xC6E00BF3u, 0xD5A79147u, 0x06CA6351u, 0x14292967u,
            0x27B70A85u, 0x2E1B2138u, 0x4D2C6DFCu, 0x53380D13u, 0x650A7354u, 0x766A0ABBu, 0x81C2C92Eu, 0x92722C85u,
            0xA2BFE8A1u, 0xA81A664Bu, 0xC24B8B70u, 0xC76C51A3u, 0xD192E819u, 0xD6990624u, 0xF40E3585u, 0x106AA070u,
            0x19A4C116u, 0x1E376C08u, 0x2748774Cu, 0x34B0BCB5u, 0x391C0CB3u, 0x4ED8AA4Au, 0x5B9CCA4Fu, 0x682E6FF3u,
            0x748F82EEu, 0x78A5636Fu, 0x84C87814u, 0x8CC70208u, 0x90BEFFFAu, 0xA4506CEBu, 0xBEF9A3F7u, 0xC67178F2u};

        auto w = std::array<std::uint32_t, 64u>{};
        for (auto i = 0u; i < 16u; ++i)
        {
            w[i] = (static_cast<std::uint32_t>(block[i * 4u + 0u]) << 24) |
                   (static_cast<std::uint32_t>(block[i * 4u + 1u]) << 16) |
                   (static_cast<std::uint32_t>(block[i * 4u + 2u]) << 8) |
                   static_cast<std::uint32_t>(block[i * 4u + 3u]);
        }
        for (auto i = 16u; i < 64u; ++i)
        {
            const auto s0 = rotr(w[i - 15u], 7) ^ rotr(w[i - 15u], 18) ^ (w[i - 15u] >> 3);
            const auto s1 = rotr(w[i - 2u], 17) ^ rotr(w[i - 2u], 19) ^ (w[i - 2u] >> 10);
            w[i] = w[i - 16u] + s0 + w[i - 7u] + s1;
        }

        auto a = state_[0], b = state_[1], c = state_[2], d = state_[3];
        auto e = state_[4], f = state_[5], g = state_[6], h = state_[7];

        for (auto i = 0u; i < 64u; ++i)
        {
            const auto s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
            const auto ch = (e & f) ^ (~e & g);
            const auto temp1 = h + s1 + ch + k[i] + w[i];
            const auto s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
            const auto maj = (a & b) ^ (a & c) ^ (b & c);
            const auto temp2 = s0 + maj;

            h = g;
            g = f;
            f = e;
            e = d + temp1;
            d = c;
            c = b;
            b = a;
            a = temp1 + temp2;
        }

        state_[0] += a;
        state_[1] += b;
        state_[2] += c;
        state_[3] += d;
        state_[4] += e;
        state_[5] += f;
        state_[6] += g;
        state_[7] += h;
    }

    std::array<std::uint32_t, 8u> state_;
    std::array<std::uint8_t, 64u> buffer_;
    std::size_t length_;
    std::size_t buffered_;
};

}
