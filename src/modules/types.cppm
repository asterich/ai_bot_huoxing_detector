module;
#include <opencv2/core.hpp>

export module huoxing.types;
import std;

export namespace huoxing {

// Evidence describes measurable checks; none of these values is a probability.
struct Evidence {
    std::string method;
    int inliers = 0;
    double inlier_ratio = 0;
    double coverage = 0;
    double pixel_agreement = 0;
};

// Results own their strings so they remain valid after the SQLite cursor closes.
struct Result {
    bool duplicate = false;
    bool replayed = false;
    std::int64_t image_id = 0;
    std::string first_message_id;
    std::string first_sender_id;
    std::string first_seen;
    Evidence evidence;
};

struct Feature {
    std::string digest;
    cv::Mat gray;
    cv::Mat points;
    cv::Mat descriptors;
};

} // namespace huoxing
