#include "huoxing/core.hpp"
#include "schema.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <iomanip>
#include <memory>
#include <set>
#include <sstream>
#include <stdexcept>
#include <vector>
#include <openssl/evp.h>
#include <sqlite3.h>
#include <opencv2/calib3d.hpp>
#include <opencv2/features2d.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

namespace huoxing {
namespace {
static_assert(std::endian::native == std::endian::little, "Database v1 uses little endian floats");
struct Db {
    sqlite3* p = nullptr;
    explicit Db(const std::string& path) {
        if (sqlite3_open(path.c_str(), &p) != SQLITE_OK) {
            std::string error = p ? sqlite3_errmsg(p) : "sqlite open failed";
            if (p) sqlite3_close(p);
            throw std::runtime_error(error);
        }
        sqlite3_busy_timeout(p, 30000);
        try { exec("PRAGMA foreign_keys=ON"); } catch (...) { sqlite3_close(p); throw; }
    }
    ~Db() { sqlite3_close(p); }
    Db(const Db&) = delete;
    void exec(const char* sql) {
        char* error = nullptr;
        if (sqlite3_exec(p, sql, nullptr, nullptr, &error) != SQLITE_OK) {
            std::string text = error ? error : sqlite3_errmsg(p);
            sqlite3_free(error);
            throw std::runtime_error(text);
        }
    }
};
struct Statement {
    sqlite3_stmt* p = nullptr;
    explicit Statement(Db& db, const char* sql) {
        if (sqlite3_prepare_v2(db.p, sql, -1, &p, nullptr) != SQLITE_OK)
            throw std::runtime_error(sqlite3_errmsg(db.p));
    }
    ~Statement() { sqlite3_finalize(p); }
    Statement(const Statement&) = delete;
    void check(int rc) {
        if (rc != SQLITE_OK) throw std::runtime_error(sqlite3_errmsg(sqlite3_db_handle(p)));
    }
    void bind(int n, const std::string& v) { check(sqlite3_bind_text(p,n,v.data(),int(v.size()),SQLITE_TRANSIENT)); }
    void bind(int n, std::int64_t v) { check(sqlite3_bind_int64(p,n,v)); }
    void bind(int n, double v) { check(sqlite3_bind_double(p,n,v)); }
    void blob(int n, const void* data, int bytes) {
        if (bytes == 0) check(sqlite3_bind_zeroblob(p,n,0));
        else check(sqlite3_bind_blob(p,n,data,bytes,SQLITE_TRANSIENT));
    }
    bool step() {
        int rc = sqlite3_step(p);
        if (rc == SQLITE_ROW) return true;
        if (rc == SQLITE_DONE) return false;
        throw std::runtime_error(sqlite3_errmsg(sqlite3_db_handle(p)));
    }
    std::int64_t integer(int col) { return sqlite3_column_int64(p,col); }
    double number(int col) { return sqlite3_column_double(p,col); }
    std::string text(int col) {
        auto v = sqlite3_column_text(p,col);
        return v ? reinterpret_cast<const char*>(v) : "";
    }
};
struct Transaction {
    Db& db;
    bool committed = false;
    explicit Transaction(Db& d): db(d) { db.exec("BEGIN IMMEDIATE"); }
    ~Transaction() { if (!committed) sqlite3_exec(db.p,"ROLLBACK",nullptr,nullptr,nullptr); }
    void commit() { db.exec("COMMIT"); committed = true; }
};
std::string digest(const cv::Mat& rgb) {
    std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> ctx(EVP_MD_CTX_new(), EVP_MD_CTX_free);
    auto header = std::to_string(rgb.cols)+"x"+std::to_string(rgb.rows)+":";
    std::array<unsigned char,EVP_MAX_MD_SIZE> bytes{};
    unsigned int size = 0;
    if (!ctx || EVP_DigestInit_ex(ctx.get(),EVP_sha256(),nullptr)!=1 ||
        EVP_DigestUpdate(ctx.get(),header.data(),header.size())!=1)
        throw std::runtime_error("SHA256 initialization failed");
    for (int y=0; y<rgb.rows; ++y)
        if (EVP_DigestUpdate(ctx.get(),rgb.ptr(y),std::size_t(rgb.cols)*3)!=1)
            throw std::runtime_error("SHA256 update failed");
    if (EVP_DigestFinal_ex(ctx.get(),bytes.data(),&size)!=1)
        throw std::runtime_error("SHA256 final failed");
    std::ostringstream out;
    for (unsigned int i=0;i<size;++i) out<<std::hex<<std::setw(2)<<std::setfill('0')<<int(bytes[i]);
    return out.str();
}
std::vector<cv::Point2f> points(const cv::Mat& m) {
    std::vector<cv::Point2f> out;
    for (int i=0;i<m.rows;++i) { auto v=m.at<cv::Vec2f>(i); out.emplace_back(v[0],v[1]); }
    return out;
}
double coverage(const std::vector<cv::Point2f>& p, const cv::Mat& gray) {
    if (p.size()<3) return 0;
    std::vector<cv::Point2f> hull;
    cv::convexHull(p,hull);
    return std::abs(cv::contourArea(hull))/double(gray.total());
}
Feature load(Statement& q) { // SELECT id,digest,gray,points,descriptors,npoints
    Feature f;
    f.digest=q.text(1);
    const int n=int(q.integer(5));
    if(n<0 || n>10000) throw std::runtime_error("Corrupt feature count");
    auto read = [&](int col,int rows,int cols,int type) {
        cv::Mat m(rows,cols,type);
        int size=sqlite3_column_bytes(q.p,col);
        if (std::size_t(size)!=m.total()*m.elemSize()) throw std::runtime_error("Corrupt feature blob");
        if (size) std::memcpy(m.data,sqlite3_column_blob(q.p,col),size);
        return m;
    };
    int size=sqlite3_column_bytes(q.p,2);
    const auto* blob=static_cast<const unsigned char*>(sqlite3_column_blob(q.p,2));
    if(size<=0 || !blob) throw std::runtime_error("Corrupt gray blob");
    f.gray=cv::imdecode(std::vector<unsigned char>(blob,blob+size),cv::IMREAD_GRAYSCALE);
    if(f.gray.empty() || f.gray.total()>960*960) throw std::runtime_error("Corrupt gray image");
    f.points=read(3,n,1,CV_32FC2);
    f.descriptors=read(4,n,128,CV_32F);
    return f;
}
void fill_first(Db& db, Result& r, std::int64_t image) {
    Statement q(db,"SELECT message_id,sender_id,received_at FROM occurrences WHERE image_id=? ORDER BY id LIMIT 1");
    q.bind(1,image);
    if(q.step()) { r.first_message_id=q.text(0); r.first_sender_id=q.text(1); r.first_seen=q.text(2); }
}
}

Feature extract(const cv::Mat& rgb) {
    if(rgb.type()!=CV_8UC3 || rgb.rows<16 || rgb.cols<16 || rgb.total()>16000000)
        throw std::invalid_argument("Expected RGB uint8, dimensions >=16 and <=16M pixels");
    Feature f;
    f.digest=digest(rgb);
    cv::Mat small=rgb;
    if(std::max(rgb.rows,rgb.cols)>960) {
        double scale=960.0/std::max(rgb.rows,rgb.cols);
        cv::resize(rgb,small,{},scale,scale,cv::INTER_AREA);
    }
    cv::cvtColor(small,f.gray,cv::COLOR_RGB2GRAY);
    std::vector<cv::KeyPoint> kp;
    cv::SIFT::create(1000)->detectAndCompute(f.gray,cv::noArray(),kp,f.descriptors);
    f.points=cv::Mat(int(kp.size()),1,CV_32FC2);
    for(int i=0;i<int(kp.size());++i) f.points.at<cv::Vec2f>(i)={kp[i].pt.x,kp[i].pt.y};
    if(f.descriptors.empty()) f.descriptors=cv::Mat(0,128,CV_32F);
    return f;
}

std::optional<Evidence> compare(const Feature& a,const Feature& b) {
    if(a.digest==b.digest) return Evidence{"decoded_pixels"};
    if(std::min(a.descriptors.rows,b.descriptors.rows)<12) return {};
    cv::BFMatcher matcher(cv::NORM_L2);
    std::vector<std::vector<cv::DMatch>> forward,reverse;
    matcher.knnMatch(a.descriptors,b.descriptors,forward,2);
    matcher.knnMatch(b.descriptors,a.descriptors,reverse,2);
    std::set<std::pair<int,int>> rev;
    for(const auto& pair:reverse)
        if(pair.size()==2 && pair[0].distance<0.72f*pair[1].distance)
            rev.emplace(pair[0].trainIdx,pair[0].queryIdx);
    auto ap=points(a.points),bp=points(b.points);
    std::vector<cv::Point2f> src,dst;
    std::set<std::array<int,4>> unique;
    for(const auto& pair:forward) {
        if(pair.size()!=2) continue;
        auto m=pair[0];
        if(m.distance>=0.72f*pair[1].distance || !rev.contains({m.queryIdx,m.trainIdx})) continue;
        auto p=ap[m.queryIdx],q=bp[m.trainIdx];
        if(unique.insert({cvRound(p.x),cvRound(p.y),cvRound(q.x),cvRound(q.y)}).second) {
            src.push_back(p); dst.push_back(q);
        }
    }
    if(src.size()<12) return {};
    cv::Mat mask;
    auto h=cv::findHomography(src,dst,cv::RANSAC,3.0,mask,2000,0.995);
    if(h.empty() || !cv::checkRange(h)) return {};
    std::vector<cv::Point2f> si,di;
    for(int i=0;i<int(src.size());++i) if(mask.at<unsigned char>(i)) {si.push_back(src[i]);di.push_back(dst[i]);}
    int count=int(si.size());
    double ratio=double(count)/src.size(), cov=std::max(coverage(si,a.gray),coverage(di,b.gray));
    if(count<12 || ratio<0.55 || cov<0.20) return {};
    std::vector<cv::Point2f> corners{{0,0},{float(a.gray.cols),0},
        {float(a.gray.cols),float(a.gray.rows)},{0,float(a.gray.rows)}},mapped;
    cv::perspectiveTransform(corners,mapped,h);
    for(auto p:mapped) if(!std::isfinite(p.x)||!std::isfinite(p.y)) return {};
    double area=std::abs(cv::contourArea(mapped))/b.gray.total();
    if(!cv::isContourConvex(mapped)||area<0.02||area>50) return {};
    cv::Mat aligned,valid,aa,bb,diff,agree;
    cv::warpPerspective(a.gray,aligned,h,b.gray.size());
    cv::warpPerspective(cv::Mat(a.gray.size(),CV_8U,cv::Scalar(255)),valid,h,b.gray.size());
    cv::erode(valid,valid,cv::Mat::ones(5,5,CV_8U));
    cv::threshold(valid,valid,250,255,cv::THRESH_BINARY);
    int pixels=cv::countNonZero(valid);
    if(pixels<1024 || double(pixels)/valid.total()<0.15) return {};
    cv::GaussianBlur(aligned,aa,{3,3},0);
    cv::GaussianBlur(b.gray,bb,{3,3},0);
    cv::absdiff(aa,bb,diff);
    cv::compare(diff,32,agree,cv::CMP_LT);
    cv::bitwise_and(agree,valid,agree);
    double agreement=double(cv::countNonZero(agree))/pixels;
    if(agreement<0.72) return {};
    return Evidence{"sift_ransac_pixels",count,ratio,cov,agreement};
}

Detector::Detector(std::string path,int max_images):path_(std::move(path)),max_images_(max_images) {
    if(path_.empty() || path_==":memory:" || max_images_<1) throw std::invalid_argument("Need file database and positive capacity");
    auto parent=std::filesystem::path(path_).parent_path();
    if(!parent.empty()) std::filesystem::create_directories(parent);
    Db db(path_);
    db.exec("PRAGMA journal_mode=WAL");
    Transaction tx(db);
    db.exec(schema_sql);
    Statement q(db,"SELECT value FROM meta WHERE key='version'");
    if(!q.step() || q.text(0)!="cpp26-sift-gray960-v1-le") throw std::runtime_error("Incompatible database version");
    tx.commit();
}

Result Detector::ingest(const cv::Mat& rgb,const std::string& scope,const std::string& message,
                        const std::string& sender,int part) const {
    if(scope.empty()||message.empty()||part<0) throw std::invalid_argument("scope/message_id required; part >=0");
    auto f=extract(rgb);
    Db db(path_);
    Transaction tx(db);
    Result result;
    {
        Statement old(db,"SELECT image_id,matched_image_id,method,inliers,inlier_ratio,coverage,pixel_agreement FROM occurrences WHERE scope=? AND message_id=? AND part=?");
        old.bind(1,scope);old.bind(2,message);old.bind(3,std::int64_t(part));
        if(old.step()) {
            result.image_id=old.integer(0); result.duplicate=old.integer(1)!=0; result.replayed=true;
            result.evidence={old.text(2),int(old.integer(3)),old.number(4),old.number(5),old.number(6)};
            if(result.duplicate) fill_first(db,result,old.integer(1));
            return result;
        }
    }
    std::int64_t matched=0,existing=0;
    {
        Statement exact(db,"SELECT id FROM images WHERE scope=? AND digest=?");
        exact.bind(1,scope);exact.bind(2,f.digest);
        if(exact.step()) {matched=existing=exact.integer(0);result.evidence.method="decoded_pixels";}
    }
    if(!matched) {
        Statement rows(db,"SELECT id,digest,gray,points,descriptors,npoints FROM images WHERE scope=? ORDER BY id");
        rows.bind(1,scope);
        while(rows.step()) {
            auto e=compare(f,load(rows));
            if(e) {matched=rows.integer(0);result.evidence=*e;break;}
        }
    }
    if(existing) result.image_id=existing;
    else {
        Statement n(db,"SELECT COUNT(*) FROM images WHERE scope=?");n.bind(1,scope);n.step();
        if(n.integer(0)>=max_images_) throw std::runtime_error("Scope capacity full; nothing recorded");
        std::vector<unsigned char> gray;
        if(!cv::imencode(".png",f.gray,gray)) throw std::runtime_error("Gray encode failed");
        Statement add(db,"INSERT INTO images(scope,digest,gray,points,descriptors,npoints) VALUES(?,?,?,?,?,?)");
        add.bind(1,scope);add.bind(2,f.digest);add.blob(3,gray.data(),int(gray.size()));
        add.blob(4,f.points.data,int(f.points.total()*f.points.elemSize()));
        add.blob(5,f.descriptors.data,int(f.descriptors.total()*f.descriptors.elemSize()));
        add.bind(6,std::int64_t(f.points.rows));add.step();
        result.image_id=sqlite3_last_insert_rowid(db.p);
    }
    result.duplicate=matched!=0;
    if(matched) fill_first(db,result,matched);
    Statement add(db,"INSERT INTO occurrences(scope,message_id,part,sender_id,image_id,matched_image_id,method,inliers,inlier_ratio,coverage,pixel_agreement) VALUES(?,?,?,?,?,NULLIF(?,0),?,?,?,?,?)");
    add.bind(1,scope);add.bind(2,message);add.bind(3,std::int64_t(part));add.bind(4,sender);
    add.bind(5,result.image_id);add.bind(6,matched);add.bind(7,result.evidence.method);
    add.bind(8,std::int64_t(result.evidence.inliers));add.bind(9,result.evidence.inlier_ratio);
    add.bind(10,result.evidence.coverage);add.bind(11,result.evidence.pixel_agreement);add.step();
    tx.commit();
    return result;
}
void Detector::delete_scope(const std::string& scope) const {
    Db db(path_);Transaction tx(db);
    Statement q(db,"DELETE FROM images WHERE scope=?");q.bind(1,scope);q.step();tx.commit();
}
}
