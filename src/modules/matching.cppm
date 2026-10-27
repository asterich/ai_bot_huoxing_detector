module;
#include <algorithm>
#include <array>
#include <cmath>
#include <opencv2/calib3d.hpp>
#include <opencv2/core.hpp>
#include <opencv2/features2d.hpp>
#include <opencv2/imgproc.hpp>
#include <optional>
#include <set>
#include <utility>
#include <vector>

export module huoxing.matching;
import huoxing.types;

namespace {
std::vector<cv::Point2f> points(const cv::Mat& m) {
    std::vector<cv::Point2f> out;
    for (int i = 0; i < m.rows; ++i) {
        auto v = m.at<cv::Vec2f>(i);
        out.emplace_back(v[0], v[1]);
    }
    return out;
}
double coverage(const std::vector<cv::Point2f>& p, const cv::Mat& gray) {
    if (p.size() < 3)
        return 0;
    std::vector<cv::Point2f> hull;
    cv::convexHull(p, hull);
    return std::abs(cv::contourArea(hull)) / double(gray.total());
}
} // namespace
export namespace huoxing {
// SIFT locates common content. RANSAC and pixel agreement reject weak local overlaps.
std::optional<Evidence> compare(const Feature& a, const Feature& b) {
    if (a.digest == b.digest)
        return Evidence{"decoded_pixels"};
    if (std::min(a.descriptors.rows, b.descriptors.rows) < 12)
        return {};
    cv::BFMatcher matcher(cv::NORM_L2);
    std::vector<std::vector<cv::DMatch>> forward, reverse;
    matcher.knnMatch(a.descriptors, b.descriptors, forward, 2);
    matcher.knnMatch(b.descriptors, a.descriptors, reverse, 2);
    std::set<std::pair<int, int>> rev;
    for (const auto& pair : reverse)
        if (pair.size() == 2 && pair[0].distance < 0.72f * pair[1].distance)
            rev.emplace(pair[0].trainIdx, pair[0].queryIdx);
    auto ap = points(a.points), bp = points(b.points);
    std::vector<cv::Point2f> src, dst;
    std::set<std::array<int, 4>> unique;
    for (const auto& pair : forward) {
        if (pair.size() != 2)
            continue;
        auto m = pair[0];
        if (m.distance >= 0.72f * pair[1].distance || !rev.contains({m.queryIdx, m.trainIdx}))
            continue;
        auto p = ap[m.queryIdx], q = bp[m.trainIdx];
        if (unique.insert({cvRound(p.x), cvRound(p.y), cvRound(q.x), cvRound(q.y)}).second) {
            src.push_back(p);
            dst.push_back(q);
        }
    }
    if (src.size() < 12)
        return {};
    cv::Mat mask;
    auto h = cv::findHomography(src, dst, cv::RANSAC, 3.0, mask, 2000, 0.995);
    if (h.empty() || !cv::checkRange(h))
        return {};
    std::vector<cv::Point2f> si, di;
    for (int i = 0; i < int(src.size()); ++i)
        if (mask.at<unsigned char>(i)) {
            si.push_back(src[i]);
            di.push_back(dst[i]);
        }
    int count = int(si.size());
    double ratio = double(count) / src.size(),
           cov = std::max(coverage(si, a.gray), coverage(di, b.gray));
    if (count < 12 || ratio < 0.55 || cov < 0.20)
        return {};
    std::vector<cv::Point2f> corners{{0, 0},
                                     {float(a.gray.cols), 0},
                                     {float(a.gray.cols), float(a.gray.rows)},
                                     {0, float(a.gray.rows)}},
        mapped;
    cv::perspectiveTransform(corners, mapped, h);
    for (auto p : mapped)
        if (!std::isfinite(p.x) || !std::isfinite(p.y))
            return {};
    double area = std::abs(cv::contourArea(mapped)) / b.gray.total();
    if (!cv::isContourConvex(mapped) || area < 0.02 || area > 50)
        return {};
    cv::Mat aligned, valid, aa, bb, diff, agree;
    cv::warpPerspective(a.gray, aligned, h, b.gray.size());
    cv::warpPerspective(cv::Mat(a.gray.size(), CV_8U, cv::Scalar(255)), valid, h, b.gray.size());
    cv::erode(valid, valid, cv::Mat::ones(5, 5, CV_8U));
    cv::threshold(valid, valid, 250, 255, cv::THRESH_BINARY);
    int pixels = cv::countNonZero(valid);
    if (pixels < 1024 || double(pixels) / valid.total() < 0.15)
        return {};
    cv::GaussianBlur(aligned, aa, {3, 3}, 0);
    cv::GaussianBlur(b.gray, bb, {3, 3}, 0);
    cv::absdiff(aa, bb, diff);
    cv::compare(diff, 32, agree, cv::CMP_LT);
    cv::bitwise_and(agree, valid, agree);
    double agreement = double(cv::countNonZero(agree)) / pixels;
    if (agreement < 0.72)
        return {};
    return Evidence{"sift_ransac_pixels", count, ratio, cov, agreement};
}
} // namespace huoxing
