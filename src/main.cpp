#include "simulation/event_source.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <string>
#include <string_view>
#include <vector>

namespace {

struct SimConfig {
    int         num_sources       = 4;
    int         total_events      = 1000;
    std::uint64_t seed            = 42;
    double      drift_ppm         = 100.0;
    double      max_offset_us     = 500.0;   // each source offset varies up to ±this
    double      noise_std_ns      = 50.0;
    double      inter_event_us    = 1000.0;  // mean inter-event time per source in μs
    std::string output_path;                 // empty = stdout only
};

void print_usage(const char* prog) {
    std::cerr << "Usage: " << prog << " [options]\n"
              << "  --sources N           number of event sources      (default 4)\n"
              << "  --events N            total events to generate     (default 1000)\n"
              << "  --seed N              RNG seed                     (default 42)\n"
              << "  --drift-ppm F         clock drift in PPM           (default 100.0)\n"
              << "  --max-offset-us F     max abs clock offset in us   (default 500.0)\n"
              << "  --noise-std-ns F      timestamp noise std-dev ns   (default 50.0)\n"
              << "  --inter-event-us F    mean inter-event time in us  (default 1000.0)\n"
              << "  --output PATH         write events CSV to PATH\n";
}

SimConfig parse_args(int argc, char** argv) {
    SimConfig cfg;
    for (int i = 1; i < argc; ++i) {
        std::string_view key{argv[i]};
        auto next = [&]() -> std::string_view {
            if (i + 1 >= argc) {
                std::cerr << "Missing value for " << key << "\n";
                std::exit(1);
            }
            return argv[++i];
        };
        if      (key == "--sources")       cfg.num_sources     = std::stoi(std::string(next()));
        else if (key == "--events")        cfg.total_events    = std::stoi(std::string(next()));
        else if (key == "--seed")          cfg.seed            = std::stoull(std::string(next()));
        else if (key == "--drift-ppm")     cfg.drift_ppm       = std::stod(std::string(next()));
        else if (key == "--max-offset-us") cfg.max_offset_us   = std::stod(std::string(next()));
        else if (key == "--noise-std-ns")  cfg.noise_std_ns    = std::stod(std::string(next()));
        else if (key == "--inter-event-us")cfg.inter_event_us  = std::stod(std::string(next()));
        else if (key == "--output")        cfg.output_path     = std::string(next());
        else if (key == "--help" || key == "-h") { print_usage(argv[0]); std::exit(0); }
        else { std::cerr << "Unknown option: " << key << "\n"; print_usage(argv[0]); std::exit(1); }
    }
    if (cfg.num_sources <= 0 || cfg.total_events <= 0) {
        std::cerr << "--sources and --events must be positive\n";
        std::exit(1);
    }
    return cfg;
}

// Fraction of event pairs (i<j) where the ordering by local_time matches
// the ordering by true_time. Pairs with equal true_time are excluded.
double pairwise_ordering_accuracy(const std::vector<chronos::MarketEvent>& events,
                                  bool use_local_time) {
    std::size_t concordant = 0;
    std::size_t total      = 0;

    for (std::size_t i = 0; i < events.size(); ++i) {
        for (std::size_t j = i + 1; j < events.size(); ++j) {
            const auto& a = events[i];
            const auto& b = events[j];

            if (a.true_time == b.true_time) continue; // skip simultaneous events

            const bool true_order  = a.true_time < b.true_time;
            const bool cmp_order   = use_local_time
                ? a.source_local_time < b.source_local_time
                : a.true_time         < b.true_time; // baseline sanity check

            if (true_order == cmp_order) ++concordant;
            ++total;
        }
    }
    return total == 0 ? 1.0 : static_cast<double>(concordant) / static_cast<double>(total);
}

const char* event_type_name(chronos::EventType t) {
    switch (t) {
        case chronos::EventType::OrderSubmitted:   return "OrderSubmitted";
        case chronos::EventType::OrderAcknowledged:return "OrderAcknowledged";
        case chronos::EventType::OrderExecuted:    return "OrderExecuted";
        case chronos::EventType::QuoteUpdated:     return "QuoteUpdated";
        case chronos::EventType::TradePublished:   return "TradePublished";
        case chronos::EventType::Heartbeat:        return "Heartbeat";
    }
    return "Unknown";
}

} // namespace

