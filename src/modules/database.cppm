module;
#include "schema.hpp"
#include <bit>
#include <cstring>
#include <filesystem>
#include <functional>
#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>
#include <optional>
#include <sqlite3.h>
#include <stdexcept>
#include <string>
#include <vector>

export module huoxing.database;
import huoxing.types;

// This internal module owns SQLite handles, statements, transactions and blob decoding.
export namespace huoxing::storage {
static_assert(std::endian::native == std::endian::little, "Database v1 uses little endian floats");
struct Connection {
    sqlite3* p = nullptr;
    explicit Connection(const std::string& path) {
        if (sqlite3_open(path.c_str(), &p) != SQLITE_OK) {
            std::string error = p ? sqlite3_errmsg(p) : "sqlite open failed";
            if (p)
                sqlite3_close(p);
            throw std::runtime_error(error);
        }
        sqlite3_busy_timeout(p, 30000);
        try {
            exec("PRAGMA foreign_keys=ON");
        } catch (...) {
            sqlite3_close(p);
            throw;
        }
    }
    ~Connection() {
        sqlite3_close(p);
    }
    Connection(const Connection&) = delete;
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
    explicit Statement(Connection& db, const char* sql) {
        if (sqlite3_prepare_v2(db.p, sql, -1, &p, nullptr) != SQLITE_OK)
            throw std::runtime_error(sqlite3_errmsg(db.p));
    }
    ~Statement() {
        sqlite3_finalize(p);
    }
    Statement(const Statement&) = delete;
    void check(int rc) {
        if (rc != SQLITE_OK)
            throw std::runtime_error(sqlite3_errmsg(sqlite3_db_handle(p)));
    }
    void bind(int n, const std::string& v) {
        check(sqlite3_bind_text(p, n, v.data(), int(v.size()), SQLITE_TRANSIENT));
    }
    void bind(int n, std::int64_t v) {
        check(sqlite3_bind_int64(p, n, v));
    }
    void bind(int n, double v) {
        check(sqlite3_bind_double(p, n, v));
    }
    void blob(int n, const void* data, int bytes) {
        if (bytes == 0)
            check(sqlite3_bind_zeroblob(p, n, 0));
        else
            check(sqlite3_bind_blob(p, n, data, bytes, SQLITE_TRANSIENT));
    }
    bool step() {
        int rc = sqlite3_step(p);
        if (rc == SQLITE_ROW)
            return true;
        if (rc == SQLITE_DONE)
            return false;
        throw std::runtime_error(sqlite3_errmsg(sqlite3_db_handle(p)));
    }
    std::int64_t integer(int col) {
        return sqlite3_column_int64(p, col);
    }
    double number(int col) {
        return sqlite3_column_double(p, col);
    }
    std::string text(int col) {
        auto v = sqlite3_column_text(p, col);
        return v ? reinterpret_cast<const char*>(v) : "";
    }
};
struct Transaction {
    Connection& db;
    bool committed = false;
    explicit Transaction(Connection& d) : db(d) {
        db.exec("BEGIN IMMEDIATE");
    }
    ~Transaction() {
        if (!committed)
            sqlite3_exec(db.p, "ROLLBACK", nullptr, nullptr, nullptr);
    }
    void commit() {
        db.exec("COMMIT");
        committed = true;
    }
};
Feature load(Statement& q) { // SELECT id,digest,gray,points,descriptors,npoints
    Feature f;
    f.digest = q.text(1);
    const int n = int(q.integer(5));
    if (n < 0 || n > 10000)
        throw std::runtime_error("Corrupt feature count");
    auto read = [&](int col, int rows, int cols, int type) {
        cv::Mat m(rows, cols, type);
        int size = sqlite3_column_bytes(q.p, col);
        if (std::size_t(size) != m.total() * m.elemSize())
            throw std::runtime_error("Corrupt feature blob");
        if (size)
            std::memcpy(m.data, sqlite3_column_blob(q.p, col), size);
        return m;
    };
    int size = sqlite3_column_bytes(q.p, 2);
    const auto* blob = static_cast<const unsigned char*>(sqlite3_column_blob(q.p, 2));
    if (size <= 0 || !blob)
        throw std::runtime_error("Corrupt gray blob");
    f.gray = cv::imdecode(std::vector<unsigned char>(blob, blob + size), cv::IMREAD_GRAYSCALE);
    if (f.gray.empty() || f.gray.total() > 960 * 960)
        throw std::runtime_error("Corrupt gray image");
    f.points = read(3, n, 1, CV_32FC2);
    f.descriptors = read(4, n, 128, CV_32F);
    return f;
}
void fill_first(Connection& db, Result& r, std::int64_t image) {
    Statement q(db, "SELECT message_id,sender_id,received_at FROM occurrences WHERE image_id=? "
                    "ORDER BY id LIMIT 1");
    q.bind(1, image);
    if (q.step()) {
        r.first_message_id = q.text(0);
        r.first_sender_id = q.text(1);
        r.first_seen = q.text(2);
    }
}

// Keeps all image SQL and transaction ownership behind a repository boundary.
class ImageRepository {
  public:
    explicit ImageRepository(const std::string& path)
        : connection_(path), transaction_(connection_) {}

