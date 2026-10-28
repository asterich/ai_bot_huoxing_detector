module;
#include <opencv2/core.hpp>
#include <opencv2/features2d.hpp>
#include <opencv2/imgproc.hpp>

export module huoxing.features;
import std;
import huoxing.types;
import huoxing.hashing;

export namespace huoxing {

Feature extract(const cv::Mat& rgb) {
    if (rgb.type() != CV_8UC3 || rgb.rows < 16 || rgb.cols < 16 || rgb.total() > 16000000) {
        throw std::invalid_argument("Expected RGB uint8, dimensions >=16 and <=16M pixels");
    }

    // Hash the full decoded image. Resizing affects descriptors, never identity.
    Feature feature;
    feature.digest = hashing::sha256_rgb(rgb);

    cv::Mat resized = rgb;
    const auto longest_side = std::max(rgb.rows, rgb.cols);
    if (longest_side > 960) {
        const auto scale = 960.0 / longest_side;
        cv::resize(rgb, resized, {}, scale, scale, cv::INTER_AREA);
    }
    cv::cvtColor(resized, feature.gray, cv::COLOR_RGB2GRAY);

    std::vector<cv::KeyPoint> keypoints;
    cv::SIFT::create(1000)
        ->detectAndCompute(feature.gray, cv::noArray(), keypoints, feature.descriptors);

    feature.points = cv::Mat(static_cast<int>(keypoints.size()), 1, CV_32FC2);
    for (const auto index : std::views::iota(0, static_cast<int>(keypoints.size()))) {
        const auto location = keypoints[index].pt;
        feature.points.at<cv::Vec2f>(index) = {location.x, location.y};
    }

    // Keep the empty descriptor shape consistent with the persisted feature format.
    if (feature.descriptors.empty()) {
        feature.descriptors = cv::Mat(0, 128, CV_32F);
    }

    return feature;
}

} // namespace huoxing
