export module huoxing.detector;
import std;
import huoxing.vision;
import huoxing.types;
import huoxing.features;
import huoxing.matching;
import huoxing.database;

export namespace huoxing {

// Orchestrates image matching. Persistence and transaction details stay in the database module.
class Detector {
  public:

    explicit Detector(std::string path, int max_images = 2000)
        : path_(std::move(path)), max_images_(max_images) {
        if (path_.empty() || path_ == ":memory:" || max_images_ < 1) {
            throw std::invalid_argument("Need file database and positive capacity");
        }
        storage::ImageRepository::initialize(path_);
    }

    Result ingest(
        const cv::Mat& rgb,
        std::string_view scope,
        std::string_view message_id,
        std::string_view sender_id = "",
        int part = 0
    ) const {
        if (scope.empty() || message_id.empty() || part < 0) {
            throw std::invalid_argument("scope/message_id required; part >=0");
        }

        // Extract before acquiring SQLite's writer lock; only search/register must be atomic.
        auto query = extract(rgb);
        storage::ImageRepository repository(path_);
        if (auto replay = repository.find_replay(scope, message_id, part)) {
            return *replay;
        }

        Result result;
        auto existing_image = repository.find_exact(scope, query.digest);
        auto matched_image = existing_image;
        if (existing_image != 0) {
            result.evidence.method = "decoded_pixels";
        } else {
            repository.visit_candidates(scope, [&](std::int64_t id, const Feature& candidate) {
                auto evidence = compare(query, candidate);
                if (!evidence) {
                    return true;
                }
                matched_image = id;
                result.evidence = *evidence;
                return false;
            });
        }

        result.image_id = existing_image != 0 ? existing_image
                                              : repository.insert_image(scope, query, max_images_);
        result.duplicate = matched_image != 0;
        if (result.duplicate) {
            repository.attach_first_occurrence(result, matched_image);
        }
        repository.insert_occurrence(scope, message_id, sender_id, part, result, matched_image);
        repository.commit();
        return result;
    }

    // Authentication belongs to the host plugin, before this maintenance entry point is called.
    void delete_scope(std::string_view scope) const {
        storage::ImageRepository repository(path_);
        repository.delete_scope(scope);
        repository.commit();
    }

  private:

    std::string path_;
    int max_images_;
};
} // namespace huoxing
