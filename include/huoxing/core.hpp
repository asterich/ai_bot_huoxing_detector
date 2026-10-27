#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <opencv2/core.hpp>

namespace huoxing {
struct Evidence {
    std::string method;
    int inliers = 0;
    double inlier_ratio = 0, coverage = 0, pixel_agreement = 0;
};
struct Result {
    bool duplicate = false;
    bool replayed = false;
    std::int64_t image_id = 0;
    std::string first_message_id, first_sender_id, first_seen;
    Evidence evidence;
};
struct Feature {
    std::string digest;
    cv::Mat gray, points, descriptors;
};
struct ForwardResult {
    std::string status; // new_resource / same_resource / possible_same_card / insufficient_evidence
    bool replayed = false;
    std::string first_message_id, first_seen;
};
// RGB CV_8UC3, 16..16000000 pixels, dimensions >=16.
Feature extract(const cv::Mat& rgb);
std::optional<Evidence> compare(const Feature& a, const Feature& b);
class Detector {
public:
    explicit Detector(std::string path, int max_images = 2000);
    Result ingest(const cv::Mat& rgb, const std::string& scope,
                  const std::string& message_id, const std::string& sender_id = "",
                  int part = 0) const;
    void delete_scope(const std::string& scope) const;
    ForwardResult ingest_forward(const std::string& scope, const std::string& message_id,
        int part, const std::string& resource_namespace, const std::string& resource_id,
        const std::string& visible_card = "") const;
private:
    std::string path_;
    int max_images_;
};
}
