#include "clock/drift_estimator.hpp"

#include <algorithm>
#include <cmath>

namespace chronos {

namespace {
// Number of leading samples always accepted regardless of RTT -- there is no
// history yet to judge them against.
constexpr std::size_t kBootstrapMinSamples = 3;
} // namespace

DriftAwareOffsetEstimator::DriftAwareOffsetEstimator() : DriftAwareOffsetEstimator(Config{}) {}

DriftAwareOffsetEstimator::DriftAwareOffsetEstimator(Config config) : config_(config) {}

void DriftAwareOffsetEstimator::add_sample(const SyncSample& sample) {
    if (accepted_.size() >= kBootstrapMinSamples) {
        const std::size_t window_start =
            accepted_.size() > config_.window_size ? accepted_.size() - config_.window_size : 0;

        std::vector<NsDuration> recent_rtts;
        recent_rtts.reserve(accepted_.size() - window_start);
        for (std::size_t i = window_start; i < accepted_.size(); ++i) {
            recent_rtts.push_back(accepted_[i].rtt);
        }
        std::sort(recent_rtts.begin(), recent_rtts.end());
        const NsDuration median_rtt = recent_rtts[recent_rtts.size() / 2];

        if (static_cast<double>(sample.round_trip_delay) >
            static_cast<double>(median_rtt) * config_.outlier_rtt_multiplier) {
            ++rejected_count_;
            return;
        }
    }

    const NsTimestamp key = (sample.collector_send_time + sample.collector_receive_time) / 2;
    accepted_.push_back(Entry{key, sample.estimated_offset, sample.round_trip_delay});
}

std::optional<DriftAwareOffsetEstimator::OffsetEstimate>
DriftAwareOffsetEstimator::estimate_with_uncertainty_at(NsTimestamp query_time) const {
    // accepted_ is kept in non-decreasing key order (samples arrive in
    // chronological order), so an upper_bound-style scan finds the last
    // entry with key <= query_time.
    auto it = std::upper_bound(accepted_.begin(), accepted_.end(), query_time,
        [](NsTimestamp qt, const Entry& e) { return qt < e.key; });
    if (it == accepted_.begin()) return std::nullopt;

    const auto end_idx   = static_cast<std::size_t>(it - accepted_.begin()); // exclusive
    const auto start_idx = end_idx > config_.window_size ? end_idx - config_.window_size : 0;

    const NsTimestamp anchor = accepted_[start_idx].key;
    double Sw = 0.0, Sx = 0.0, Sy = 0.0, Sxx = 0.0, Sxy = 0.0;
    for (std::size_t i = start_idx; i < end_idx; ++i) {
        const double x   = static_cast<double>(accepted_[i].key - anchor);
        const double y   = static_cast<double>(accepted_[i].offset);
        const double rtt = static_cast<double>(std::max<NsDuration>(accepted_[i].rtt, 1));
        const double w   = 1.0 / rtt; // lower RTT -> higher weight

        Sw += w;
        Sx += w * x;
        Sy += w * y;
        Sxx += w * x * x;
        Sxy += w * x * y;
    }

    double slope, intercept;
    const double denom = Sw * Sxx - Sx * Sx;
    if (end_idx - start_idx < 2 || std::abs(denom) < 1e-9) {
        slope     = 0.0;
        intercept = Sy / Sw; // weighted-mean fallback
    } else {
        slope     = (Sw * Sxy - Sx * Sy) / denom;
        intercept = (Sy - slope * Sx) / Sw;
    }

    // Don't extrapolate the fitted line beyond the window it was fit on: a
    // slope estimated from a handful of noisy samples is itself noisy, and
    // that noise is amplified by however far past the window query_time
    // falls -- e.g. under heavy outlier rejection, accepted samples thin
    // out, windows span longer real time, and an unclamped extrapolation can
    // overshoot far worse than the naive single-sample estimator ever would.
    // Clamping to the observed range trades a small bias (no drift
    // correction past the last sample) for a hard bound on worst-case error.
    const double x_min = static_cast<double>(accepted_[start_idx].key - anchor);
    const double x_max = static_cast<double>(accepted_[end_idx - 1].key - anchor);
    const double x_query = std::clamp(static_cast<double>(query_time - anchor), x_min, x_max);
    const auto offset = static_cast<NsDuration>(intercept + slope * x_query);

    // Uncertainty half-width: the larger of two explainable, physically
    // grounded bounds (see the OffsetEstimate doc comment in the header).
    //
    // Bound 1 -- half the window's median RTT. Sync exchanges can't resolve
    // offset more precisely than this from timing alone under the standard
    // symmetric-network-delay assumption.
    std::vector<NsDuration> window_rtts;
    window_rtts.reserve(end_idx - start_idx);
    for (std::size_t i = start_idx; i < end_idx; ++i) window_rtts.push_back(accepted_[i].rtt);
    std::sort(window_rtts.begin(), window_rtts.end());
    const double rtt_bound = static_cast<double>(window_rtts[window_rtts.size() / 2]) / 2.0;

    // Bound 2 -- weighted RMS residual of the fit: how much the accepted
    // samples actually scatter around the fitted line. Grows when drift is
    // noisy or the linear model fits the window poorly.
    double weighted_sq_residual = 0.0;
    for (std::size_t i = start_idx; i < end_idx; ++i) {
        const double x   = static_cast<double>(accepted_[i].key - anchor);
        const double y   = static_cast<double>(accepted_[i].offset);
        const double rtt = static_cast<double>(std::max<NsDuration>(accepted_[i].rtt, 1));
        const double w   = 1.0 / rtt;
        const double residual = y - (intercept + slope * x);
        weighted_sq_residual += w * residual * residual;
    }
    const double residual_rms = Sw > 0.0 ? std::sqrt(weighted_sq_residual / Sw) : 0.0;

    const auto uncertainty = static_cast<NsDuration>(std::max(rtt_bound, residual_rms));
    return OffsetEstimate{offset, uncertainty};
}

std::optional<NsDuration> DriftAwareOffsetEstimator::estimate_at(NsTimestamp query_time) const {
    const auto est = estimate_with_uncertainty_at(query_time);
    if (!est) return std::nullopt;
    return est->offset;
}

} // namespace chronos
