#include "simulation/sync_exchange.hpp"

namespace chronos {

SyncSample run_sync_exchange(SourceId      source_id,
                              NsTimestamp   collector_send_time_true,
                              ClockModel&   source_clock,
                              LatencyModel& latency) {
    const NsTimestamp t1 = collector_send_time_true;

    const NsDuration   outbound              = latency.sample_latency(LinkDirection::Outbound);
    const NsTimestamp  source_receive_true   = t1 + outbound;
    const NsTimestamp  t2 = source_clock.to_local(source_receive_true, NoiseChannel::Sync);

    // Source replies immediately -- no processing delay is modeled.
    const NsTimestamp  source_send_true = source_receive_true;
    const NsTimestamp  t3 = source_clock.to_local(source_send_true, NoiseChannel::Sync);

    const NsDuration   inbound = latency.sample_latency(LinkDirection::Inbound);
    const NsTimestamp  t4      = source_send_true + inbound;

    SyncSample sample;
    sample.source_id              = source_id;
    sample.collector_send_time    = t1;
    sample.source_receive_time    = t2;
    sample.source_send_time       = t3;
    sample.collector_receive_time = t4;
    sample.estimated_offset       = ((t2 - t1) + (t3 - t4)) / 2;
    sample.round_trip_delay       = (t4 - t1) - (t3 - t2);
    return sample;
}

std::vector<SyncSample> run_sync_schedule(SourceId      source_id,
                                           NsTimestamp   start_time,
                                           NsTimestamp   end_time,
                                           NsDuration    interval_ns,
                                           ClockModel&   source_clock,
                                           LatencyModel& latency) {
    std::vector<SyncSample> samples;
    if (interval_ns <= 0) return samples;

    for (NsTimestamp t = start_time; t < end_time; t += interval_ns) {
        samples.push_back(run_sync_exchange(source_id, t, source_clock, latency));
    }
    return samples;
}

} // namespace chronos
