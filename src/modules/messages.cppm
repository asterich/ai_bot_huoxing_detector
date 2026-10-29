export module huoxing.messages;
import std;
import huoxing.vision;
import huoxing.format;
import huoxing.types;
import huoxing.forward_types;
import huoxing.hashing;

export namespace huoxing {

enum class MessageKind { Image = 1, Forward = 2 };
enum class HashBasis { DecodedPixels = 1, ForwardResource = 2, VisibleCard = 3, Occurrence = 4 };
enum class DetectionStatus { New, Duplicate, PossibleSameCard, InsufficientEvidence };

std::string_view basis_name(HashBasis basis) {
    switch (basis) {
    case HashBasis::DecodedPixels:
        return "decoded_pixels";
    case HashBasis::ForwardResource:
        return "forward_resource";
    case HashBasis::VisibleCard:
        return "visible_card";
    case HashBasis::Occurrence:
        return "occurrence";
    }
    throw std::invalid_argument("Unknown fingerprint basis");
}

struct MessageContext {
    std::string scope;
    std::string message_id;
    std::string sender_id;
    int part = 0;
};

struct ImageMessage {
    cv::Mat rgb;
};

// Only an outer reference/card is represented. There is no node collection to traverse.
struct ForwardMessage {
    std::string resource_namespace;
    std::string resource_id;
    std::string visible_card;
};

struct Message {
    MessageContext context;
    std::variant<ImageMessage, ForwardMessage> payload;

    Message(MessageContext metadata, ImageMessage image)
        : context(std::move(metadata)), payload(std::move(image)) {}

    Message(MessageContext metadata, ForwardMessage forward)
        : context(std::move(metadata)), payload(std::move(forward)) {}
};

struct Fingerprint {
    MessageKind kind = MessageKind::Image;
    HashBasis basis = HashBasis::Occurrence;
    std::string value;
    // Needed only when looking up legacy records not yet in the common hash index.
    std::vector<std::string> identity_fields;
};

Fingerprint make_fingerprint(MessageKind kind, HashBasis basis, std::vector<std::string> fields) {
    std::string canonical = "huoxing.message.v1|";
    fmt::format_to(
        std::back_inserter(canonical),
        "{}:{}|",
        static_cast<int>(kind),
        static_cast<int>(basis)
    );
    for (const auto& field : fields) {
        // Explicit byte lengths disambiguate delimiters that occur inside a field.
        fmt::format_to(std::back_inserter(canonical), "{}:{}|", field.size(), field);
    }
    return {kind, basis, hashing::sha256_text(canonical), std::move(fields)};
}

struct ForwardReference {
    std::string resource_namespace;
    std::string resource_id;
    std::string card_digest;
};

Fingerprint forward_fingerprint(const ForwardReference& reference, const MessageContext& context) {
    if (!reference.resource_id.empty()) {
        return make_fingerprint(
            MessageKind::Forward,
            HashBasis::ForwardResource,
            {reference.resource_namespace, reference.resource_id}
        );
    }
    if (!reference.card_digest.empty()) {
        return make_fingerprint(
            MessageKind::Forward,
            HashBasis::VisibleCard,
            {reference.card_digest}
        );
    }
    // A delivery can still be identified when there is no cross-delivery evidence.
    return make_fingerprint(
        MessageKind::Forward,
        HashBasis::Occurrence,
        {context.scope, context.message_id, fmt::format("{}", context.part)}
    );
}

struct PreparedMessage {
    MessageContext context;
    Fingerprint fingerprint;
    std::optional<Fingerprint> hint;
    std::variant<Feature, ForwardReference> payload;
};

struct FirstOccurrence {
    std::string message_id;
    std::string sender_id;
    std::string received_at;
};

struct StorageRef {
    MessageKind kind = MessageKind::Image;
    std::int64_t id = 0;
};

struct Candidate {
    StorageRef storage;
    std::optional<FirstOccurrence> first;
    std::optional<Feature> image;
};

struct Detection {
    DetectionStatus status = DetectionStatus::New;
    Fingerprint fingerprint;
    bool replayed = false;
    StorageRef storage;
    std::optional<FirstOccurrence> first;
    Evidence evidence;
};

Result image_result(const Detection& detection) {
    Result result;
    result.duplicate = detection.status == DetectionStatus::Duplicate;
    result.replayed = detection.replayed;
    result.image_id = detection.storage.id;
    result.evidence = detection.evidence;
    result.message_hash = detection.fingerprint.value;
    result.hash_basis = basis_name(detection.fingerprint.basis);
    if (detection.first) {
        result.first_message_id = detection.first->message_id;
        result.first_sender_id = detection.first->sender_id;
        result.first_seen = detection.first->received_at;
    }
    return result;
}

ForwardResult forward_result(const Detection& detection) {
    ForwardResult result;
    switch (detection.status) {
    case DetectionStatus::New:
        result.status = "new_resource";
        break;
    case DetectionStatus::Duplicate:
        result.status = "same_resource";
        break;
    case DetectionStatus::PossibleSameCard:
        result.status = "possible_same_card";
        break;
    case DetectionStatus::InsufficientEvidence:
        result.status = "insufficient_evidence";
        break;
    }
    result.replayed = detection.replayed;
    result.message_hash = detection.fingerprint.value;
    result.hash_basis = basis_name(detection.fingerprint.basis);
    if (detection.first) {
        result.first_message_id = detection.first->message_id;
        result.first_seen = detection.first->received_at;
    }
    return result;
}

} // namespace huoxing
