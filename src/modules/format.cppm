module;
#include "std_compat.hpp"
#include <fmt/format.h>

export module huoxing.format;
import std;

// Export the original fmt entities, keeping their library ABI and implementation.
export namespace fmt {
using ::fmt::format;
using ::fmt::format_to;
using ::fmt::memory_buffer;
using ::fmt::println;
using ::fmt::to_string;
} // namespace fmt

export namespace huoxing::formatting {

// std modules do not export C stream macros. Keep that boundary inside the fmt adapter.
void report_error(std::string_view message) {
    fmt::println(stderr, "{}", message);
}

} // namespace huoxing::formatting
