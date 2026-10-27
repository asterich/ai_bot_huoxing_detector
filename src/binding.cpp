#include "huoxing/core.hpp"
#include <cstring>
#include <pybind11/numpy.h>
#include <pybind11/pybind11.h>
namespace py=pybind11;
PYBIND11_MODULE(_native,m) {
    py::class_<huoxing::ForwardResult>(m,"ForwardResult")
        .def_readonly("status",&huoxing::ForwardResult::status)
        .def_readonly("replayed",&huoxing::ForwardResult::replayed)
        .def_readonly("first_message_id",&huoxing::ForwardResult::first_message_id)
        .def_readonly("first_seen",&huoxing::ForwardResult::first_seen);
    py::class_<huoxing::Evidence>(m,"Evidence")
        .def_readonly("method",&huoxing::Evidence::method)
        .def_readonly("inliers",&huoxing::Evidence::inliers)
        .def_readonly("inlier_ratio",&huoxing::Evidence::inlier_ratio)
        .def_readonly("coverage",&huoxing::Evidence::coverage)
        .def_readonly("pixel_agreement",&huoxing::Evidence::pixel_agreement);
    py::class_<huoxing::Result>(m,"Result")
        .def_readonly("duplicate",&huoxing::Result::duplicate)
        .def_readonly("replayed",&huoxing::Result::replayed)
        .def_readonly("image_id",&huoxing::Result::image_id)
        .def_readonly("first_message_id",&huoxing::Result::first_message_id)
        .def_readonly("first_sender_id",&huoxing::Result::first_sender_id)
        .def_readonly("first_seen",&huoxing::Result::first_seen)
        .def_readonly("evidence",&huoxing::Result::evidence);
    py::class_<huoxing::Detector>(m,"Detector")
        .def(py::init<std::string,int>(),py::arg("db_path"),py::arg("max_images")=2000)
        .def("ingest_rgb",[](const huoxing::Detector& d,
                            py::array_t<unsigned char,py::array::c_style> array,
                            const std::string& scope,const std::string& message,
                            const std::string& sender,int part) {
            auto v=array.request();
            if(v.ndim!=3||v.shape[2]!=3||v.shape[0]<16||v.shape[1]<16||
               v.shape[0]>16000000||v.shape[1]>16000000||v.shape[0]*v.shape[1]>16000000)
                throw std::invalid_argument("Expected contiguous HxWx3 uint8 RGB, <=16M pixels");
            // Own a copy before releasing GIL: caller cannot mutate our input while matching.
            cv::Mat rgb(int(v.shape[0]),int(v.shape[1]),CV_8UC3,v.ptr);
            auto owned=rgb.clone();
            py::gil_scoped_release release;
            return d.ingest(owned,scope,message,sender,part);
        },py::arg("rgb").noconvert(),py::arg("scope"),py::arg("message_id"),
          py::arg("sender_id")="",py::arg("part")=0)
        .def("delete_scope",&huoxing::Detector::delete_scope,py::call_guard<py::gil_scoped_release>())
        .def("ingest_forward",&huoxing::Detector::ingest_forward,
             py::arg("scope"),py::arg("message_id"),py::arg("part"),py::arg("resource_namespace"),
             py::arg("resource_id"),py::arg("visible_card")="",py::call_guard<py::gil_scoped_release>());
}
