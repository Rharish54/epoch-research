# epoch-research
C++20 distributed timeline reconstructor which corrects clock skew, drift, and network jitter across concurrent market-data sources to recover true event ordering.

## Phase 3: Drift Estimation and Outlier Rejection

Phase 2 introduced a naive offset estimator (`OffsetEstimator`) that just remembers the most recent sync sample. That's exact when a source's clock has zero drift, but real clocks drift over time, and any single sync sample also carries whatever network jitter happened to hit it at that moment.

Phase 3 adds `DriftAwareOffsetEstimator`, a sliding-window linear model:

```
estimated_offset(t) = intercept + slope * (t - anchor)
```

fit by weighted least squares over the last `--drift-window` accepted sync samples (default 10), where each sample is weighted by `1/RTT` — lower round-trip-time samples are trusted more, since RTT is a proxy for how much one-way network jitter could have polluted that sample's offset estimate. The fit is clamped to the observed sample range rather than extrapolated, since a slope fit from a handful of noisy samples gets increasingly unreliable the further it's projected.

**Outlier rejection**: before a sync sample enters the window, it's rejected outright if its RTT exceeds `--outlier-rtt-multiplier` (default 3x) times the rolling median RTT of recently accepted samples. This is a deliberately simple, explainable rule rather than a more statistically sophisticated one — per this project's principles, correctness and clarity come before cleverness. The first 3 samples for a source are always accepted (bootstrap), since there's no history yet to judge them against.

### Measured results

All numbers below are produced by `./build/chronos` (Release build, deterministic seed 42, 16 sources, 5000 events, ±300ppm drift, ±2000us max offset). Commands and raw output are in `results/`.

**Baseline** (`results/phase3_baseline.txt`, default latency: 100us base, 50us jitter, no spikes):

| Metric | Raw | Naive (Phase 2) | Weighted (Phase 3) |
|---|---|---|---|
| Ordering accuracy | 71.36% | 92.22% | 92.46% |
| Correction error median / p95 / p99 (ns) | — | 22893 / 68008 / 90657 | 17293 / 44113 / 65660 |

**Under latency spikes** (`results/phase3_latency_spikes.txt`, `--spike-probability 0.05 --spike-multiplier 10`, 5.65% of sync samples rejected as RTT outliers):

| Metric | Raw | Naive (Phase 2) | Weighted (Phase 3) |
|---|---|---|---|
| Ordering accuracy | 71.36% | 91.70% | 92.45% |
| Correction error median / p95 / p99 (ns) | — | 21276 / 75190 / **699928** | 17336 / 51657 / **73297** |

The naive estimator's p99 error blows up under spikes (a single bad sample directly corrupts the offset estimate); the weighted estimator's p99 barely moves, because RTT-weighting and outlier rejection both suppress the spiky samples' influence on the fit.

**Isolating outlier rejection** (`results/phase3_latency_spikes_no_rejection.txt`, same spike config but `--outlier-rtt-multiplier 100000` effectively disables rejection): weighted correction error was 15844 / 48231 / 64296 ns — marginally *better* than with rejection enabled. This is an honest result, not cherry-picked: at this spike level, the `1/RTT` weighting in the least-squares fit already suppresses spiky samples' influence, so hard rejection is largely redundant here. Rejection is expected to matter more as pollution gets severe enough that down-weighted (but not excluded) samples still measurably drag the fit — this project reports what was measured rather than what was expected.
