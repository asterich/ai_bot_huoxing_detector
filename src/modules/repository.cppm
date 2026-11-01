export module huoxing.repository;
import std;
import huoxing.messages;

export namespace huoxing {

// One session owns the atomic search/register transaction for either message kind.
class RepositorySession {
  public:

    virtual ~RepositorySession() = default;

    virtual std::optional<Detection> replay(const PreparedMessage& message) = 0;

    virtual std::optional<Candidate> lookup(std::string_view scope, const Fingerprint& key) = 0;

    virtual void visit_candidates(
        const PreparedMessage& message,
        const std::function<bool(const Candidate&)>& visitor
    ) = 0;

    virtual FirstOccurrence first_occurrence(const Candidate& candidate) = 0;

    virtual StorageRef record(
        const PreparedMessage& message,
        const Detection& detection,
        const std::optional<Candidate>& matched
    ) = 0;

    virtual void commit() = 0;
};

class Repository {
  public:

    virtual ~Repository() = default;

    virtual std::unique_ptr<RepositorySession> begin() const = 0;

    virtual void delete_scope(std::string_view scope) const = 0;
};

} // namespace huoxing
