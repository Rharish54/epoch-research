#include "metrics/percentile.hpp"

#include <algorithm>

namespace chronos {

double percentile(std::vector<double> values, double p) {
    if (values.empty()) return 0.0;
    std::sort(values.begin(), values.end());

    const double clamped = std::min(std::max(p, 0.0), 1.0);
    auto index = static_cast<std::size_t>(clamped * static_cast<double>(values.size() - 1) + 0.5);
    index = std::min(index, values.size() - 1);
    return values[index];
}

} // namespace chronos
