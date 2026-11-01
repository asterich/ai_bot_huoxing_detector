export module huoxing.forward_database;
import std;
import huoxing.sqlite;
import huoxing.forward_types;

export namespace huoxing::storage {

// Owns forward-record SQL separately from the image repository and matching policy.
class ForwardRepository {
  public:

    explicit ForwardRepository(Connection& connection) : connection_(connection) {}

    std::optional<ForwardResult>
    find_replay(std::string_view scope, std::string_view message_id, int part) {
        Statement query(
            connection_,
            "SELECT status,matched_occurrence FROM forward_occurrences "
            "WHERE scope=? AND message_id=? AND part=?"
        );
        query.bind(1, scope);
        query.bind(2, message_id);
        query.bind(3, std::int64_t(part));
        if (!query.step()) {
            return {};
        }
        ForwardResult result;
        result.status = query.text(0);
        result.replayed = true;
        const auto matched_id = query.integer(1);
        if (matched_id != 0) {
            Statement first(
                connection_,
                "SELECT message_id,received_at FROM forward_occurrences WHERE id=?"
            );
            first.bind(1, matched_id);
            if (first.step()) {
                result.first_message_id = first.text(0);
                result.first_seen = first.text(1);
            }
        }
        return result;
    }

    std::optional<ForwardMatch> find_resource(
        std::string_view scope,
        std::string_view resource_namespace,
        std::string_view resource_id
    ) {
        Statement query(
            connection_,
            "SELECT id,message_id,received_at FROM forward_occurrences "
            "WHERE scope=? AND resource_namespace=? AND resource_id=? ORDER BY id LIMIT 1"
        );
        query.bind(1, scope);
        query.bind(2, resource_namespace);
        query.bind(3, resource_id);
        return read_match(query);
    }

    std::optional<ForwardMatch> find_card(std::string_view scope, std::string_view card_digest) {
        Statement query(
            connection_,
            "SELECT id,message_id,received_at FROM forward_occurrences "
            "WHERE scope=? AND card_digest=? ORDER BY id LIMIT 1"
        );
        query.bind(1, scope);
        query.bind(2, card_digest);
        return read_match(query);
    }

    std::int64_t insert(
        std::string_view scope,
        std::string_view message_id,
        int part,
        std::string_view resource_namespace,
        std::string_view resource_id,
        std::string_view card_digest,
        std::string_view status,
        std::int64_t matched_id
    ) {
        Statement query(
            connection_,
            "INSERT INTO forward_occurrences(scope,message_id,part,resource_namespace,"
            "resource_id,card_digest,status,matched_occurrence) "
            "VALUES(?,?,?,?,?,?,?,NULLIF(?,0))"
        );
        query.bind(1, scope);
        query.bind(2, message_id);
        query.bind(3, std::int64_t(part));
        query.bind(4, resource_namespace);
        query.bind(5, resource_id);
        query.bind(6, card_digest);
        query.bind(7, status);
        query.bind(8, matched_id);
        query.step();
        return connection_.last_insert_id();
    }

    void delete_scope(std::string_view scope) {
        Statement query(connection_, "DELETE FROM forward_occurrences WHERE scope=?");
        query.bind(1, scope);
        query.step();
    }

  private:

    static std::optional<ForwardMatch> read_match(Statement& query) {
        if (!query.step()) {
            return {};
        }
        return ForwardMatch{query.integer(0), query.text(1), query.text(2)};
    }

    Connection& connection_;
};
} // namespace huoxing::storage
