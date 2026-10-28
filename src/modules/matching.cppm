export module huoxing.matching;
import std;
import huoxing.vision;
import huoxing.types;

namespace {

// Keep these thresholds unchanged during the readability refactor.
constexpr int minimum_inliers = 12;
constexpr float nearest_neighbor_ratio = 0.72F;
constexpr double minimum_inlier_ratio = 0.55;
constexpr double minimum_hull_coverage = 0.20;
constexpr double minimum_pixel_agreement = 0.72;

using NeighborPairs = std::vector<std::vector<cv::DMatch>>;

struct Correspondences {
    std::vector<cv::Point2f> query;
    std::vector<cv::Point2f> candidate;
};

struct Geometry {
    cv::Mat homography;
    int inliers = 0;
    double inlier_ratio = 0;
    double coverage = 0;
};

cv::Point2f keypoint_at(const cv::Mat& locations, int index) {
    const auto point = locations.at<cv::Vec2f>(index);
    return {point[0], point[1]};
}

bool passes_ratio_test(const std::vector<cv::DMatch>& neighbors) {
    return neighbors.size() == 2
        && neighbors.front().distance < nearest_neighbor_ratio * neighbors.back().distance;
}

Correspondences
collect_mutual_matches(const huoxing::Feature& query, const huoxing::Feature& candidate) {
    cv::BFMatcher matcher(cv::NORM_L2);
    NeighborPairs forward;
    NeighborPairs reverse;
    matcher.knnMatch(query.descriptors, candidate.descriptors, forward, 2);
    matcher.knnMatch(candidate.descriptors, query.descriptors, reverse, 2);

    std::set<std::pair<int, int>> reverse_matches;
    for (const auto& neighbors : reverse | std::views::filter(passes_ratio_test)) {
        const auto& match = neighbors.front();
        reverse_matches.emplace(match.trainIdx, match.queryIdx);
    }

    // SIFT may report multiple scales at one location. Count each pixel pair only once.
    Correspondences result;
    std::set<std::array<int, 4>> unique_locations;
    for (const auto& neighbors : forward | std::views::filter(passes_ratio_test)) {
        const auto& match = neighbors.front();
        if (!reverse_matches.contains({match.queryIdx, match.trainIdx})) {
            continue;
        }

        const auto query_point = keypoint_at(query.points, match.queryIdx);
        const auto candidate_point = keypoint_at(candidate.points, match.trainIdx);
        const std::array location_key{
            huoxing::vision::round_coordinate(query_point.x),
            huoxing::vision::round_coordinate(query_point.y),
            huoxing::vision::round_coordinate(candidate_point.x),
            huoxing::vision::round_coordinate(candidate_point.y)
        };
        if (unique_locations.insert(location_key).second) {
            result.query.push_back(query_point);
            result.candidate.push_back(candidate_point);
        }
    }

    return result;
}

double hull_coverage(const std::vector<cv::Point2f>& points, const cv::Mat& image) {
    if (points.size() < 3) {
        return 0;
    }

    std::vector<cv::Point2f> hull;
    cv::convexHull(points, hull);
    return std::abs(cv::contourArea(hull)) / static_cast<double>(image.total());
}

std::optional<Geometry>
verify_geometry(const Correspondences& matches, const cv::Mat& query, const cv::Mat& candidate) {
    if (matches.query.size() < minimum_inliers) {
        return {};
    }

    cv::Mat inlier_mask;
    auto homography = cv::findHomography(
        matches.query,
        matches.candidate,
        cv::RANSAC,
        3.0,
        inlier_mask,
        2000,
        0.995
    );
    if (homography.empty() || !cv::checkRange(homography)) {
        return {};
    }

    std::vector<cv::Point2f> query_inliers;
    std::vector<cv::Point2f> candidate_inliers;
    for (const auto index : std::views::iota(0, static_cast<int>(matches.query.size()))) {
        if (inlier_mask.at<unsigned char>(index)) {
            query_inliers.push_back(matches.query[index]);
            candidate_inliers.push_back(matches.candidate[index]);
        }
    }

    const auto inliers = static_cast<int>(query_inliers.size());
    const auto ratio = static_cast<double>(inliers) / matches.query.size();
    const auto coverage =
        std::max(hull_coverage(query_inliers, query), hull_coverage(candidate_inliers, candidate));
    if (inliers < minimum_inliers || ratio < minimum_inlier_ratio
        || coverage < minimum_hull_coverage) {
        return {};
    }

    // Reject folds, invalid projections and extreme scale changes before allocating warped images.
    const std::vector<cv::Point2f> corners{
        {0, 0},
        {static_cast<float>(query.cols), 0},
        {static_cast<float>(query.cols), static_cast<float>(query.rows)},
        {0, static_cast<float>(query.rows)}
    };
    std::vector<cv::Point2f> projected_corners;
    cv::perspectiveTransform(corners, projected_corners, homography);
    if (std::ranges::any_of(projected_corners, [](const auto point) {
            return !std::isfinite(point.x) || !std::isfinite(point.y);
        })) {
        return {};
    }

    const auto area_ratio = std::abs(cv::contourArea(projected_corners)) / candidate.total();
    if (!cv::isContourConvex(projected_corners) || area_ratio < 0.02 || area_ratio > 50) {
        return {};
    }

    return Geometry{std::move(homography), inliers, ratio, coverage};
}

std::optional<double>
verify_pixels(const cv::Mat& query, const cv::Mat& candidate, const cv::Mat& homography) {
    cv::Mat aligned;
    cv::warpPerspective(query, aligned, homography, candidate.size());

    // Erode the validity mask so interpolation at the crop boundary does not count as evidence.
    cv::Mat valid;
    cv::warpPerspective(
        cv::Mat(query.size(), huoxing::vision::gray_type, cv::Scalar(255)),
        valid,
        homography,
        candidate.size()
    );
    cv::erode(valid, valid, cv::Mat::ones(5, 5, huoxing::vision::gray_type));
    cv::threshold(valid, valid, 250, 255, cv::THRESH_BINARY);

    const auto valid_pixels = cv::countNonZero(valid);
    if (valid_pixels < 1024 || static_cast<double>(valid_pixels) / valid.total() < 0.15) {
        return {};
    }

    // Mild blur tolerates JPEG/resizing noise while still checking the shared image content.
    cv::Mat blurred_query;
    cv::Mat blurred_candidate;
    cv::Mat difference;
    cv::Mat agreement_mask;
    cv::GaussianBlur(aligned, blurred_query, {3, 3}, 0);
    cv::GaussianBlur(candidate, blurred_candidate, {3, 3}, 0);
    cv::absdiff(blurred_query, blurred_candidate, difference);
    cv::compare(difference, 32, agreement_mask, cv::CMP_LT);
    cv::bitwise_and(agreement_mask, valid, agreement_mask);

    const auto agreement = static_cast<double>(cv::countNonZero(agreement_mask)) / valid_pixels;
    if (agreement < minimum_pixel_agreement) {
        return {};
    }

    return agreement;
}

} // namespace

export namespace huoxing {

std::optional<Evidence> compare(const Feature& query, const Feature& candidate) {
    if (query.digest == candidate.digest) {
        return Evidence{"decoded_pixels"};
    }
    if (std::min(query.descriptors.rows, candidate.descriptors.rows) < minimum_inliers) {
        return {};
    }

    // Each stage rejects weak evidence before the more expensive stage that follows it.
    const auto matches = collect_mutual_matches(query, candidate);
    const auto geometry = verify_geometry(matches, query.gray, candidate.gray);
    if (!geometry) {
        return {};
    }

    const auto agreement = verify_pixels(query.gray, candidate.gray, geometry->homography);
    if (!agreement) {
        return {};
    }

    return Evidence{
        "sift_ransac_pixels",
        geometry->inliers,
        geometry->inlier_ratio,
        geometry->coverage,
        *agreement
    };
}

} // namespace huoxing
