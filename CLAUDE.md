# CLAUDE.md

## Project Name

**Chronos: Distributed Market Event Timeline Reconstructor**

Chronos is a C++20 systems project that simulates distributed market-data sources with imperfect clocks and unreliable network behavior, then reconstructs the most defensible global ordering of events.

The goal is to build a project that demonstrates strong C++ engineering, distributed systems reasoning, performance awareness, statistical estimation, reliability, and correctness. This project is intended to be compelling for highly selective trading-firm software engineering roles.

---

## Core Pitch

Distributed trading systems receive events from many machines, feeds, and services. Each source has its own clock. Those clocks can be ahead, behind, drifting, noisy, or poorly synchronized. Network delivery also introduces latency, jitter, reordering, and occasional spikes.

If events are sorted only by their reported timestamps, the resulting timeline can be wrong or even impossible.

Chronos simulates this problem and reconstructs a better global timeline by combining:

- Clock offset estimation
- Clock drift estimation
- Network-latency-aware outlier rejection
- Timestamp uncertainty intervals
- Causal dependency constraints
- Performance benchmarking

The system should distinguish between:

1. Events that are definitely ordered
2. Events that are temporally ambiguous
3. Events that violate known causal constraints

---

## Non-Negotiable Project Principles

### 1. Correctness before cleverness

Do not optimize until the baseline system is correct and tested.

A slower correct timeline reconstructor is better than a fast inaccurate one.

### 2. No fake metrics

Never hardcode or invent benchmark results.

All performance, latency, throughput, and accuracy claims must be produced by reproducible benchmark commands.

### 3. Determinism matters

The simulator must support deterministic runs using explicit random seeds.

Every experiment should be reproducible.

### 4. Make tradeoffs explicit

When choosing between two approaches, document the decision in comments or the README.

Examples:

- Why use a single collector?
- Why use weighted regression?
- Why mark overlapping intervals as ambiguous instead of forcing a total order?
- Why reject high-RTT synchronization samples?

### 5. Keep the first version buildable

Do not over-engineer the initial version. Build in phases.

The first complete version should run locally, generate events, distort timestamps, correct timestamps, and report accuracy.

---

## Target Technology Stack

Use:

- C++20
- CMake
- Boost.Asio for asynchronous simulation/network/event scheduling
- GoogleTest for correctness tests
- Google Benchmark for performance benchmarks
- Python for plotting and analysis scripts
- Linux/macOS compatible command-line workflow

Optional later additions:

- `perf` on Linux
- `valgrind` or `heaptrack`
- `clang-tidy`
- `clang-format`

Do not introduce unnecessary frameworks.

---

## Intended Repository Structure

Use this structure unless there is a strong reason to change it.

```text
chronos/
├── CLAUDE.md
├── README.md
├── CMakeLists.txt
├── config/
│   └── simulation.json
├── include/
│   ├── core/
│   │   ├── types.hpp
│   │   └── time_utils.hpp
│   ├── clock/
│   │   ├── clock_model.hpp
│   │   ├── sync_sample.hpp
│   │   ├── offset_estimator.hpp
│   │   └── drift_estimator.hpp
│   ├── simulation/
│   │   ├── market_event.hpp
│   │   ├── event_source.hpp
│   │   └── simulator.hpp
│   ├── network/
│   │   ├── latency_model.hpp
│   │   └── async_collector.hpp
│   ├── timeline/
│   │   ├── corrected_event.hpp
│   │   ├── confidence_interval.hpp
│   │   ├── causal_graph.hpp
│   │   └── timeline_reconstructor.hpp
│   └── metrics/
│       ├── accuracy_report.hpp
│       └── latency_stats.hpp
├── src/
│   ├── clock/
│   ├── simulation/
│   ├── network/
│   ├── timeline/
│   ├── metrics/
│   └── main.cpp
├── tests/
│   ├── clock_model_test.cpp
│   ├── offset_estimator_test.cpp
│   ├── drift_estimator_test.cpp
│   ├── causal_graph_test.cpp
│   └── reconstruction_test.cpp
├── benchmarks/
│   ├── estimator_benchmark.cpp
│   └── reconstruction_benchmark.cpp
├── scripts/
│   ├── analyze_results.py
│   └── plot_results.py
└── results/
    └── .gitkeep
```

---

## Build Commands

Prefer a standard CMake workflow.

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

