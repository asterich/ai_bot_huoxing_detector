module;
#include "schema.hpp"

export module huoxing.database;
import std;
import huoxing.format;
import huoxing.vision;
import huoxing.types;
import huoxing.sqlite;

namespace {

static_assert(std::endian::native == std::endian::little, "Database v1 uses little endian floats");

huoxing::Feature decode_feature(huoxing::storage::Statement& row) {
    huoxing::Feature feature;
    feature.digest = row.text(1);
    const auto point_count = static_cast<int>(row.integer(5));
    if (point_count < 0 || point_count > 10000) {
        throw std::runtime_error(fmt::format("Corrupt feature count: {}", point_count));
    }

    // A column view must be copied before advancing the SQLite cursor.
    const auto read_matrix = [&](int column, int rows, int columns, int type) {
        cv::Mat matrix(rows, columns, type);
        const auto bytes = row.blob(column);
        const auto expected = matrix.total() * matrix.elemSize();
        if (bytes.size() != expected) {
            throw std::runtime_error(
                fmt::format(
                    "Corrupt feature blob: column {}, expected {} bytes, got {}",
                    column,
                    expected,
                    bytes.size()
                )
            );
        }
        if (!bytes.empty()) {
            std::memcpy(matrix.data, bytes.data(), bytes.size());
        }

        return matrix;
    };

    const auto encoded = row.blob(2);
    if (encoded.empty()) {
        throw std::runtime_error("Corrupt gray blob");
    }
    const auto* begin = reinterpret_cast<const unsigned char*>(encoded.data());
    const std::vector<unsigned char> gray_bytes(begin, begin + encoded.size());
    feature.gray = cv::imdecode(gray_bytes, cv::IMREAD_GRAYSCALE);
    if (feature.gray.empty() || feature.gray.total() > 960 * 960) {
        throw std::runtime_error("Corrupt gray image");
    }

    feature.points = read_matrix(3, point_count, 1, huoxing::vision::point_type);
    feature.descriptors = read_matrix(4, point_count, 128, huoxing::vision::descriptor_type);
    return feature;
}

void read_first_occurrence(
    huoxing::storage::Connection& connection,
    huoxing::Result& result,
    std::int64_t image_id
) {
    huoxing::storage::Statement query(
        connection,
        "SELECT message_id,sender_id,received_at FROM occurrences WHERE image_id=? ORDER BY id "
        "LIMIT 1"
    );
    query.bind(1, image_id);
    if (query.step()) {
        result.first_message_id = query.text(0);
        result.first_sender_id = query.text(1);
        result.first_seen = query.text(2);
    }
}

} // namespace

export namespace huoxing::storage {

// Keeps all image SQL and transaction ownership behind a repository boundary.
class ImageRepository {
  public:

    explicit ImageRepository(std::string_view path)
        : connection_(path), transaction_(connection_) {}

    static void initialize(std::string_view path) {
        auto parent = std::filesystem::path(path).parent_path();
        if (!parent.empty()) {
            std::filesystem::create_directories(parent);
        }
        Connection connection(path);
        connection.execute("PRAGMA journal_mode=WAL");
        Transaction transaction(connection);
        connection.execute(schema_sql);
        Statement version(connection, "SELECT value FROM meta WHERE key='version'");
        if (!version.step() || version.text(0) != "cpp26-sift-gray960-v1-le") {
            throw std::runtime_error("Incompatible database version");
        }
        transaction.commit();
    }

    std::optional<Result> find_replay(std::string_view scope, std::string_view message, int part) {
        Statement query(
            connection_,
            "SELECT image_id,matched_image_id,method,inliers,inlier_ratio,coverage,pixel_agreement "
            "FROM occurrences WHERE scope=? AND message_id=? AND part=?"
        );
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
        result.evidence = {
            query.text(2),
            int(query.integer(3)),
            query.number(4),
            query.number(5),
            query.number(6)
        };
        if (result.duplicate) {
            read_first_occurrence(connection_, result, query.integer(1));
        }
        return result;
    }

    std::int64_t find_exact(std::string_view scope, std::string_view digest) {
        Statement query(connection_, "SELECT id FROM images WHERE scope=? AND digest=?");
        query.bind(1, scope);
        query.bind(2, digest);
        return query.step() ? query.integer(0) : 0;
    }

    // Streams candidates so a large group does not materialize every descriptor in RAM.
    void visit_candidates(
        std::string_view scope,
        const std::function<bool(std::int64_t, const Feature&)>& visitor
    ) {
        Statement query(
            connection_,
            "SELECT id,digest,gray,points,descriptors,npoints FROM images "
            "WHERE scope=? ORDER BY id"
        );
        query.bind(1, scope);
        while (query.step()) {
            auto feature = decode_feature(query);
            if (!visitor(query.integer(0), feature)) {
                break;
            }
        }
    }

    std::int64_t insert_image(std::string_view scope, const Feature& feature, int capacity) {
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
            "INSERT INTO images(scope,digest,gray,points,descriptors,npoints) VALUES(?,?,?,?,?,?)"
        );
        insert.bind(1, scope);
        insert.bind(2, feature.digest);
        insert.bind_blob(3, std::as_bytes(std::span{gray}));
        insert.bind_blob(
            4,
            std::span{
                reinterpret_cast<const std::byte*>(feature.points.data),
                feature.points.total() * feature.points.elemSize()
            }
        );
        insert.bind_blob(
            5,
            std::span{
                reinterpret_cast<const std::byte*>(feature.descriptors.data),
                feature.descriptors.total() * feature.descriptors.elemSize()
            }
        );
        insert.bind(6, std::int64_t(feature.points.rows));
        insert.step();
        return connection_.last_insert_id();
    }

    void attach_first_occurrence(Result& result, std::int64_t image) {
        read_first_occurrence(connection_, result, image);
    }

    void insert_occurrence(
        std::string_view scope,
        std::string_view message,
        std::string_view sender,
        int part,
        const Result& result,
        std::int64_t matched_image
    ) {
        Statement insert(
            connection_,
            "INSERT INTO occurrences(scope,message_id,part,sender_id,image_id,matched_image_id,"
            "method,inliers,inlier_ratio,coverage,pixel_agreement) "
            "VALUES(?,?,?,?,?,NULLIF(?,0),?,?,?,?,?)"
        );
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

    void delete_scope(std::string_view scope) {
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
