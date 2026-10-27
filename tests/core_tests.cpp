#include "huoxing/core.hpp"
#include <atomic>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <opencv2/imgproc.hpp>

void require(bool value,const char* reason) {
    if(!value) throw std::runtime_error(reason);
}
cv::Mat fixture(int seed) {
    cv::Mat im(480,640,CV_8UC3,cv::Scalar(235,230,218));
    cv::RNG rng(seed);
    for(int i=0;i<180;++i) {
        cv::Point p{rng.uniform(10,600),rng.uniform(10,440)};
        cv::circle(im,p,rng.uniform(5,25),
            cv::Scalar(rng.uniform(0,230),rng.uniform(0,230),rng.uniform(0,230)),-1);
        cv::line(im,p,p+cv::Point(15,20),cv::Scalar(0,0,0),2);
    }
    cv::putText(im,"MEME TEST",{40,240},cv::FONT_HERSHEY_SIMPLEX,1,cv::Scalar(0,0,0),2);
    return im;
}
int main() {
    auto path=std::filesystem::temp_directory_path()/
        ("huoxing-test-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())+".sqlite");
    try {
        auto im=fixture(42),f=huoxing::extract(im);
        auto crop=im(cv::Rect(100,70,450,350)).clone();
        auto cf=huoxing::extract(crop);
        require(huoxing::compare(f,cf).has_value(),"crop");
        require(huoxing::compare(cf,f).has_value(),"reverse crop");
        cv::Mat border;
        cv::copyMakeBorder(im,border,40,80,60,100,cv::BORDER_CONSTANT,cv::Scalar(255,255,255));
        require(huoxing::compare(f,huoxing::extract(border)).has_value(),"border");
        auto watermark=im.clone();
        cv::rectangle(watermark,{350,350,270,85},cv::Scalar(255,255,255),-1);
        cv::putText(watermark,"WATERMARK",{360,390},cv::FONT_HERSHEY_SIMPLEX,.5,cv::Scalar(0,0,0));
        require(huoxing::compare(f,huoxing::extract(watermark)).has_value(),"watermark");
        for(int i=0;i<6;++i)
            require(!huoxing::compare(f,huoxing::extract(fixture(i))),"unrelated");
        huoxing::Detector db(path.string());
        auto first=db.ingest(im,"g1","1","alice");
        require(!first.duplicate,"first occurrence");
        auto repeat=db.ingest(im,"g1","1");
        require(repeat.replayed&&!repeat.duplicate,"idempotent first occurrence");
        auto modified=db.ingest(crop,"g1","2");
        require(modified.duplicate&&modified.first_message_id=="1"&&modified.first_sender_id=="alice","metadata");
        require(!db.ingest(im,"g2","1").duplicate,"group isolation");
        std::atomic<int> originals=0, errors=0;
        std::vector<std::jthread> threads;
        for(int i=0;i<4;++i) threads.emplace_back([&,i] {
            try { if(!db.ingest(im,"concurrent",std::to_string(i)).duplicate) ++originals; }
            catch(...) { ++errors; }
        });
        threads.clear();
        require(errors==0&&originals==1,"concurrent ingestion");
        db.delete_scope("g1");
        require(!db.ingest(im,"g1","1").duplicate,"cleanup");
        std::cout<<"C++ tests passed: crop/border/watermark/negatives/storage/concurrency\n";
    } catch(const std::exception& e) { std::cerr<<e.what()<<'\n';return 1; }
    std::filesystem::remove(path);
    std::filesystem::remove(path.string()+"-wal");
    std::filesystem::remove(path.string()+"-shm");
}