Run the main simulator:

```bash
./build/chronos --config config/simulation.json
```

Run tests:

```bash
ctest --test-dir build --output-on-failure
```

Run benchmarks:

```bash
./build/estimator_benchmark
./build/reconstruction_benchmark
```

If GoogleTest or Google Benchmark are not available locally, configure CMake to fetch them with `FetchContent`.

---

## Coding Standards

Use modern C++20.

Prefer:

- `std::chrono` for time representation
- Strong types for source IDs, event IDs, and timestamps when practical
- `std::vector` for contiguous event storage
- `std::optional` for absent estimates
- `std::span` where useful
- `const` correctness
- Value semantics for small structs
- Clear ownership with smart pointers only when necessary

Avoid:

- Global mutable state
- Raw owning pointers
- Floating-point timestamps when integer nanoseconds/microseconds are sufficient
- Premature multithreading
- Hidden randomness
- Hardcoded benchmark outputs

Use fixed-width integer types where appropriate:

```cpp
std::int64_t
std::uint64_t
std::size_t
```

Represent timestamps internally as integer nanoseconds or microseconds, not strings.

---

## Core Data Model

### MarketEvent

A market event should include:

- Event ID
- Source ID
- Event type
- Ground-truth timestamp
- Source-local timestamp
- Receive timestamp at collector
- Optional parent/dependency event IDs

Suggested event types:

```cpp
enum class EventType {
    OrderSubmitted,
    OrderAcknowledged,
    OrderExecuted,
    QuoteUpdated,
    TradePublished,
    Heartbeat
};
```

Ground truth is used only for evaluation, not reconstruction.

### ClockModel

Each simulated source has a local clock:

```text
local_time = true_time + offset + drift * elapsed_time + noise
```

The model should support:

- Initial offset
- Drift in parts per million
- Random timestamp noise
- Deterministic seed

### LatencyModel

The network model should support:

- Base latency
- Jitter
- Asymmetric inbound/outbound latency
- Latency spikes
- Optional packet reordering
- Deterministic seed

---

## Synchronization Model

Implement a four-timestamp synchronization exchange:

```text
t1: collector sends sync request
t2: source receives sync request
t3: source sends sync response
t4: collector receives sync response
```

Estimate offset:

```text
offset = ((t2 - t1) + (t3 - t4)) / 2
```

Estimate round-trip delay:

```text
delay = (t4 - t1) - (t3 - t2)
```

Store sync samples:

```cpp
struct SyncSample {
    SourceId source_id;
    TimePoint collector_send_time;
    TimePoint source_receive_time;
    TimePoint source_send_time;
    TimePoint collector_receive_time;
    Duration estimated_offset;
    Duration round_trip_delay;
};
```

---

## Estimation Requirements

### Baseline estimator

Start with the most recent valid offset estimate.

### Weighted drift estimator

Then implement a sliding-window linear drift estimator.

The model:

```text
estimated_offset(t) = intercept + slope * elapsed_time
```

The estimator should:

- Maintain recent sync samples per source
- Weight samples with lower RTT more heavily
- Estimate offset at any target event time
- Reject or downweight outliers

### Outlier rejection

Implement simple and explainable rejection.

Acceptable methods:

- Reject samples above a configured RTT percentile
- Reject samples with RTT much larger than the rolling median
- Reject samples whose residual error is too large

Do not make this statistically overcomplicated. The README should explain the approach clearly.

---

## Confidence Intervals

Each corrected event should have:

- Estimated corrected global timestamp
- Lower bound
- Upper bound
- Uncertainty value

Example:

```cpp
struct CorrectedEvent {
    MarketEvent original;
    TimePoint corrected_time;
    TimePoint lower_bound;
    TimePoint upper_bound;
};
```

Ordering rule:

```text
A definitely happened before B if A.upper_bound < B.lower_bound
B definitely happened before A if B.upper_bound < A.lower_bound
Otherwise, A and B are temporally ambiguous
```

Do not force a total ordering when uncertainty intervals overlap.

This is one of the project's main differentiators.

---

## Causal Graph

Some events have known dependencies.

Examples:

```text
OrderSubmitted -> OrderAcknowledged
OrderAcknowledged -> OrderExecuted
OrderExecuted -> TradePublished
```

Represent dependencies as a directed graph.

The timeline reconstructor should identify:

