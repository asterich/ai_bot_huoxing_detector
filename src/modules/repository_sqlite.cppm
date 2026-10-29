export module huoxing.repository_sqlite;
import std;
import huoxing.types;
import huoxing.forward_types;
import huoxing.messages;
import huoxing.repository;
import huoxing.sqlite;
import huoxing.database;
import huoxing.forward_database;

namespace {

using huoxing::storage::Statement;

// The concrete session adapts existing storage tables to one logical repository.
// All identity-index and occurrence writes share this connection and transaction.
class SqliteSession final : public huoxing::RepositorySession {
  public:

    SqliteSession(std::string_view path, int capacity)
        : connection_(path), transaction_(connection_), images_(connection_),
          forwards_(connection_), capacity_(capacity) {}

    std::optional<huoxing::Detection> replay(const huoxing::PreparedMessage& message) override {
        const auto& context = message.context;
        huoxing::Detection result;
        result.replayed = true;

        if (message.fingerprint.kind == huoxing::MessageKind::Image) {
            auto cached = images_.find_replay(context.scope, context.message_id, context.part);
            if (!cached) {
                return {};
            }

            Statement identity(connection_, "SELECT digest FROM images WHERE id=?");
            identity.bind(1, cached->image_id);
            if (!identity.step()) {
                throw std::runtime_error("Missing replay image");
            }
            result.fingerprint = huoxing::make_fingerprint(
                huoxing::MessageKind::Image,
                huoxing::HashBasis::DecodedPixels,
                {identity.text(0)}
            );
            result.storage = {huoxing::MessageKind::Image, cached->image_id};
            result.status = cached->duplicate ? huoxing::DetectionStatus::Duplicate
                                              : huoxing::DetectionStatus::New;
            result.evidence = cached->evidence;
            if (cached->duplicate) {
                result.first = huoxing::FirstOccurrence{
                    cached->first_message_id,
                    cached->first_sender_id,
                    cached->first_seen
                };
            }
        } else {
            auto cached = forwards_.find_replay(context.scope, context.message_id, context.part);
            if (!cached) {
                return {};
            }

            Statement identity(
                connection_,
                "SELECT id,resource_namespace,resource_id,card_digest,matched_occurrence FROM "
                "forward_occurrences WHERE scope=? AND message_id=? AND part=?"
            );
            identity.bind(1, context.scope);
            identity.bind(2, context.message_id);
            identity.bind(3, std::int64_t(context.part));
            if (!identity.step()) {
                throw std::runtime_error("Missing replay forward reference");
            }
            const huoxing::ForwardReference reference{
                identity.text(1),
                identity.text(2),
                identity.text(3)
            };
            result.fingerprint = huoxing::forward_fingerprint(reference, context);
            result.storage = {huoxing::MessageKind::Forward, identity.integer(0)};
            if (cached->status == "same_resource") {
                result.status = huoxing::DetectionStatus::Duplicate;
            } else if (cached->status == "possible_same_card") {
                result.status = huoxing::DetectionStatus::PossibleSameCard;
            } else if (cached->status == "insufficient_evidence") {
                result.status = huoxing::DetectionStatus::InsufficientEvidence;
            }
            if (!cached->first_message_id.empty()) {
                result.first = huoxing::FirstOccurrence{
                    cached->first_message_id,
                    known_sender(context.scope, identity.integer(4)),
                    cached->first_seen
                };
            }
        }

        return result;
    }

    std::optional<huoxing::Candidate>
    lookup(std::string_view scope, const huoxing::Fingerprint& key) override {
        if (key.basis == huoxing::HashBasis::Occurrence) {
            return {};
        }

        // The common hash index handles all newly registered message kinds.
        Statement indexed(
            connection_,
            "SELECT storage_id,sender_id FROM message_hashes WHERE scope=? AND kind=? AND digest=?"
        );
        indexed.bind(1, scope);
        indexed.bind(2, std::int64_t(key.kind));
        indexed.bind(3, key.value);
        if (indexed.step()) {
            huoxing::Candidate candidate{{key.kind, indexed.integer(0)}, {}, {}};
            if (key.kind == huoxing::MessageKind::Forward) {
                candidate.first = first_occurrence(candidate);
                candidate.first->sender_id = indexed.text(1);
            }
            return candidate;
        }

        // Legacy records are looked up by their original indexes; no bulk rewrite is required.
        const auto& fields = key.identity_fields;
        if (key.basis == huoxing::HashBasis::DecodedPixels && fields.size() == 1) {
            const auto id = images_.find_exact(scope, fields[0]);
            if (id != 0) {
                return huoxing::Candidate{{huoxing::MessageKind::Image, id}, {}, {}};
            }
        }

        std::optional<huoxing::ForwardMatch> match;
        if (key.basis == huoxing::HashBasis::ForwardResource && fields.size() == 2) {
            match = forwards_.find_resource(scope, fields[0], fields[1]);
        } else if (key.basis == huoxing::HashBasis::VisibleCard && fields.size() == 1) {
            match = forwards_.find_card(scope, fields[0]);
        }
        if (match) {
            return huoxing::Candidate{
                {huoxing::MessageKind::Forward, match->occurrence_id},
                huoxing::FirstOccurrence{match->message_id, "", match->received_at},
                {}
            };
        }

        return {};
    }

    void visit_candidates(
        const huoxing::PreparedMessage& message,
        const std::function<bool(const huoxing::Candidate&)>& visitor
    ) override {
        if (message.fingerprint.kind != huoxing::MessageKind::Image) {
            return;
        }

        images_.visit_candidates(
            message.context.scope,
            [&](std::int64_t id, const huoxing::Feature& feature) {
                return visitor(huoxing::Candidate{{huoxing::MessageKind::Image, id}, {}, feature});
            }
        );
    }

