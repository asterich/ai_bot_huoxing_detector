export module huoxing.forward;
import std;
import huoxing.hashing;
import huoxing.database;
import huoxing.forward_database;
import huoxing.forward_types;

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

    ForwardResult ingest(
        std::string_view scope,
        std::string_view message_id,
        int part,
        std::string_view resource_namespace,
        std::string_view resource_id,
        std::string_view visible_card = ""
    ) const {
        if (scope.empty() || message_id.empty() || part < 0 || resource_namespace.empty()) {
            throw std::invalid_argument("scope/message/namespace required; part >=0");
        }
        if (resource_id.size() > 4096 || visible_card.size() > 65536) {
            throw std::invalid_argument("Top-level forward metadata too large");
        }

        const auto card_digest =
            visible_card.empty() ? std::string{} : hashing::sha256_text(visible_card);
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
        repository.insert(
            scope,
            message_id,
            part,
            resource_namespace,
            resource_id,
            card_digest,
            result.status,
            match ? match->occurrence_id : 0
        );
        repository.commit();
        return result;
    }

    void delete_scope(std::string_view scope) const {
        storage::ForwardRepository repository(path_);
        repository.delete_scope(scope);
        repository.commit();
    }

  private:

    std::string path_;
};
} // namespace huoxing
