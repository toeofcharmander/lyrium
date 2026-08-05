#pragma once

#include <cstdint>

namespace lyrium::diag
{

// Whether the engine is failing to create its own textures.
//
// This exists because `main_pool_mb` has two walls and only one of them is
// visible. Shrinking the engine's main memory pool is the largest address-space
// win available -- 850 MB down to 704 MB moved the largest free block below the
// 2 GB line from 2.4 MB to 78.2 MB, and the rescue stopped arming entirely. Cut
// too far and the engine starves inside its own pool, and its failure mode is to
// **skip the asset and carry on**.
//
// A run at 512 MB reported `creates=4622 failures=0` while the world was
// visibly missing geometry. D3D never failed, because the starvation happened a
// layer below it, so nothing lyrium logged could see it and the only detector
// was a person looking at the screen. That makes the knob unsafe to recommend to
// anyone.
//
// The engine's own counters have been collected since long before this and
// printed nowhere -- the same trap CLAUDE.md records from the other direction,
// where a permanent `texture loads: 0` was mistaken for a broken gauge. A
// counter nobody reads is not a diagnostic.
//
// Portable and Windows-free so the verdict is tested; `dao/engine_hooks.h`
// reaches `windows.h` and cannot be.

enum class EngineHealth : std::uint8_t
{
    // Not enough traffic to judge. Deliberately not folded into healthy: a
    // single failure among the first few creates is not a starving pool, and
    // reporting health before there is evidence is how a gauge earns distrust.
    unknown,
    healthy,
    // Failing, but rarely. Worth naming because a good session produces none at
    // all -- 5313 engine creates and zero failures at 704 MB.
    degraded,
    starving,
};

struct EngineHealthConfig
{
    // Below this, say unknown. Load-in produces hundreds of creates in the first
    // seconds, so this costs nothing in responsiveness.
    std::uint64_t minimum_creates{64};

    // Failures per thousand creates at which this stops being bad luck. 10 is
    // 1%; the 512 MB run was far past it and a healthy run sits at exactly zero.
    std::uint64_t starving_permille{10};
};

[[nodiscard]] constexpr auto diagnose_engine_health(
    std::uint64_t creates,
    std::uint64_t failures,
    const EngineHealthConfig &config) -> EngineHealth
{
    if (creates < config.minimum_creates)
    {
        return EngineHealth::unknown;
    }
    if (failures == 0u)
    {
        return EngineHealth::healthy;
    }

    // failures cannot really exceed creates, but the two are read from separate
    // relaxed atomics and a torn pair can say otherwise for one sample. Compare
    // by multiplication rather than dividing, so there is nothing to overflow or
    // divide by.
    return failures * 1000u >= creates * config.starving_permille ? EngineHealth::starving : EngineHealth::degraded;
}

[[nodiscard]] constexpr auto name_of(EngineHealth health) -> const char *
{
    switch (health)
    {
        case EngineHealth::healthy: return "healthy";
        case EngineHealth::degraded: return "degraded";
        case EngineHealth::starving: return "starving";
        case EngineHealth::unknown: break;
    }
    return "unknown";
}

// Whether this verdict deserves a line of its own.
//
// Only on the way down, and only once per level. The sampler runs every five
// seconds, so warning on every starving verdict would print the same line for
// the rest of the session and bury the log it exists to make readable.
[[nodiscard]] constexpr auto worth_warning(EngineHealth current, EngineHealth previous) -> bool
{
    if (current != EngineHealth::starving)
    {
        return false;
    }
    return previous != EngineHealth::starving;
}

}
