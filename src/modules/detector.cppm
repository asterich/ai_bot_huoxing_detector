export module huoxing.detector;
import std;
import huoxing.vision;
import huoxing.types;
import huoxing.messages;
import huoxing.repository;
import huoxing.repository_sqlite;
import huoxing.features;
import huoxing.hashing;
import huoxing.matching;

namespace {

huoxing::PreparedMessage prepare_message(const huoxing::Message& message) {
    const auto& context = message.context;
    if (context.scope.empty() || context.message_id.empty() || context.part < 0) {
        throw std::invalid_argument("scope/message_id required; part >=0");
    }

    huoxing::PreparedMessage prepared;
    prepared.context = context;
    if (const auto* image = std::get_if<huoxing::ImageMessage>(&message.payload)) {
        auto feature = huoxing::extract(image->rgb);
        prepared.fingerprint = huoxing::make_fingerprint(
            huoxing::MessageKind::Image,
            huoxing::HashBasis::DecodedPixels,
            {feature.digest}
        );
        prepared.payload = std::move(feature);
    } else {
        const auto& forward = std::get<huoxing::ForwardMessage>(message.payload);
        if (forward.resource_namespace.empty() || forward.resource_id.size() > 4096
            || forward.visible_card.size() > 65536) {
            throw std::invalid_argument("Invalid top-level forward metadata");
        }

        huoxing::ForwardReference reference{
            forward.resource_namespace,
            forward.resource_id,
            forward.visible_card.empty() ? std::string{}
                                         : huoxing::hashing::sha256_text(forward.visible_card)
        };
        prepared.fingerprint = huoxing::forward_fingerprint(reference, context);
        if (!reference.card_digest.empty()) {
            prepared.hint = huoxing::make_fingerprint(
                huoxing::MessageKind::Forward,
                huoxing::HashBasis::VisibleCard,
                {reference.card_digest}
            );
        }
        prepared.payload = std::move(reference);
    }

    return prepared;
}

} // namespace

export namespace huoxing {

class IDetector {
  public:

    virtual ~IDetector() = default;

    virtual Detection detect(const Message& message) const = 0;

    virtual void delete_scope(std::string_view scope) const = 0;
};

// One coordinator handles replay, exact identity, hints and approximate matches.
// Storage is injected through an abstract repository; no SQL appears in this module.
class Detector final : public IDetector {
  public:

    explicit Detector(std::shared_ptr<Repository> repository) : repository_(std::move(repository)) {
        if (!repository_) {
            throw std::invalid_argument("A repository is required");
        }
    }

    explicit Detector(std::string path, int max_images = 2000)
        : Detector(std::make_shared<SqliteRepository>(std::move(path), max_images)) {}

    Detection detect(const Message& message) const override {
        // CPU work happens before taking the SQLite writer lock.
        const auto prepared = prepare_message(message);
        auto session = repository_->begin();
        if (auto replayed = session->replay(prepared)) {
            session->commit();
            return *replayed;
        }

        Detection result;
        result.fingerprint = prepared.fingerprint;
        result.storage.kind = prepared.fingerprint.kind;
        if (prepared.fingerprint.kind == MessageKind::Forward
            && (prepared.fingerprint.basis == HashBasis::VisibleCard
                || prepared.fingerprint.basis == HashBasis::Occurrence)) {
            result.status = DetectionStatus::InsufficientEvidence;
        }

        auto matched = session->lookup(prepared.context.scope, prepared.fingerprint);
        auto matched_basis = prepared.fingerprint.basis;
        if (!matched && prepared.hint && prepared.hint->value != prepared.fingerprint.value) {
            matched = session->lookup(prepared.context.scope, *prepared.hint);
            matched_basis = prepared.hint->basis;
        }
        if (matched) {
            result.status = matched_basis == HashBasis::VisibleCard
                              ? DetectionStatus::PossibleSameCard
                              : DetectionStatus::Duplicate;
            result.evidence.method = basis_name(matched_basis);
        }

        // Only image payloads support geometric approximate matching.
        if (!matched) {
            if (const auto* query = std::get_if<Feature>(&prepared.payload)) {
                session->visit_candidates(prepared, [&](const Candidate& candidate) {
                    if (!candidate.image) {
                        return true;
                    }
                    if (auto evidence = compare(*query, *candidate.image)) {
                        matched = candidate;
                        matched->image.reset();
                        result.status = DetectionStatus::Duplicate;
                        result.evidence = *evidence;
                        return false;
                    }
                    return true;
                });
            }
        }

        if (matched) {
            result.first = session->first_occurrence(*matched);
        }
        result.storage = session->record(prepared, result, matched);
        session->commit();
        return result;
    }

    // Compatibility entry point for existing image plugins and native callers.
    Result ingest(
        const cv::Mat& rgb,
        std::string_view scope,
        std::string_view message_id,
        std::string_view sender_id = "",
        int part = 0
    ) const {
        Message message{
            {std::string(scope), std::string(message_id), std::string(sender_id), part},
            ImageMessage{rgb}
        };
        return image_result(detect(message));
    }

    void delete_scope(std::string_view scope) const override {
        repository_->delete_scope(scope);
    }

  private:

    std::shared_ptr<Repository> repository_;
};

} // namespace huoxing
