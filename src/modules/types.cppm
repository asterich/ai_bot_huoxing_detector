module;
#include <cstdint>
#include <opencv2/core.hpp>
#include <string>

export module huoxing.types;

// Shared value types contain evidence, never a calibrated confidence probability.
export namespace huoxing {
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
} // namespace huoxing
