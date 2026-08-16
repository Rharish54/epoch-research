#pragma once

#include "core/types.hpp"

namespace chronos {

// One four-timestamp synchronization round trip between the collector and a
// single source:
//   t1: collector sends sync request
//   t2: source receives sync request
//   t3: source sends sync response
//   t4: collector receives sync response
//
// t1 and t4 are true time (the collector holds the reference clock); t2 and
// t3 are the source's distorted local clock.
struct SyncSample {
    SourceId    source_id;
    NsTimestamp collector_send_time;
    NsTimestamp source_receive_time;
    NsTimestamp source_send_time;
    NsTimestamp collector_receive_time;
    NsDuration  estimated_offset;
    NsDuration  round_trip_delay;
};

} // namespace chronos
