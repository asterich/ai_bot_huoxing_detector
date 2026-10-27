module;
#include <cstdint>
#include <string>

export module huoxing.forward_types;

export namespace huoxing {
// The status describes top-level evidence, never equality of hidden message content.
struct ForwardResult {
    std::string status;
    bool replayed = false;
    std::string first_message_id;
    std::string first_seen;
};

struct ForwardMatch {
    std::int64_t occurrence_id = 0;
    std::string message_id;
    std::string received_at;
};
} // namespace huoxing
