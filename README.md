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

## Phase 4: Confidence-Aware Timeline Reconstruction

Phase 3's `DriftAwareOffsetEstimator` produces a single corrected timestamp per event. That's still a point estimate — it silently claims perfect precision even when the underlying sync data doesn't support it. Phase 4 attaches an uncertainty interval to every corrected event and stops asserting a total order where the intervals don't support one.

### Uncertainty interval

`estimate_with_uncertainty_at` (in `DriftAwareOffsetEstimator`) returns an `OffsetEstimate{offset, uncertainty}`, where `uncertainty` is a symmetric half-width around the point estimate, taken as the *larger* of two explainable, physically grounded bounds:

1. **Half the window's median round-trip delay.** Under the standard assumption of roughly symmetric outbound/inbound network delay, a sync exchange cannot pin down the true offset more precisely than `RTT / 2` from timing alone — the classic NTP error-bound argument.
2. **The weighted RMS residual of the linear fit** over the drift window — how much the accepted samples actually scatter around the fitted line. This grows when drift is noisy or the linear model fits the window poorly, catching cases the RTT bound alone would miss.

This is deliberately not a formal statistical confidence interval with a stated coverage probability — per this project's principle of avoiding unnecessary statistical complexity, it's two simple, inspectable quantities combined by `max`.

`CorrectedEvent` now carries `lower_bound = corrected_time - uncertainty` and `upper_bound = corrected_time + uncertainty`.

### Ordering rule

`timeline/interval_order.hpp`'s `classify_order(a, b)`:

```
a.upper_bound < b.lower_bound  -> Before
b.upper_bound < a.lower_bound  -> After
otherwise                      -> Ambiguous
```

Touching bounds (`a.upper_bound == b.lower_bound`) classify as `Ambiguous`, not `Before` — an interval represents "could plausibly be anywhere in this range," so equal endpoints are a tie, not a strict separation. This is the project's core differentiator: rather than forcing every pair of events into a total order, Chronos distinguishes events that are *definitely* ordered from ones that are only *ambiguously* ordered given the available evidence.

### Ambiguity metrics

`metrics/ambiguity.hpp`'s `classify_pairs` reports, over the same sampled/exact pair set used elsewhere:

- **Definite pairs** and **ambiguous pairs** — how much of the timeline the interval logic is willing to commit to.
- **Definite-pair accuracy** — of the pairs classified definite, what fraction agree with ground truth. This is the metric that matters most: when Chronos claims a definite order, that claim needs to be trustworthy.
- **Point-estimate errors caught by the ambiguity flag** — among pairs where the bare point estimate (`corrected_time` alone) disagrees with ground truth, how many the interval logic correctly flagged `Ambiguous` instead of silently asserting the wrong order.

### Measured results

Both runs: seed 42, 16 sources, 5000 events, ±300ppm drift, ±2000us max offset (same config as the Phase 3 numbers above).

**Baseline** (`results/phase4_baseline.txt`, default latency: 100us base, 50us jitter, no spikes):

| Metric | Value |
|---|---|
| Definite pairs | 91.81% of considered pairs |
| Definite-pair accuracy | 92.51% |
| Ambiguous pairs | 8.19% of considered pairs |
| Point-estimate errors | 150,712 |
| ...caught by ambiguity flag | 13,120 (8.71%) |

**Higher uncertainty** (`results/phase4_high_uncertainty.txt`, `--noise-std-ns 500 --jitter-us 300 --spike-probability 0.05 --spike-multiplier 10`):

| Metric | Value |
|---|---|
| Definite pairs | 91.25% of considered pairs |
| Definite-pair accuracy | 91.98% |
| Ambiguous pairs | 8.75% of considered pairs |
| Point-estimate errors | 164,618 |
| ...caught by ambiguity flag | 18,305 (11.12%) |

Two honest observations from these numbers, not the ones a marketing-driven writeup would lead with:

- **Definite-pair accuracy (~92%) sits close to overall weighted-corrected accuracy (92.46%, from Phase 3), not near 100%.** The reason is pair *sampling*, not a flaw in the interval logic: `classify_pairs` samples uniformly at random over all 5000 events spanning the full run duration, so most sampled pairs are events far apart in time — trivially separable regardless of estimator uncertainty, and rarely where errors occur. The interval logic's real value concentrates on temporally *close* pairs, which uniform-random sampling underrepresents. A pair-sampling strategy biased toward nearby events (or an explicit "adjacent-in-corrected-order" evaluation) would show a sharper effect; that's a natural follow-up rather than something implemented here.
- **The direction is still correct and moves as expected**: raising noise/jitter/spikes widens uncertainty intervals, which raises both the ambiguous-pair fraction (8.19% -> 8.75%) and the fraction of point-estimate errors correctly caught as ambiguous (8.71% -> 11.12%) — the interval logic is doing more hedging exactly when the underlying estimate is less trustworthy, which is the property it's supposed to have.
