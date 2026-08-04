// Its own binary, deliberately.
//
// This test drives the process-global counters into a wrapped state. Because
// diag/texture_totals.h keeps them in inline variables with no reset hook, any
// other assertion running afterwards in the same process would be measuring
// garbage. Isolating it is the only way to observe the behaviour safely.

#include <cstdint>
#include <limits>

#include <gtest/gtest.h>

#include "lyrium/diag/texture_totals.h"

using lyrium::diag::note_texture_released;
using lyrium::diag::texture_totals;

// Characterisation of a real defect. note_texture_released has no floor: it
// fetch_subs unconditionally, so releasing bytes that were never created wraps
// the unsigned total around to near the top of its range. The same applies to
// live_count.
//
// It is left unfixed on purpose. Phase 3 deletes this header outright in favour
// of an owned TextureLedger whose counters are derived from a record map, which
// makes an unmatched release a no-op by construction rather than by a bounds
// check bolted on afterwards. This test is the specification for that: the
// behaviour recorded here is exactly what the replacement must not do.
TEST(TextureTotalsUnderflow, UnmatchedReleaseWrapsTheTotalInsteadOfClamping)
{
    const auto before = texture_totals();
    ASSERT_EQ(before.total, 0u) << "this test must run first in its own process";

    note_texture_released(0u, 4096u);

    const auto after = texture_totals();

    EXPECT_EQ(after.total, std::numeric_limits<std::uint64_t>::max() - 4096u + 1u)
        << "the total wrapped rather than clamping at zero";
    EXPECT_GT(after.total, 1ull << 60) << "the reported total is now an absurd figure";
    EXPECT_EQ(after.live_count, std::numeric_limits<std::uint64_t>::max())
        << "the live count wrapped too, from zero to its maximum";

    // released_bytes is the one counter that stays sane, because it only ever
    // accumulates.
    EXPECT_EQ(after.released_bytes, 4096u);
}
