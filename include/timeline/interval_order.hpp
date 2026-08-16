#pragma once

#include "timeline/corrected_event.hpp"

namespace chronos {

// The result of comparing two CorrectedEvents' uncertainty intervals.
enum class TemporalOrder {
    Before,    // a definitely happened before b
    After,     // a definitely happened after b
    Ambiguous, // intervals overlap -- a total order is not defensible
};

// a.upper_bound < b.lower_bound  -> Before
// b.upper_bound < a.lower_bound  -> After
// otherwise (intervals overlap, touch, or invert)             -> Ambiguous
//
// Touching bounds (a.upper_bound == b.lower_bound) are classified Ambiguous,
// not Before: an uncertainty interval represents "could plausibly be
// anywhere in this range," so equal endpoints are a tie, not a strict
// separation. This project deliberately does not force a total order when
// evidence doesn't support one -- see CLAUDE.md's confidence-interval
// ordering rule.
inline TemporalOrder classify_order(const CorrectedEvent& a, const CorrectedEvent& b) {
    if (a.upper_bound < b.lower_bound) return TemporalOrder::Before;
    if (b.upper_bound < a.lower_bound) return TemporalOrder::After;
    return TemporalOrder::Ambiguous;
}

} // namespace chronos
