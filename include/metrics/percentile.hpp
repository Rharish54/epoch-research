#pragma once

#include <vector>

namespace chronos {

// Nearest-rank percentile of `values` (0.0 <= p <= 1.0). Sorts a copy.
// Returns 0.0 for an empty input.
double percentile(std::vector<double> values, double p);

} // namespace chronos
