#include "std_compat.hpp"
#include <pybind11/numpy.h>
#include <pybind11/pybind11.h>

import std;
import huoxing.vision;
import huoxing;

namespace py = pybind11;

namespace {

void bind_evidence_and_results(py::module_& module) {
    py::class_<huoxing::Evidence>(module, "Evidence")
        .def_readonly("method", &huoxing::Evidence::method)
        .def_readonly("inliers", &huoxing::Evidence::inliers)
        .def_readonly("inlier_ratio", &huoxing::Evidence::inlier_ratio)
        .def_readonly("coverage", &huoxing::Evidence::coverage)
        .def_readonly("pixel_agreement", &huoxing::Evidence::pixel_agreement);

    py::class_<huoxing::Result>(module, "Result")
        .def_readonly("duplicate", &huoxing::Result::duplicate)
        .def_readonly("replayed", &huoxing::Result::replayed)
        .def_readonly("image_id", &huoxing::Result::image_id)
        .def_readonly("first_message_id", &huoxing::Result::first_message_id)
        .def_readonly("first_sender_id", &huoxing::Result::first_sender_id)
        .def_readonly("first_seen", &huoxing::Result::first_seen)
        .def_readonly("evidence", &huoxing::Result::evidence);

    py::class_<huoxing::ForwardResult>(module, "ForwardResult")
        .def_readonly("status", &huoxing::ForwardResult::status)
        .def_readonly("replayed", &huoxing::ForwardResult::replayed)
        .def_readonly("first_message_id", &huoxing::ForwardResult::first_message_id)
        .def_readonly("first_seen", &huoxing::ForwardResult::first_seen);
}

huoxing::Result ingest_python_array(
    const huoxing::Detector& detector,
    py::array_t<unsigned char, py::array::c_style> array,
    std::string_view scope,
    std::string_view message_id,
    std::string_view sender_id,
    int part
) {
    const auto buffer = array.request();
    if (buffer.ndim != 3 || buffer.shape[2] != 3 || buffer.shape[0] < 16 || buffer.shape[1] < 16
        || buffer.shape[0] > 16000000 || buffer.shape[1] > 16000000
        || buffer.shape[0] * buffer.shape[1] > 16000000) {
        throw std::invalid_argument("Expected contiguous HxWx3 uint8 RGB, <=16M pixels");
    }

    // Copy while the GIL is held. The caller can mutate its array once we release it.
    const cv::Mat borrowed_rgb(
        static_cast<int>(buffer.shape[0]),
        static_cast<int>(buffer.shape[1]),
        huoxing::vision::rgb_type,
        buffer.ptr
    );
    const auto owned_rgb = borrowed_rgb.clone();

    // Pybind's argument casters keep the borrowed UTF-8 strings alive until this call returns.
    // The detector and SQLite either consume the views immediately or copy their contents.
    py::gil_scoped_release release;
    return detector.ingest(owned_rgb, scope, message_id, sender_id, part);
}

void bind_image_detector(py::module_& module) {
    py::class_<huoxing::Detector>(module, "Detector")
        .def(py::init<std::string, int>(), py::arg("db_path"), py::arg("max_images") = 2000)
        .def(
            "ingest_rgb",
            &ingest_python_array,
            py::arg("rgb").noconvert(),
            py::arg("scope"),
            py::arg("message_id"),
            py::arg("sender_id") = "",
            py::arg("part") = 0
        )
        .def(
            "delete_scope",
            &huoxing::Detector::delete_scope,
            py::call_guard<py::gil_scoped_release>()
        );
}

void bind_forward_detector(py::module_& module) {
    py::class_<huoxing::ForwardDetector>(module, "ForwardDetector")
        .def(py::init<std::string>(), py::arg("db_path"))
        .def(
            "ingest",
            &huoxing::ForwardDetector::ingest,
            py::arg("scope"),
            py::arg("message_id"),
            py::arg("part"),
            py::arg("resource_namespace"),
            py::arg("resource_id"),
            py::arg("visible_card") = "",
            py::call_guard<py::gil_scoped_release>()
        )
        .def(
            "delete_scope",
            &huoxing::ForwardDetector::delete_scope,
            py::call_guard<py::gil_scoped_release>()
        );
}

} // namespace

PYBIND11_MODULE(_native, module) {
    bind_evidence_and_results(module);
    bind_image_detector(module);
    bind_forward_detector(module);
}
