module;
#include "std_compat.hpp"
#include <opencv2/calib3d.hpp>
#include <opencv2/core.hpp>
#include <opencv2/features2d.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

export module huoxing.vision;
import std;

// Own the global-fragment declarations once. The using-declarations re-export
// vendor entities without attaching OpenCV's ABI to a project named module.
export namespace cv {
using ::cv::BFMatcher;
using ::cv::DMatch;
using ::cv::KeyPoint;
using ::cv::Mat;
using ::cv::Point;
using ::cv::Point2f;
using ::cv::Rect;
using ::cv::RNG;
using ::cv::Scalar;
using ::cv::SIFT;
using ::cv::Vec2f;

using ::cv::BORDER_CONSTANT;
using ::cv::CMP_LT;
using ::cv::COLOR_RGB2GRAY;
using ::cv::FONT_HERSHEY_SIMPLEX;
using ::cv::IMREAD_GRAYSCALE;
using ::cv::INTER_AREA;
using ::cv::NORM_L2;
using ::cv::RANSAC;
using ::cv::THRESH_BINARY;

using ::cv::absdiff;
using ::cv::bitwise_and;
using ::cv::checkRange;
using ::cv::circle;
using ::cv::compare;
using ::cv::contourArea;
using ::cv::convexHull;
using ::cv::copyMakeBorder;
using ::cv::countNonZero;
using ::cv::cvtColor;
using ::cv::erode;
using ::cv::findHomography;
using ::cv::GaussianBlur;
using ::cv::imdecode;
using ::cv::imencode;
using ::cv::isContourConvex;
using ::cv::line;
using ::cv::noArray;
using ::cv::perspectiveTransform;
using ::cv::putText;
using ::cv::rectangle;
using ::cv::resize;
using ::cv::threshold;
using ::cv::warpPerspective;
} // namespace cv

export namespace huoxing::vision {

// Macros cannot cross a module boundary; expose only the pixel formats we use.
inline constexpr int rgb_type = CV_8UC3;
inline constexpr int gray_type = CV_8U;
inline constexpr int point_type = CV_32FC2;
inline constexpr int descriptor_type = CV_32F;

int round_coordinate(float value) {
    return cvRound(value);
}

} // namespace huoxing::vision