- Valid causal chains
- Causality violations
- Cases where timestamps suggest an impossible ordering
- Cases where corrected timestamps are ambiguous but causal constraints still imply order

Use topological sorting or graph traversal as needed.

---

## Accuracy Metrics

Because the simulator knows ground truth, report:

- Raw timestamp ordering accuracy
- Corrected timestamp ordering accuracy
- Accuracy improvement
- Number of ambiguous event pairs
- Number of correctly classified ambiguous pairs
- Causal violations before reconstruction
- Causal violations after reconstruction
- Median clock-offset estimation error
- p95 clock-offset estimation error
- p99 clock-offset estimation error

Pairwise ordering accuracy can be expensive for very large event counts. For large runs, sample event pairs deterministically.

---

## Performance Metrics

Report:

- Events processed per second
- p50 processing latency
- p95 processing latency
- p99 processing latency
- Total reconstruction time
- Memory usage if practical

Benchmark separately:

1. Clock-offset estimation
2. Timeline reconstruction
3. End-to-end simulation and reconstruction

All benchmark output should be saved to `results/`.

---

## Implementation Phases

### Phase 1: Minimal deterministic simulator

Build:

- `MarketEvent`
- `ClockModel`
- `EventSource`
- Deterministic event generation
- Ground-truth timestamps
- Distorted source-local timestamps

Goal:

Show that sorting by raw local timestamps creates incorrect event ordering.

Deliverables:

- Unit tests for clock conversion
- A simple CLI run
- Accuracy report for raw timestamp ordering

### Phase 2: Basic synchronization

Build:

- Four-timestamp sync samples
- Basic offset estimator
- Timestamp correction
- Corrected ordering accuracy report

Goal:

Show that synchronization improves event ordering accuracy.

Deliverables:

- Unit tests for offset estimation
- Before/after accuracy comparison

### Phase 3: Drift estimation and outlier rejection

Build:

- Sliding-window estimator
- Weighted linear drift model
- RTT-based outlier rejection
- Offset error metrics

Goal:

Handle clocks that change over time and bad latency samples.

Deliverables:

- Unit tests for drift estimation
- Experiment comparing naive mean vs weighted drift estimator
- Accuracy report under latency spikes

### Phase 4: Confidence-aware timeline reconstruction

Build:

- Corrected timestamp intervals
- Definite/ambiguous ordering classifier
- Ambiguity metrics

Goal:

Avoid pretending to know event order when corrected timestamps overlap.

Deliverables:

- Tests for interval comparison
- Report showing ambiguous event classification

### Phase 5: Causal graph

Build:

- Event dependency graph
- Causality violation detection
- Timeline reconstruction using causal constraints

Goal:

Detect impossible timelines and combine timing evidence with protocol-level causality.

Deliverables:

- Graph tests
- Example where raw timestamps create impossible causality
- Corrected timeline output

### Phase 6: Boost.Asio async pipeline

Build:

- Asynchronous event source scheduling
- Async collector
- Simulated network latency and reordering
- Same deterministic seed behavior

Goal:

Demonstrate event-driven systems engineering in C++.

Deliverables:

- End-to-end async simulation
- Benchmark under several source counts

### Phase 7: Benchmarks, README, and final polish

Build:

- Google Benchmark suite
- Python analysis scripts
- Results tables
- README explanation
- Architecture diagram
- Reproducible commands

Goal:

Make the project portfolio-ready and resume-ready.

Deliverables:

- Clear README
- Verified numbers
- Plots or CSVs
- Final resume bullets

---

## Suggested CLI

The final executable should support flags like:

```bash
./build/chronos \
  --sources 8 \
  --events 1000000 \
  --seed 42 \
  --drift-ppm 50 \
  --base-latency-us 100 \
  --jitter-us 50 \
  --spike-probability 0.01 \
  --output results/run_seed_42.csv
```

Keep CLI simple at first. Add config-file support later.

---

## Example Output

A normal run should print something like:

```text
Chronos Simulation Report
-------------------------
Sources: 8
Events: 1,000,000
Seed: 42

Raw timestamp ordering accuracy: 72.4%
Corrected ordering accuracy: 97.8%
Ambiguous event pairs: 1.9%
Causal violations before correction: 14,283
Causal violations after correction: 0

Median offset error: 8.4 us
p95 offset error: 41.2 us
p99 offset error: 87.6 us

Reconstruction throughput: 742,000 events/sec
p50 processing latency: 4.1 us
p95 processing latency: 13.8 us
p99 processing latency: 22.5 us
```

