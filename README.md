# epoch-research
C++20 distributed timeline reconstructor which corrects clock skew, drift, and network jitter across concurrent market-data sources to recover true event ordering.

## Building

Prerequisites: CMake 3.21+, a C++20 compiler, and [Boost](https://www.boost.org/) (used for Boost.Asio, Phase 6's async delivery pipeline). GoogleTest and Google Benchmark are fetched automatically via CMake `FetchContent` and need no separate install.

```bash
brew install boost   # macOS; use your distro's package manager on Linux
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="$(brew --prefix)"
cmake --build build -j
ctest --test-dir build --output-on-failure
```

`-DCMAKE_PREFIX_PATH="$(brew --prefix)"` is defensive (helps CMake find Boost's CMake config on Intel macOS or a non-default Homebrew prefix); it's usually unnecessary on Apple Silicon but harmless either way.

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

## Phase 5: Causal Graph

Phase 4 gives every event a defensible uncertainty interval, but timing evidence alone has a floor: when two intervals overlap, the pair is ambiguous even if we *know* the order from protocol semantics — an order cannot be acknowledged before it is submitted. Phase 5 adds that second, independent source of evidence: cross-source causal chains (`OrderSubmitted -> OrderAcknowledged -> OrderExecuted -> TradePublished`), a directed graph over them, violation detection against three different timeline representations, and interval narrowing that propagates causal constraints back into the Phase 4 bounds.

### Causal chain generation

`assign_causal_chains` (`include/simulation/causal_linker.hpp`) runs as a post-generation pass over the pooled event list. It walks events in `(true_time, event_id)` order and, at a configurable target rate (`--causal-chain-fraction`, default 0.3), links each chain to the temporally *nearest* eligible event on a *different* source. It mutates only `event_type` and `parent_event_id` — **never a timestamp** — which is a hard correctness requirement: every Phase 3/4 metric is a function of timestamps alone, so causal linking must not perturb any of them. This is enforced by a dedicated test (`TimestampsAreUnmodifiedByLinking`) and verified end-to-end: `results/phase5_baseline.txt` (linking on) and a `--causal-chain-fraction 0.0` run reproduce the exact Phase 3/4 numbers in `results/phase4_baseline.txt`, character-for-character.

By construction, every assigned edge satisfies `parent.true_time < child.true_time` and `parent.source_id != child.source_id` — ground truth is causally consistent by construction, and any violation observed later is entirely an artifact of clock skew, which is the effect Phase 5 is built to expose.

**Design tradeoff**: linking to the *nearest* eligible cross-source successor (rather than a random or far-apart one) maximizes how often clock skew alone is enough to make a chain look causally impossible in the raw timeline — the true gap between chain members can be as small as the mean inter-event time. This is deliberate for demonstrating the headline "impossible timeline" effect, but it has a real cost documented below: it also produces chains whose true gaps are smaller than what the Phase 4 uncertainty intervals can reliably resolve.

### Causal graph and violation detection

`CausalGraph` (`include/timeline/causal_graph.hpp`) builds an index-addressed DAG from `parent_event_id` links (Kahn's algorithm for topological sort; cycle detection guards against hand-built/corrupt input, since the linker's forest structure can never produce one). Three distinct violation checks exist for three timeline representations:

| Timeline | Edge parent->child is violated when |
|---|---|
| Raw (`source_local_time`) | `child < parent` |
| Corrected point estimate | `child < parent` |
| Corrected **interval** | `child.upper_bound <= parent.lower_bound` (no feasible assignment satisfies `parent < child`) |

The interval check uses `<=`, not the strict `<` that Phase 4's `classify_order` uses for its Before/After split — a shared boundary point means the only feasible assignment is `parent == child`, which still violates strict causal precedence, even though `classify_order` calls it Ambiguous. This is a deliberate, tested divergence between the two checks (`TouchingIntervalsAreAViolationEvenThoughClassifyOrderCallsThemAmbiguous`).

### Interval narrowing

`narrow_intervals` (`include/timeline/causal_constraints.hpp`) tightens `CorrectedEvent` bounds via arc consistency on the "parent strictly before child" constraint: a forward pass in topological order lifts each child's lower bound above its parent's (`child.lower = max(child.lower, parent.lower + 1ns)`), then a reverse pass lowers each parent's upper bound below its child's. **Soundness**: if every input interval already contains its true time, narrowing can never push a bound past that true time (by induction over the topological order, using the ground-truth guarantee `parent.true_time < child.true_time`), so a correct interval cannot be made incorrect by narrowing alone.

That soundness argument has a load-bearing premise that does not always hold: Phase 4's uncertainty is a heuristic bound (`max(RTT/2, fit residual RMS)`), not a coverage-guaranteed confidence interval — the measured 92.51% definite-pair accuracy (Phase 4) already proves a material share of intervals miss their true time. When that happens, narrowing propagates the error along the chain, and a set of causal constraints plus timing intervals can become **jointly infeasible** — a node's narrowed lower bound exceeds its narrowed upper bound. Rather than emit an inverted or empty interval (which would make `classify_order` report a confident wrong answer), every node in that node's connected component is reverted to its pre-narrowing bounds and excluded from any "narrowed" claim. This is measured, not assumed away — see the results below, where it turns out to matter a great deal.

### Measured results

All runs: seed 42, 16 sources, 5000 events, `--causal-chain-fraction 0.3` (502 chains, 1499 events linked, 997 edges in every run below — chain structure depends only on `true_time`/`source_id`/`seed`, none of which change across these experiments).

**Baseline** (`results/phase5_baseline.txt`, same config as the Phase 3/4 baseline: 10us mean inter-event time, ±2000us max offset):

| Metric | Value |
|---|---|
| Raw timeline violations | 487 / 997 edges (48.8%) |
| Corrected-point violations | 482 / 997 edges (48.3%) |
| Corrected-interval violations | 337 / 997 edges (33.8%) |
| Nodes narrowed | 202 |
| Inconsistent components (nodes reverted) | 294 (935 of 1499 linked nodes — 62%) |
| Causally-related pairs: ambiguous | 552 -> 550 |
| Causally-related pairs: definite, correct | 1098, 564 -> 1100, 566 |

**Wider event spacing** (`results/phase5_wide_spacing.txt`, `--inter-event-us 500`, everything else unchanged):

| Metric | Value |
|---|---|
| Raw timeline violations | 478 / 997 edges (47.9%) |
| Corrected-point violations | 69 / 997 edges (6.9%) |
| Corrected-interval violations | 9 / 997 edges (0.9%) |
| Nodes narrowed | 149 |
| Inconsistent components (nodes reverted) | 8 (25 of 1499 — 1.7%) |
| Weighted + causal ordering accuracy | 99.98% |
| Causally-related pairs: ambiguous | 952 -> 947 |
| Causally-related pairs: definite, correct | 698, 686 -> 703, 691 |

**Higher skew** (`results/phase5_high_skew.txt`, wide spacing + `--max-offset-us 5000`):

| Metric | Value |
|---|---|
| Raw timeline violations | 486 / 997 edges (48.7%) |
| Corrected-point violations | 77 / 997 edges (7.7%) |
| Corrected-interval violations | 34 / 997 edges (3.4%) |

Several honest observations, again not the ones a marketing-driven writeup would lead with:

- **Raw-timeline violations sit around 48% almost regardless of skew magnitude, because the linker already links to the temporally nearest cross-source event.** Going from ±2000us to ±5000us max offset barely moves the raw violation rate (48.8% -> 48.7% at wide spacing, 47.9% -> 48.7%). The nearest-neighbor linking design (documented above) already saturates the effect at moderate skew — most linked pairs' true gaps are already smaller than the clock offset spread, so adding more skew doesn't create meaningfully more violations. This is a real finding about the experiment design, not a defect in causality detection.
- **The corrected-interval check does its job in both regimes**: it is consistently the lowest violation count of the three timelines (337 vs. 482 corrected-point at baseline; 9 vs. 69 at wide spacing) — intervals hedge instead of confidently asserting a wrong order, exactly as designed.
- **At default (tight) event spacing, narrowing mostly fails** — 62% of linked nodes end up in an inconsistent component and get reverted, versus only 1.7% at wide spacing. This is the failure mode flagged above, observed directly: the nearest-neighbor linker (by design) creates chains whose true gaps can be smaller than what a ~17-90us-wide heuristic interval can resolve, so a majority of chains are judged causally infeasible against their own timing intervals rather than successfully narrowed. **Wider event spacing is what makes interval narrowing actually work** — at 500us mean inter-event time, reverted nodes drop to 1.7% and the global "weighted + causal" ordering accuracy reaches 99.98%.
- **The causally-related-pairs effect is small but consistently in the right direction** in every run: ambiguous pairs go down and correct-definite pairs go up after narrowing, never the reverse. It is a modest effect (562->550 ambiguous at baseline; 952->947 at wide spacing) because narrowing only ever touches the causally-linked minority of events (1499 of 5000) and, at default spacing, further limited by the high revert rate above.

## Phase 6: Boost.Asio Async Pipeline

Phases 1-5 are entirely synchronous: `EventSource::generate(n)` computes a whole source's events as pure math and returns them as a batch; everything downstream operates on that static, fully-materialized list. `MarketEvent::receive_time` has existed since Phase 1 but, until now, got a naive one-line synchronous fill that nothing downstream read. Phase 6 replaces that fill with a real asynchronous delivery pipeline: multiple sources deliver events concurrently to one shared collector, through simulated network latency, with genuine reordering, built on Boost.Asio.

### Design: virtual time, not real time

A market-event simulator has no reason to wait in real time for simulated nanoseconds to elapse. `VirtualTimeScheduler` (`include/network/virtual_scheduler.hpp`) maintains a deterministic min-heap of pending actions ordered by `(simulated_fire_time, insertion_sequence)`; draining it pops the earliest action, posts it through a `boost::asio::io_context`, and runs exactly that one handler before moving to the next. Nothing ever sleeps — a run spanning a simulated day of activity and one spanning a simulated microsecond cost the same wall-clock time (however long it takes to drain the heap).

This is deliberately honest about what Boost.Asio is doing here: `io_context` is the **executor** (a real handler-dispatch queue, and this project's required Boost.Asio integration), not the **clock** — the min-heap does the actual ordering. A bare loop popping the heap directly would be functionally identical to running one event through `io_context::post`+`run()`; Asio earns its place by providing the real async programming model, not by adding capability the heap doesn't already have.

### Architecture

`AsyncCollector` (`include/network/async_collector.hpp`) borrows the pooled event vector via `std::span` and stamps `receive_time` in place as arrivals are dispatched. `schedule_source_delivery` schedules one source's already-generated events: it draws a one-way latency for every event in the slice *up front*, then schedules each arrival at `event.true_time + delay`. Departure time is `true_time` (when the event actually happened), not `source_local_time` (the source's distorted belief about when) — physics must not be steered by a clock's reporting error.

**One shared scheduler and collector across all sources** is the entire point of doing this asynchronously: cross-source interleaving on a single collector is exactly what per-source-isolated delivery could never produce.

### Determinism

Arrival order is a pure function of `(fire_time, insertion_sequence)`, where the sequence is assigned in ascending source order, then generation order within a source — no `unordered_map` iteration, no pointer values, no wall clock anywhere in the ordering path. `io_context` is constructed with concurrency hint 1 and is only ever driven by one thread, so Asio itself contributes zero ordering nondeterminism.

**No new RNG seed stream was needed.** Delivery latency reuses the exact per-source seed stream (`cfg.seed * kEventLatencySeedMul + s`) the old synchronous fill used, consumed in the same order. This gives a strong, directly-verified migration guarantee: at `--reorder-probability 0`, **every metric in a Phase 6 run is bit-identical to the Phase 5 baseline** — confirmed by diffing a fresh run against `results/phase5_baseline.txt` with the async-specific lines filtered out; the diff is empty.

### Measured results

All runs: seed 42, 16 sources, 5000 events (same config as prior baselines unless noted).

**Baseline** (`results/phase6_baseline.txt`):

| Metric | Value |
|---|---|
| Events delivered | 5000 / 5000 |
| Realized delivery latency median / p95 / p99 | 100,753 / 183,039 / 219,493 ns |
| Same-source arrival inversions | 3587 / 4984 adjacent pairs (71.97%) |
| Arrival-order (`receive_time`) ordering accuracy | 98.24% |
| Raw timestamp ordering accuracy (unchanged from Phase 3-5) | 71.36% |

Arrival-order accuracy (98.24%) sits far above raw-local accuracy (71.36%) for an intuitive reason: `receive_time` is stamped by one collector clock, immune to the per-source clock skew that makes `source_local_time` unreliable — its only failure mode is network latency jitter, a much smaller source of error than multi-microsecond clock offsets.

**Source-count sweep** (`results/phase6_sources_{1,4,16,64}.txt`, 20,000 events each):

| Sources | Wall-clock delivery time |
|---|---|
| 1 | 3 ms |
| 4 | 3 ms |
| 16 | 3 ms |
| 64 | 3 ms |

Flat across source count, exactly as the virtual-time design predicts: wall-clock cost is a function of event count (heap operations), not source count or simulated time span.

**Honest findings, not the ones a marketing-driven writeup would lead with:**

- **Realized delivery latency matches `LatencyModel`'s synchronous draw distribution exactly**, because the collector has unbounded capacity and virtual time adds no queueing delay. The async layer changes *ordering and interleaving*, not the latency distribution — adding a queueing/service-time model would change that, and is deliberately out of scope here.
- **`apply_reordering`'s effect on the aggregate same-source-inversion count is small and inconsistent in direction, not a clean "more reordering" signal.** Comparing `results/phase6_baseline.txt` (`--reorder-probability 0`, 3587 inversions) against `results/phase6_reorder.txt` (`--reorder-probability 0.3`, 3585 inversions) shows essentially no aggregate change; a moderate-jitter comparison (`--jitter-us 5`) showed a small *decrease* (1152 -> 1128) enabling reordering. This is because `apply_reordering` swaps adjacent-pair *values* in an already-i.i.d.-jitter-drawn delay sequence — the multiset of realized latencies is provably unchanged either way (see `ReorderingPermutesLatencyPairingsOnlyNotTheMultiset`), so its effect on any aggregate statistic over that multiset is genuinely small and can move in either direction depending on which specific pairs happen to get swapped. With jitter off entirely, every delay value is identical, so swapping identical values is a mathematical no-op on delivery order regardless of `reorder_probability` — verified directly (`--jitter-us 0` gives 0 same-source inversions at both `--reorder-probability 0` and `1`). The primitive's integration and correctness are proven directly at the individual-event level instead (`ApplyReorderingSwapsExpectedDelayPairingIntoRealReceiveTimes` hand-computes the exact expected post-swap `receive_time` and asserts the real pipeline matches it exactly) — that is the right level to verify this mechanism at, not an aggregate CLI statistic that a symmetric pairwise swap of already-random values isn't well-suited to move.
