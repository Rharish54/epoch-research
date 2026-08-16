#pragma once

#include "clock/clock_model.hpp"
#include "clock/sync_sample.hpp"
#include "core/types.hpp"
#include "network/latency_model.hpp"
#include <vector>

namespace chronos {

// Runs a single four-timestamp sync round trip for one source, with the
// collector sending at `collector_send_time_true` (true time). The source is
// modeled as replying immediately on receipt -- no processing delay.
SyncSample run_sync_exchange(SourceId          source_id,
                              NsTimestamp       collector_send_time_true,
                              ClockModel&       source_clock,
                              LatencyModel&     latency);

// Runs sync exchanges for one source at a fixed interval, one per tick of
// `interval_ns` covering [start_time, end_time). Returns an empty vector if
// interval_ns <= 0.
std::vector<SyncSample> run_sync_schedule(SourceId      source_id,
                                           NsTimestamp   start_time,
                                           NsTimestamp   end_time,
                                           NsDuration    interval_ns,
                                           ClockModel&   source_clock,
                                           LatencyModel& latency);

} // namespace chronos