Important: these are example placeholders. Do not print these exact values unless measured.

---

## Testing Strategy

Use GoogleTest.

Required tests:

### Clock model tests

- Offset is applied correctly
- Drift accumulates correctly
- Noise is deterministic with fixed seed

### Offset estimator tests

- Offset estimate is exact under zero network asymmetry
- High-RTT samples are rejected or downweighted
- Estimator improves over raw timestamps

### Drift estimator tests

- Recovers positive drift
- Recovers negative drift
- Handles noisy samples
- Sliding window updates correctly

### Confidence interval tests

- Non-overlapping intervals are ordered correctly
- Overlapping intervals are ambiguous
- Edge cases are handled consistently

### Causal graph tests

- Valid chains pass
- Impossible orderings are detected
- Topological constraints are respected

### Reconstruction tests

- Raw ordering performs worse than corrected ordering under clock skew
- Corrected timeline has fewer causality violations
- Deterministic seed produces repeatable results

---

## Benchmarking Rules

Use Release builds for benchmarks.

Do not benchmark Debug builds.

Use warm-up iterations when possible.

Record:

- Hardware
- OS
- Compiler
- Compiler flags
- Event count
- Source count
- Seed
- Latency configuration
- Drift configuration

Benchmark output should be reproducible from README commands.

---

## Resume-Safe Final Bullet Template

Only use this after implementation and benchmarks are complete.

```latex
\resumeProjectHeading
    {
        \textbf{Chronos: Distributed Market Event Timeline Reconstructor}
        $|$
        \emph{C++20, Boost.Asio, CMake, GoogleTest, Google Benchmark, Python}
    }
    {July 2026 -- Present}

\resumeProjectItemListStart

    \resumeItem{
        Built a distributed C++20 simulator modeling clock offset, drift,
        asymmetric network latency, jitter, and packet reordering across
        [X] concurrent market-data sources and [Y]+ million generated events
    }

    \resumeItem{
        Developed an asynchronous synchronization and causal reconstruction
        engine using weighted drift estimation, confidence intervals, outlier
        rejection, and dependency graphs, improving event-ordering accuracy
        from [X]\% to [Y]\% while processing [Z]+ events per second
    }

\resumeProjectItemListEnd
```

Do not replace placeholders until the benchmark suite produces the numbers.

---

## README Requirements

The README should include:

1. One-paragraph project explanation
2. Why naive timestamp sorting fails
3. System architecture diagram
4. Clock model explanation
5. Synchronization method
6. Drift estimation method
7. Confidence interval logic
8. Causal dependency graph explanation
9. Benchmark results
10. How to build and run
11. What was difficult
12. Future improvements

Include at least one example showing an impossible timeline before reconstruction.

---

## Future Improvements

Only after the main system works:

- Real UDP sockets instead of simulated async messages
- Multiple collector nodes
- More advanced robust regression
- Kalman filtering
- PTP/NTP comparison mode
- Binary event serialization
- Lock-free queues
- Flamegraphs and deeper profiling
- Dashboard visualization

Do not start with these.

---

## Important Guidance for Claude Code

When helping build this project:

1. Prefer small, working commits over large rewrites.
2. Keep code simple and readable.
3. Add tests whenever a new module is introduced.
4. Avoid inventing benchmark numbers.
5. Keep all random behavior seedable.
6. Document assumptions.
7. Preserve the core project identity: distributed market-event timeline reconstruction under clock skew and network uncertainty.
8. If a requested change makes the project too large, propose a smaller version that still supports the resume story.
9. Do not turn this into a generic order book project.
10. Do not remove the confidence-aware or causal-reconstruction elements; those are the differentiators.

---

## First Task to Start From Scratch

Start by creating the project skeleton:

- `CMakeLists.txt`
- `include/core/types.hpp`
- `include/simulation/market_event.hpp`
- `include/clock/clock_model.hpp`
- `src/main.cpp`
- `tests/clock_model_test.cpp`
- Basic GoogleTest integration

Then implement:

1. A deterministic `ClockModel`
2. A simple event generator
3. A CLI demo showing true timestamps versus distorted source-local timestamps

Do not implement Boost.Asio until the deterministic simulator and clock model tests are passing.
