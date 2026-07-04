#pragma once

#include <cstdint>

namespace chronos {

// Opaque strong-type wrappers using enum class to prevent implicit mixing.
enum class SourceId : std::uint32_t {};
enum class EventId  : std::uint64_t {};

enum class EventType : std::uint8_t {
    OrderSubmitted,
    OrderAcknowledged,
    OrderExecuted,
    QuoteUpdated,
    TradePublished,
    Heartbeat,
};

// All timestamps and durations are integer nanoseconds.
// Signed to allow negative offsets and differences.
using NsTimestamp = std::int64_t;
using NsDuration  = std::int64_t;

} // namespace chronos
