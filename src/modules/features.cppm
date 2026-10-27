module;
#include <algorithm>
#include <array>
#include <iomanip>
#include <memory>
#include <opencv2/core.hpp>
#include <opencv2/features2d.hpp>
#include <opencv2/imgproc.hpp>
#include <openssl/evp.h>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

export module huoxing.features;
import huoxing.types;

namespace {
// Hash full RGB pixels before resizing; include dimensions to disambiguate buffers.
std::string digest(const cv::Mat& rgb) {
    std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> ctx(EVP_MD_CTX_new(), EVP_MD_CTX_free);
    auto header = std::to_string(rgb.cols) + "x" + std::to_string(rgb.rows) + ":";
    std::array<unsigned char, EVP_MAX_MD_SIZE> bytes{};
    unsigned int size = 0;
    if (!ctx || EVP_DigestInit_ex(ctx.get(), EVP_sha256(), nullptr) != 1 ||
        EVP_DigestUpdate(ctx.get(), header.data(), header.size()) != 1)
        throw std::runtime_error("SHA256 initialization failed");
    for (int y = 0; y < rgb.rows; ++y)
        if (EVP_DigestUpdate(ctx.get(), rgb.ptr(y), std::size_t(rgb.cols) * 3) != 1)
            throw std::runtime_error("SHA256 update failed");
    if (EVP_DigestFinal_ex(ctx.get(), bytes.data(), &size) != 1)
        throw std::runtime_error("SHA256 final failed");
    std::ostringstream out;
    for (unsigned int i = 0; i < size; ++i)
        out << std::hex << std::setw(2) << std::setfill('0') << int(bytes[i]);
    return out.str();
}
} // namespace
export namespace huoxing {
// Input is owned, decoded RGB. The Python adapter enforces compressed-byte limits.
Feature extract(const cv::Mat& rgb) {
    if (rgb.type() != CV_8UC3 || rgb.rows < 16 || rgb.cols < 16 || rgb.total() > 16000000)
        throw std::invalid_argument("Expected RGB uint8, dimensions >=16 and <=16M pixels");
    Feature f;
    f.digest = digest(rgb);
    cv::Mat small = rgb;
    if (std::max(rgb.rows, rgb.cols) > 960) {
        double scale = 960.0 / std::max(rgb.rows, rgb.cols);
        cv::resize(rgb, small, {}, scale, scale, cv::INTER_AREA);
    }
    cv::cvtColor(small, f.gray, cv::COLOR_RGB2GRAY);
    std::vector<cv::KeyPoint> kp;
    cv::SIFT::create(1000)->detectAndCompute(f.gray, cv::noArray(), kp, f.descriptors);
    f.points = cv::Mat(int(kp.size()), 1, CV_32FC2);
    for (int i = 0; i < int(kp.size()); ++i)
        f.points.at<cv::Vec2f>(i) = {kp[i].pt.x, kp[i].pt.y};
    if (f.descriptors.empty())
        f.descriptors = cv::Mat(0, 128, CV_32F);
    return f;
}
} // namespace huoxing
