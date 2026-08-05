#pragma once

#include "lyrium/policy/rescue_coordinator.h"

namespace lyrium
{

// The rescue coordinator's own tally, published for the overlay.
//
// It exists because the overlay previously reported rescue activity from
// dao::engine_state().evictions, which counts every call through the engine's
// emergency-evict hook -- including the hundreds the game makes managing its own
// texture cache. The panel therefore read RECLAIMING during ordinary streaming
// while this coordinator had done nothing. Only these counters describe what
// lyrium did.
//
// Read without a lock from the render thread. The counters are diagnostics and a
// torn read costs a wrong digit for one frame, where taking a lock on the render
// path would cost a stall.
[[nodiscard]] auto rescue_stats() -> policy::RescueStats;

}