    huoxing::FirstOccurrence first_occurrence(const huoxing::Candidate& candidate) override {
        if (candidate.first) {
            return *candidate.first;
        }

        if (candidate.storage.kind == huoxing::MessageKind::Image) {
            huoxing::Result first;
            images_.attach_first_occurrence(first, candidate.storage.id);
            return {first.first_message_id, first.first_sender_id, first.first_seen};
        }

        Statement query(
            connection_,
            "SELECT message_id,received_at FROM forward_occurrences WHERE id=?"
        );
        query.bind(1, candidate.storage.id);
        if (!query.step()) {
            throw std::runtime_error("Missing forward occurrence");
        }
        return {query.text(0), "", query.text(1)};
    }

    huoxing::StorageRef record(
        const huoxing::PreparedMessage& message,
        const huoxing::Detection& detection,
        const std::optional<huoxing::Candidate>& matched
    ) override {
        const auto& context = message.context;
        const auto matched_id = matched ? matched->storage.id : 0;
        huoxing::StorageRef storage{message.fingerprint.kind, 0};

        if (const auto* feature = std::get_if<huoxing::Feature>(&message.payload)) {
            storage.id = images_.find_exact(context.scope, feature->digest);
            if (storage.id == 0) {
                storage.id = images_.insert_image(context.scope, *feature, capacity_);
            }
            auto cached = huoxing::image_result(detection);
            cached.image_id = storage.id;
            images_.insert_occurrence(
                context.scope,
                context.message_id,
                context.sender_id,
                context.part,
                cached,
                matched_id
            );
        } else {
            const auto& reference = std::get<huoxing::ForwardReference>(message.payload);
            const auto cached = huoxing::forward_result(detection);
            storage.id = forwards_.insert(
                context.scope,
                context.message_id,
                context.part,
                reference.resource_namespace,
                reference.resource_id,
                reference.card_digest,
                cached.status,
                matched_id
            );
        }

        index(context, message.fingerprint, storage);
        if (message.hint) {
            index(context, *message.hint, storage);
        }
        return storage;
    }

    void erase(std::string_view scope) {
        images_.delete_scope(scope);
        forwards_.delete_scope(scope);
        Statement remove(connection_, "DELETE FROM message_hashes WHERE scope=?");
        remove.bind(1, scope);
        remove.step();
    }

    void commit() override {
        transaction_.commit();
    }

  private:

    std::string known_sender(std::string_view scope, std::int64_t storage_id) {
        Statement query(
            connection_,
            "SELECT sender_id FROM message_hashes WHERE scope=? AND kind=2 AND storage_id=? AND "
            "sender_id<>'' LIMIT 1"
        );
        query.bind(1, scope);
        query.bind(2, storage_id);
        return query.step() ? query.text(0) : std::string{};
    }

    void index(
        const huoxing::MessageContext& context,
        const huoxing::Fingerprint& key,
        huoxing::StorageRef storage
    ) {
        auto target_id = storage.id;
        std::string sender;
        if (key.kind == huoxing::MessageKind::Image) {
            huoxing::Result first;
            images_.attach_first_occurrence(first, target_id);
            sender = first.first_sender_id;
        } else {
            // Point a lazily created index at the earliest legacy record, not this repost.
            std::optional<huoxing::ForwardMatch> earliest;
            if (key.basis == huoxing::HashBasis::ForwardResource) {
                earliest = forwards_.find_resource(
                    context.scope,
                    key.identity_fields[0],
                    key.identity_fields[1]
                );
            } else if (key.basis == huoxing::HashBasis::VisibleCard) {
                earliest = forwards_.find_card(context.scope, key.identity_fields[0]);
            }
            if (earliest) {
                target_id = earliest->occurrence_id;
            }
            sender = target_id == storage.id ? context.sender_id
                                             : known_sender(context.scope, target_id);
        }
        Statement insert(
            connection_,
            "INSERT OR IGNORE INTO message_hashes(scope,kind,digest,basis,storage_id,sender_id) "
            "VALUES(?,?,?,?,?,?)"
        );
        insert.bind(1, context.scope);
        insert.bind(2, std::int64_t(key.kind));
        insert.bind(3, key.value);
        insert.bind(4, std::int64_t(key.basis));
        insert.bind(5, target_id);
        insert.bind(6, sender);
        insert.step();
    }

    huoxing::storage::Connection connection_;
    huoxing::storage::Transaction transaction_;
    huoxing::storage::ImageRepository images_;
    huoxing::storage::ForwardRepository forwards_;
    int capacity_;
};

} // namespace

export namespace huoxing {

class SqliteRepository final : public Repository {
  public:

    explicit SqliteRepository(std::string path, int max_images = 2000)
        : path_(std::move(path)), max_images_(max_images) {
        if (path_.empty() || path_ == ":memory:" || max_images_ < 1) {
            throw std::invalid_argument("Need file database and positive capacity");
        }
        storage::ImageRepository::initialize(path_);
    }

    std::unique_ptr<RepositorySession> begin() const override {
        return std::make_unique<SqliteSession>(path_, max_images_);
    }

    void delete_scope(std::string_view scope) const override {
        SqliteSession session(path_, max_images_);
        session.erase(scope);
        session.commit();
    }

  private:

    std::string path_;
    int max_images_;
};

} // namespace huoxing
