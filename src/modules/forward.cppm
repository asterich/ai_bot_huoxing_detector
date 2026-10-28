export module huoxing.forward;
import std;
import huoxing.messages;
import huoxing.detector;
import huoxing.forward_types;

export namespace huoxing {

// A typed compatibility facade. Detection and transaction policy live in Detector.
class ForwardDetector {
  public:

    explicit ForwardDetector(std::string path)
        : detector_(std::make_shared<Detector>(std::move(path))) {}

    explicit ForwardDetector(std::shared_ptr<IDetector> detector) : detector_(std::move(detector)) {
        if (!detector_) {
            throw std::invalid_argument("A detector is required");
        }
    }

    ForwardResult ingest(
        std::string_view scope,
        std::string_view message_id,
        int part,
        std::string_view resource_namespace,
        std::string_view resource_id,
        std::string_view visible_card = ""
    ) const {
        Message message{
            {std::string(scope), std::string(message_id), "", part},
            ForwardMessage{
                std::string(resource_namespace),
                std::string(resource_id),
                std::string(visible_card)
            }
        };
        return forward_result(detector_->detect(message));
    }

    void delete_scope(std::string_view scope) const {
        detector_->delete_scope(scope);
    }

  private:

    std::shared_ptr<IDetector> detector_;
};

} // namespace huoxing
