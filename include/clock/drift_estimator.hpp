#pragma once

#include "clock/sync_sample.hpp"
#include "core/types.hpp"
#include <cstdint>
#include <optional>
#include <vector>

namespace chronos {

// Sliding-window, RTT-weighted linear drift estimator. Unlike the naive
// OffsetEstimator (which just remembers the latest sample), this fits
// offset(t) = intercept + slope*(t - anchor) over several recent samples,
// weighting lower-RTT samples more heavily, and rejects samples whose RTT is
// far above the recent norm before they can pollute the fit. Addresses the
// two things the naive estimator can't: clock drift (a fitted line tracks
// it; a single point can't) and per-sample latency-jitter noise (averaging
// several samples reduces variance; one sample can't).
class DriftAwareOffsetEstimator {
public:
    struct Config {
        std::size_t window_size            = 10;  // samples retained for the weighted fit
        double      outlier_rtt_multiplier = 3.0; // reject if RTT > multiplier * rolling median RTT
    };

    // Two overloads rather than a default argument: a nested class's default
    // member initializers (Config's, above) can't be used in a default
    // argument declared within the same enclosing class body.
    DriftAwareOffsetEstimator();
    explicit DriftAwareOffsetEstimator(Config config);

    // Must be called in non-decreasing sample-time order: outlier rejection
    // is an online decision against recent *accepted* history, unlike
    // OffsetEstimator's insertion-order-independent design. A rejected
    // sample is discarded entirely -- it never enters the accepted store, so
    // it can't influence the fit or a future rolling-median comparison.
    void add_sample(const SyncSample& sample);

    // Weighted-least-squares estimate at query_time, fit over the last
    // window_size *accepted* samples with key <= query_time. Returns
    // std::nullopt if no accepted sample exists at or before query_time.
    std::optional<NsDuration> estimate_at(NsTimestamp query_time) const;

    std::size_t accepted_count() const noexcept { return accepted_.size(); }
    std::size_t rejected_count() const noexcept { return rejected_count_; }

private:
    struct Entry {
        NsTimestamp key; // sync-sample collector-time midpoint
        NsDuration  offset;
        NsDuration  rtt;
    };

    Config              config_;
    std::vector<Entry>  accepted_; // kept in non-decreasing key order
    std::size_t         rejected_count_ = 0;
};

} // namespace chronos
