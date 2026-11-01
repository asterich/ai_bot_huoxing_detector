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
        .def_readonly("evidence", &huoxing::Result::evidence)
        .def_readonly("message_hash", &huoxing::Result::message_hash)
        .def_readonly("hash_basis", &huoxing::Result::hash_basis);

    py::class_<huoxing::ForwardResult>(module, "ForwardResult")
        .def_readonly("status", &huoxing::ForwardResult::status)
        .def_readonly("replayed", &huoxing::ForwardResult::replayed)
        .def_readonly("first_message_id", &huoxing::ForwardResult::first_message_id)
        .def_readonly("first_seen", &huoxing::ForwardResult::first_seen)
        .def_readonly("message_hash", &huoxing::ForwardResult::message_hash)
        .def_readonly("hash_basis", &huoxing::ForwardResult::hash_basis);
}

cv::Mat copy_rgb_array(py::array_t<unsigned char, py::array::c_style> array) {
    const auto buffer = array.request();
    if (buffer.ndim != 3 || buffer.shape[2] != 3 || buffer.shape[0] < 16 || buffer.shape[1] < 16
        || buffer.shape[0] > 16000000 || buffer.shape[1] > 16000000
        || buffer.shape[0] * buffer.shape[1] > 16000000) {
        throw std::invalid_argument("Expected contiguous HxWx3 uint8 RGB, <=16M pixels");
    }

    // Own the pixels before the GIL is released or the Python caller regains control.
    const cv::Mat borrowed(
        static_cast<int>(buffer.shape[0]),
        static_cast<int>(buffer.shape[1]),
        huoxing::vision::rgb_type,
        buffer.ptr
    );
    return borrowed.clone();
}

huoxing::Result ingest_python_array(
    const huoxing::Detector& detector,
    py::array_t<unsigned char, py::array::c_style> array,
    std::string_view scope,
    std::string_view message_id,
    std::string_view sender_id,
    int part
) {
    const auto rgb = copy_rgb_array(array);
    py::gil_scoped_release release;
    return detector.ingest(rgb, scope, message_id, sender_id, part);
}

void bind_messages(py::module_& module) {
    py::enum_<huoxing::MessageKind>(module, "MessageKind")
        .value("Image", huoxing::MessageKind::Image)
        .value("Forward", huoxing::MessageKind::Forward);

    py::enum_<huoxing::HashBasis>(module, "HashBasis")
        .value("DecodedPixels", huoxing::HashBasis::DecodedPixels)
        .value("ForwardResource", huoxing::HashBasis::ForwardResource)
        .value("VisibleCard", huoxing::HashBasis::VisibleCard)
        .value("Occurrence", huoxing::HashBasis::Occurrence);

    py::enum_<huoxing::DetectionStatus>(module, "DetectionStatus")
        .value("New", huoxing::DetectionStatus::New)
        .value("Duplicate", huoxing::DetectionStatus::Duplicate)
        .value("PossibleSameCard", huoxing::DetectionStatus::PossibleSameCard)
        .value("InsufficientEvidence", huoxing::DetectionStatus::InsufficientEvidence);

    py::class_<huoxing::MessageContext>(module, "MessageContext")
        .def(
            py::init(
                [](std::string scope, std::string message_id, std::string sender_id, int part) {
                    return huoxing::MessageContext{
                        std::move(scope),
                        std::move(message_id),
                        std::move(sender_id),
                        part
                    };
                }
            ),
            py::arg("scope"),
            py::arg("message_id"),
            py::arg("sender_id") = "",
            py::arg("part") = 0
        );

    py::class_<huoxing::ImageMessage>(module, "ImageMessage")
        .def(
            py::init([](py::array_t<unsigned char, py::array::c_style> rgb) {
                return huoxing::ImageMessage{copy_rgb_array(rgb)};
            }),
            py::arg("rgb").noconvert()
        );

    py::class_<huoxing::ForwardMessage>(module, "ForwardMessage")
        .def(
            py::init([](std::string resource_namespace,
                        std::string resource_id,
                        std::string visible_card) {
                return huoxing::ForwardMessage{
                    std::move(resource_namespace),
                    std::move(resource_id),
                    std::move(visible_card)
                };
            }),
            py::arg("resource_namespace"),
            py::arg("resource_id"),
            py::arg("visible_card") = ""
        );

    py::class_<huoxing::Message>(module, "Message")
        .def(py::init<huoxing::MessageContext, huoxing::ImageMessage>())
        .def(py::init<huoxing::MessageContext, huoxing::ForwardMessage>());

    py::class_<huoxing::Fingerprint>(module, "Fingerprint")
        .def_readonly("kind", &huoxing::Fingerprint::kind)
        .def_readonly("basis", &huoxing::Fingerprint::basis)
        .def_readonly("value", &huoxing::Fingerprint::value)
        .def_property_readonly("algorithm", [](const huoxing::Fingerprint&) { return "sha256"; })
        .def_property_readonly("version", [](const huoxing::Fingerprint&) { return 1; });

    py::class_<huoxing::FirstOccurrence>(module, "FirstOccurrence")
        .def_readonly("message_id", &huoxing::FirstOccurrence::message_id)
        .def_readonly("sender_id", &huoxing::FirstOccurrence::sender_id)
        .def_readonly("received_at", &huoxing::FirstOccurrence::received_at);

    py::class_<huoxing::Detection>(module, "Detection")
        .def_readonly("status", &huoxing::Detection::status)
        .def_readonly("fingerprint", &huoxing::Detection::fingerprint)
        .def_readonly("replayed", &huoxing::Detection::replayed)
        .def_readonly("evidence", &huoxing::Detection::evidence)
        .def_property_readonly(
            "duplicate",
            [](const huoxing::Detection& result) {
                return result.status == huoxing::DetectionStatus::Duplicate;
            }
        )
        .def_property_readonly(
            "message_hash",
            [](const huoxing::Detection& result) { return result.fingerprint.value; }
        )
        .def_property_readonly(
            "hash_basis",
            [](const huoxing::Detection& result) {
                return std::string{huoxing::basis_name(result.fingerprint.basis)};
            }
        )
        .def_property_readonly(
            "image_id",
            [](const huoxing::Detection& result) {
                return result.storage.kind == huoxing::MessageKind::Image ? result.storage.id : 0;
            }
        )
        .def_property_readonly(
            "first",
            [](const huoxing::Detection& result) -> py::object {
                return result.first ? py::cast(*result.first) : py::none();
            }
        )
        .def_property_readonly(
            "first_message_id",
            [](const huoxing::Detection& result) {
                return result.first ? result.first->message_id : std::string{};
            }
        )
        .def_property_readonly(
            "first_sender_id",
            [](const huoxing::Detection& result) {
                return result.first ? result.first->sender_id : std::string{};
            }
        )
        .def_property_readonly("first_seen", [](const huoxing::Detection& result) {
            return result.first ? result.first->received_at : std::string{};
        });
}

void bind_image_detector(py::module_& module) {
    py::class_<huoxing::Detector>(module, "Detector")
        .def(py::init<std::string, int>(), py::arg("db_path"), py::arg("max_images") = 2000)
        .def("detect", &huoxing::Detector::detect, py::call_guard<py::gil_scoped_release>())
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
    bind_messages(module);
    bind_image_detector(module);
    bind_forward_detector(module);
}
