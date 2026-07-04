#pragma once

#include "core/types.hpp"
#include <optional>
#include <vector>

namespace chronos {

struct MarketEvent {
    EventId      event_id;
    SourceId     source_id;
    EventType    event_type;

    // Ground truth — known only to the simulator; never used during reconstruction.
    NsTimestamp  true_time;

    // What the source's local clock reported when the event was generated.
    NsTimestamp  source_local_time;

    // When the collector received the event (populated in Phase 6).
    NsTimestamp  receive_time{0};

    // Optional causal parent (Phase 5).
    std::optional<EventId> parent_event_id;
};

} // namespace chronos