int main(int argc, char** argv) {
    const SimConfig cfg = parse_args(argc, argv);

    const int events_per_source = cfg.total_events / cfg.num_sources;
    const int remainder         = cfg.total_events % cfg.num_sources;

    // Simulation reference time: Unix epoch 2026-01-01 00:00:00 in nanoseconds.
    // Arbitrary but fixed so results are reproducible.
    constexpr chronos::NsTimestamp kReferenceTimeNs = 1'767'225'600LL * 1'000'000'000LL;

    std::vector<chronos::MarketEvent> all_events;
    all_events.reserve(cfg.total_events);

    for (int s = 0; s < cfg.num_sources; ++s) {
        // Spread offsets symmetrically: source 0 is most negative, last is most positive.
        // Avoids all sources having the same bias direction.
        const double offset_fraction = cfg.num_sources > 1
            ? (static_cast<double>(s) / (cfg.num_sources - 1) * 2.0 - 1.0)
            : 0.0;
        const chronos::NsDuration offset_ns =
            static_cast<chronos::NsDuration>(offset_fraction * cfg.max_offset_us * 1000.0);

        // Alternate drift sign so we get both fast and slow clocks.
        const double drift = (s % 2 == 0 ? 1.0 : -1.0) * cfg.drift_ppm;

        // Derive per-source seeds deterministically from the global seed.
        const std::uint64_t clock_seed = cfg.seed * 6364136223846793005ULL + static_cast<std::uint64_t>(s);
        const std::uint64_t src_seed   = cfg.seed * 2862933555777941757ULL + static_cast<std::uint64_t>(s);

        chronos::ClockConfig clock_cfg{
            .offset_ns    = offset_ns,
            .drift_ppm    = drift,
            .noise_std_ns = cfg.noise_std_ns,
            .seed         = clock_seed,
        };
        chronos::EventSourceConfig src_cfg{
            .start_time_ns       = kReferenceTimeNs,
            .mean_inter_event_ns = static_cast<chronos::NsDuration>(cfg.inter_event_us * 1'000.0),
            .seed                = src_seed,
        };

        chronos::EventSource source(
            static_cast<chronos::SourceId>(s),
            clock_cfg, src_cfg,
            kReferenceTimeNs);

        const int count = events_per_source + (s < remainder ? 1 : 0);
        auto evts = source.generate(static_cast<std::size_t>(count));
        for (auto& e : evts) all_events.push_back(std::move(e));
    }

    // Compute accuracy before any sorting (pool order is per-source, interleaved).
    // Sort by source_local_time to get the "raw" timeline.
    std::vector<chronos::MarketEvent> raw_sorted = all_events;
    std::sort(raw_sorted.begin(), raw_sorted.end(),
              [](const auto& a, const auto& b) {
                  return a.source_local_time < b.source_local_time;
              });

    const double raw_accuracy = pairwise_ordering_accuracy(raw_sorted, /*use_local_time=*/true);

    // Ground-truth order (for reference).
    std::vector<chronos::MarketEvent> true_sorted = all_events;
    std::sort(true_sorted.begin(), true_sorted.end(),
              [](const auto& a, const auto& b) {
                  return a.true_time < b.true_time;
              });

    // Print report.
    std::cout << "\nChronos Phase 1 — Simulation Report\n";
    std::cout << "-------------------------------------\n";
    std::cout << "Sources:           " << cfg.num_sources   << "\n";
    std::cout << "Events generated:  " << all_events.size() << "\n";
    std::cout << "Seed:              " << cfg.seed          << "\n";
    std::cout << "Drift (PPM):       ±" << cfg.drift_ppm    << "\n";
    std::cout << "Max offset (us):   ±" << cfg.max_offset_us<< "\n";
    std::cout << "Noise std (ns):    " << cfg.noise_std_ns  << "\n";
    std::cout << "\n";
    std::cout << std::fixed << std::setprecision(2);
    std::cout << "Raw timestamp ordering accuracy:  " << raw_accuracy * 100.0 << "%\n";
    std::cout << "\n";
    std::cout << "First 10 events by true order vs raw order:\n";
    std::cout << std::setw(6) << "Rank"
              << std::setw(9) << "SrcID"
              << std::setw(20) << "true_time (ns)"
              << std::setw(20) << "local_time (ns)"
              << std::setw(14) << "offset (ns)"
              << "\n";
    for (std::size_t i = 0; i < std::min<std::size_t>(10, true_sorted.size()); ++i) {
        const auto& e = true_sorted[i];
        std::cout << std::setw(6)  << i
                  << std::setw(9)  << static_cast<std::uint32_t>(e.source_id)
                  << std::setw(20) << e.true_time
                  << std::setw(20) << e.source_local_time
                  << std::setw(14) << (e.source_local_time - e.true_time)
                  << "\n";
    }

    // Optional CSV output.
    if (!cfg.output_path.empty()) {
        std::ofstream out(cfg.output_path);
        if (!out) {
            std::cerr << "Could not open output file: " << cfg.output_path << "\n";
            return 1;
        }
        out << "event_id,source_id,event_type,true_time_ns,local_time_ns,offset_ns\n";
        for (const auto& e : true_sorted) {
            out << static_cast<std::uint64_t>(e.event_id)   << ","
                << static_cast<std::uint32_t>(e.source_id)  << ","
                << event_type_name(e.event_type)             << ","
                << e.true_time                               << ","
                << e.source_local_time                       << ","
                << (e.source_local_time - e.true_time)       << "\n";
        }
        std::cout << "\nEvents written to: " << cfg.output_path << "\n";
    }

    return 0;
}