    static void initialize(const std::string& path) {
        auto parent = std::filesystem::path(path).parent_path();
        if (!parent.empty()) {
            std::filesystem::create_directories(parent);
        }
        Connection connection(path);
        connection.exec("PRAGMA journal_mode=WAL");
        Transaction transaction(connection);
        connection.exec(schema_sql);
        Statement version(connection, "SELECT value FROM meta WHERE key='version'");
        if (!version.step() || version.text(0) != "cpp26-sift-gray960-v1-le") {
            throw std::runtime_error("Incompatible database version");
        }
        transaction.commit();
    }

    std::optional<Result> find_replay(const std::string& scope, const std::string& message,
                                      int part) {
        Statement query(
            connection_,
            "SELECT image_id,matched_image_id,method,inliers,inlier_ratio,coverage,pixel_agreement "
            "FROM occurrences WHERE scope=? AND message_id=? AND part=?");
        query.bind(1, scope);
        query.bind(2, message);
        query.bind(3, std::int64_t(part));
        if (!query.step()) {
            return {};
        }
        Result result;
        result.image_id = query.integer(0);
        result.duplicate = query.integer(1) != 0;
        result.replayed = true;
        result.evidence = {query.text(2), int(query.integer(3)), query.number(4), query.number(5),
                           query.number(6)};
        if (result.duplicate) {
            fill_first(connection_, result, query.integer(1));
        }
        return result;
    }

    std::int64_t find_exact(const std::string& scope, const std::string& digest) {
        Statement query(connection_, "SELECT id FROM images WHERE scope=? AND digest=?");
        query.bind(1, scope);
        query.bind(2, digest);
        return query.step() ? query.integer(0) : 0;
    }

    // Streams candidates so a large group does not materialize every descriptor in RAM.
    void visit_candidates(const std::string& scope,
                          const std::function<bool(std::int64_t, const Feature&)>& visitor) {
        Statement query(connection_, "SELECT id,digest,gray,points,descriptors,npoints FROM images "
                                     "WHERE scope=? ORDER BY id");
        query.bind(1, scope);
        while (query.step()) {
            auto feature = load(query);
            if (!visitor(query.integer(0), feature)) {
                break;
            }
        }
    }

    std::int64_t insert_image(const std::string& scope, const Feature& feature, int capacity) {
        Statement count(connection_, "SELECT COUNT(*) FROM images WHERE scope=?");
        count.bind(1, scope);
        count.step();
        if (count.integer(0) >= capacity) {
            throw std::runtime_error("Scope capacity full; nothing recorded");
        }
        std::vector<unsigned char> gray;
        if (!cv::imencode(".png", feature.gray, gray)) {
            throw std::runtime_error("Gray encode failed");
        }
        Statement insert(
            connection_,
            "INSERT INTO images(scope,digest,gray,points,descriptors,npoints) VALUES(?,?,?,?,?,?)");
        insert.bind(1, scope);
        insert.bind(2, feature.digest);
        insert.blob(3, gray.data(), int(gray.size()));
        insert.blob(4, feature.points.data,
                    int(feature.points.total() * feature.points.elemSize()));
        insert.blob(5, feature.descriptors.data,
                    int(feature.descriptors.total() * feature.descriptors.elemSize()));
        insert.bind(6, std::int64_t(feature.points.rows));
        insert.step();
        return sqlite3_last_insert_rowid(connection_.p);
    }

    void attach_first_occurrence(Result& result, std::int64_t image) {
        fill_first(connection_, result, image);
    }

    void insert_occurrence(const std::string& scope, const std::string& message,
                           const std::string& sender, int part, const Result& result,
                           std::int64_t matched_image) {
        Statement insert(
            connection_,
            "INSERT INTO occurrences(scope,message_id,part,sender_id,image_id,matched_image_id,"
            "method,inliers,inlier_ratio,coverage,pixel_agreement) "
            "VALUES(?,?,?,?,?,NULLIF(?,0),?,?,?,?,?)");
        insert.bind(1, scope);
        insert.bind(2, message);
        insert.bind(3, std::int64_t(part));
        insert.bind(4, sender);
        insert.bind(5, result.image_id);
        insert.bind(6, matched_image);
        insert.bind(7, result.evidence.method);
        insert.bind(8, std::int64_t(result.evidence.inliers));
        insert.bind(9, result.evidence.inlier_ratio);
        insert.bind(10, result.evidence.coverage);
        insert.bind(11, result.evidence.pixel_agreement);
        insert.step();
    }

    void delete_scope(const std::string& scope) {
        Statement remove(connection_, "DELETE FROM images WHERE scope=?");
        remove.bind(1, scope);
        remove.step();
    }

    void commit() {
        transaction_.commit();
    }

  private:
    Connection connection_;
    Transaction transaction_;
};
} // namespace huoxing::storage
