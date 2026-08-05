#pragma once

#include <cstddef>
#include <cstdint>

namespace lyrium::dao
{

inline constexpr auto preferred_image_base = std::uintptr_t{0x400000};
inline constexpr auto prologue_bytes = std::size_t{16};

struct Target
{
    const char *name;
    const char *symbol;
    const char *comment;
    std::uintptr_t address;
    std::size_t size;
    std::size_t patch_len;
    std::uint16_t reloc_mask;
    const char *sha256;
    std::uint8_t prologue[prologue_bytes];
};

enum class TargetId : std::size_t
{
    load_texture_file,
    create_texture_cached,
    create_texture_registered,
    stream_load,
    decode_texture_memory,
    create_texture_2d_factory,
    create_texture_from_memory,
    create_volume_from_memory,
    texture_cache_evict,
    texture_cache_clear,
    crt_malloc,
    crt_free,
    crt_realloc,
    count,
};

inline constexpr Target targets[] = {
    {
        .name = "load_texture_file",
        .symbol = "sub_846990",
        .comment = "TextureManager::loadTextureFile - reads whole file into a heap block",
        .address = 0x00846990,
        .size = 489,
        .patch_len = 7,
        .reloc_mask = 0x0078,
        .sha256 = "4efe4369b4096851be44e9d235061e5fbb86810eda19e1db03c808b6a85a0580",
        .prologue = {0x6A, 0xFF, 0x68, 0x81, 0x18, 0xAC, 0x00, 0x64, 0xA1, 0x00, 0x00, 0x00, 0x00, 0x50, 0x83, 0xEC},
    },
    {
        .name = "create_texture_cached",
        .symbol = "sub_846460",
        .comment = "TextureManager::createTexture - caches result without checking it",
        .address = 0x00846460,
        .size = 374,
        .patch_len = 7,
        .reloc_mask = 0x0078,
        .sha256 = "da787b04887ed9cc33c5e96c6b95b42e69ce97225a09bfebd0982c7d08522e40",
        .prologue = {0x6A, 0xFF, 0x68, 0xEC, 0x17, 0xAC, 0x00, 0x64, 0xA1, 0x00, 0x00, 0x00, 0x00, 0x50, 0x83, 0xEC},
    },
    {
        .name = "create_texture_registered",
        .symbol = "sub_847330",
        .comment = "TextureManager::createTexture - render-target/driver registration path",
        .address = 0x00847330,
        .size = 439,
        .patch_len = 7,
        .reloc_mask = 0x0078,
        .sha256 = "bac701cac958ed82c90e7bd67756b59d7ce02bb7f66a5502e5975f7276266f2c",
        .prologue = {0x6A, 0xFF, 0x68, 0x09, 0x1A, 0xAC, 0x00, 0x64, 0xA1, 0x00, 0x00, 0x00, 0x00, 0x50, 0x83, 0xEC},
    },
    {
        .name = "stream_load",
        .symbol = "sub_846B90",
        .comment = "TextureManager::streamLoad - open-stream decoder dispatch",
        .address = 0x00846b90,
        .size = 674,
        .patch_len = 6,
        // The 6 patched bytes are push ebp / mov ebp,esp / and esp,-8, none of
        // which carries an absolute operand, so nothing here is relocatable. The
        // mask previously described the push imm32 at offsets 9..12, which sits
        // outside the patch and could never be consulted.
        .reloc_mask = 0x0000,
        .sha256 = "1af959dfb506c915f51c8ed32d16ba41114d73704158e215d3f1228c688b4a0b",
        .prologue = {0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF8, 0x6A, 0xFF, 0x68, 0xE1, 0x18, 0xAC, 0x00, 0x64, 0xA1, 0x00},
    },
    {
        .name = "decode_texture_memory",
        .symbol = "sub_845910",
        .comment = "decodeTextureMemory - attaches decoded texture without checking it",
        .address = 0x00845910,
        .size = 312,
        .patch_len = 7,
        .reloc_mask = 0x0078,
        .sha256 = "634afddae099599ce1c0e6d45c002b7057b3f5910324f0398dabb93153c89492",
        .prologue = {0x6A, 0xFF, 0x68, 0xCC, 0x15, 0xAC, 0x00, 0x64, 0xA1, 0x00, 0x00, 0x00, 0x00, 0x50, 0x83, 0xEC},
    },
    {
        .name = "create_texture_2d_factory",
        .symbol = "sub_868FB0",
        .comment = "GraphicsDriver::createTexture2D - drops the D3D HRESULT (the core bug)",
        .address = 0x00868fb0,
        .size = 313,
        .patch_len = 7,
        .reloc_mask = 0x0078,
        .sha256 = "54ae518e5e97e8f8018d5c2d36c2b32a1fab7ddddf5bd56ddcb6774d355748ae",
        .prologue = {0x6A, 0xFF, 0x68, 0x8B, 0xC3, 0xA9, 0x00, 0x64, 0xA1, 0x00, 0x00, 0x00, 0x00, 0x50, 0x51, 0x53},
    },
    {
        .name = "create_texture_from_memory",
        .symbol = "sub_869200",
        .comment = "GraphicsDriver::createTextureFromMemory - D3DXCreateTextureFromFileInMemoryEx",
        .address = 0x00869200,
        .size = 405,
        .patch_len = 7,
        .reloc_mask = 0x0078,
        .sha256 = "552a5b628a479c4dc3a70a648100be09aeb807a03f36c521dd4258b44ef809cc",
        .prologue = {0x6A, 0xFF, 0x68, 0x4B, 0x26, 0xAC, 0x00, 0x64, 0xA1, 0x00, 0x00, 0x00, 0x00, 0x50, 0x83, 0xEC},
    },
    {
        .name = "create_volume_from_memory",
        .symbol = "sub_8690F0",
        .comment = "GraphicsDriver volume-texture equivalent of the above",
        .address = 0x008690f0,
        .size = 269,
        .patch_len = 7,
        .reloc_mask = 0x0078,
        .sha256 = "88db1198c0740eff69073abc24c1a98d76e6f2d7d71b72fafe7123912dd4a237",
        .prologue = {0x6A, 0xFF, 0x68, 0x1B, 0x26, 0xAC, 0x00, 0x64, 0xA1, 0x00, 0x00, 0x00, 0x00, 0x50, 0x53, 0x55},
    },
    {
        .name = "texture_cache_evict",
        .symbol = "sub_8481F0",
        .comment = "TextureCache::evict(max) - trickle eviction of pending releases",
        .address = 0x008481f0,
        .size = 529,
        .patch_len = 7,
        .reloc_mask = 0x0078,
        .sha256 = "9cce87e95a7dab20234a9469eb773e075a08e9b3807d6ffa824a50c431e5237a",
        .prologue = {0x6A, 0xFF, 0x68, 0x88, 0x1A, 0xAC, 0x00, 0x64, 0xA1, 0x00, 0x00, 0x00, 0x00, 0x50, 0x83, 0xEC},
    },
    {
        .name = "texture_cache_clear",
        .symbol = "sub_8490A0",
        .comment = "TextureCache::clear - full cache teardown",
        .address = 0x008490a0,
        .size = 495,
        .patch_len = 7,
        .reloc_mask = 0x0078,
        .sha256 = "ce133a2572e9f3dac86f6f72a9ddbb43654afbdce685da116a6f185a85bcda35",
        .prologue = {0x6A, 0xFF, 0x68, 0x88, 0x1A, 0xAC, 0x00, 0x64, 0xA1, 0x00, 0x00, 0x00, 0x00, 0x50, 0x83, 0xEC},
    },
    {
        .name = "crt_malloc",
        .symbol = "_malloc",
        .comment = "statically linked CRT malloc",
        .address = 0x008e47aa,
        .size = 195,
        .patch_len = 5,
        .reloc_mask = 0x0000,
        .sha256 = "f3b9f31d611a8e5b6aa6f48ae57b7bb2d32aa39b6814ad7b6e46c2473bd1a7a9",
        .prologue = {0x55, 0x8B, 0x6C, 0x24, 0x08, 0x83, 0xFD, 0xE0, 0x0F, 0x87, 0x9F, 0x00, 0x00, 0x00, 0x53, 0x8B},
    },
    {
        .name = "crt_free",
        .symbol = "_free",
        .comment = "statically linked CRT free",
        .address = 0x008e1a5a,
        .size = 142,
        .patch_len = 7,
        .reloc_mask = 0x0078,
        .sha256 = "9744a1eac1a3d02ac031cfe73b91c4b831d1a92d8ed7baf34a07c4b69f73f69c",
        .prologue = {0x6A, 0x0C, 0x68, 0xA0, 0x2E, 0xBE, 0x00, 0xE8, 0x0E, 0xCE, 0x00, 0x00, 0x8B, 0x75, 0x08, 0x85},
    },
    {
        .name = "crt_realloc",
        .symbol = "_realloc",
        .comment = "statically linked CRT realloc",
        .address = 0x008e6c0d,
        .size = 539,
        .patch_len = 7,
        .reloc_mask = 0x0078,
        .sha256 = "f60cecfc0027ee4e602541f97044fb13795c9d5e898adcbac5a487a4052e4c08",
        .prologue = {0x6A, 0x10, 0x68, 0xD0, 0x30, 0xBE, 0x00, 0xE8, 0x5B, 0x7C, 0x00, 0x00, 0x8B, 0x5D, 0x08, 0x85},
    },
};

static_assert(
    sizeof(targets) / sizeof(targets[0]) == static_cast<std::size_t>(TargetId::count),
    "target table and TargetId enum disagree");

inline constexpr auto target(TargetId id) -> const Target &
{
    return targets[static_cast<std::size_t>(id)];
}

}
