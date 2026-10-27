module;
#include <array>
#include <iomanip>
#include <openssl/evp.h>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>

export module huoxing.forward;
import huoxing.database;
import huoxing.forward_database;
import huoxing.forward_types;

namespace {
std::string hash_visible_card(const std::string& visible_card) {
    if (visible_card.empty()) {
        return {};
    }
    std::array<unsigned char, EVP_MAX_MD_SIZE> bytes{};
    unsigned int size = 0;
    if (EVP_Digest(visible_card.data(), visible_card.size(), bytes.data(), &size, EVP_sha256(),
                   nullptr) != 1) {
        throw std::runtime_error("Card hash failed");
    }
    std::ostringstream output;
    for (unsigned int index = 0; index < size; ++index) {
        output << std::hex << std::setw(2) << std::setfill('0') << int(bytes[index]);
    }
    return output.str();
}
} // namespace

export namespace huoxing {

// Accepts only an opaque top-level resource identifier and allowlisted visible card text.
class ForwardDetector {
  public:
    explicit ForwardDetector(std::string path) : path_(std::move(path)) {
        if (path_.empty() || path_ == ":memory:") {
            throw std::invalid_argument("Need a file database");
        }
        storage::ImageRepository::initialize(path_);
    }

    ForwardResult ingest(const std::string& scope, const std::string& message_id, int part,
                         const std::string& resource_namespace, const std::string& resource_id,
                         const std::string& visible_card = "") const {
        if (scope.empty() || message_id.empty() || part < 0 || resource_namespace.empty()) {
            throw std::invalid_argument("scope/message/namespace required; part >=0");
        }
        if (resource_id.size() > 4096 || visible_card.size() > 65536) {
            throw std::invalid_argument("Top-level forward metadata too large");
        }

        const auto card_digest = hash_visible_card(visible_card);
        storage::ForwardRepository repository(path_);
        if (auto replay = repository.find_replay(scope, message_id, part)) {
            return *replay;
        }

        ForwardResult result;
        std::optional<ForwardMatch> match;
        if (!resource_id.empty()) {
            match = repository.find_resource(scope, resource_namespace, resource_id);
            if (match) {
                result.status = "same_resource";
            }
        }
        if (!match && !card_digest.empty()) {
            match = repository.find_card(scope, card_digest);
            if (match) {
                result.status = "possible_same_card";
            }
        }
        if (match) {
            result.first_message_id = match->message_id;
            result.first_seen = match->received_at;
        } else {
            result.status = resource_id.empty() ? "insufficient_evidence" : "new_resource";
        }

        // Empty evidence cannot establish identity; avoid creating an all-empty shared key.
        if (resource_id.empty() && card_digest.empty()) {
            return result;
        }
        repository.insert(scope, message_id, part, resource_namespace, resource_id, card_digest,
                          result.status, match ? match->occurrence_id : 0);
        repository.commit();
        return result;
    }

    void delete_scope(const std::string& scope) const {
        storage::ForwardRepository repository(path_);
        repository.delete_scope(scope);
        repository.commit();
    }

  private:
    std::string path_;
};
} // namespace huoxing
