
import std;
import huoxing.format;
import huoxing.vision;
import huoxing;
import huoxing.hashing;
import huoxing.sqlite;
import huoxing.repository_sqlite;

namespace {

void require(bool condition, std::string_view reason) {
    if (!condition) {
        throw std::runtime_error(fmt::format("Test failed: {}", reason));
    }
}

class TemporaryDatabase {
  public:

    TemporaryDatabase()
        : path_(
              std::filesystem::temp_directory_path()
              / fmt::format(
                  "huoxing-test-{}.sqlite",
                  std::chrono::steady_clock::now().time_since_epoch().count()
              )
          ) {}

    ~TemporaryDatabase() {
        // Cleanup must still run if a test throws; never throw from this destructor.
        std::error_code ignored;
        for (const std::string_view suffix : {"", "-wal", "-shm"}) {
            std::filesystem::remove(fmt::format("{}{}", path_.string(), suffix), ignored);
        }
    }

    std::string path() const {
        return path_.string();
    }

  private:

    std::filesystem::path path_;
};

cv::Mat fixture(int seed) {
    cv::Mat image(480, 640, huoxing::vision::rgb_type, cv::Scalar(235, 230, 218));
    cv::RNG random(seed);
    for ([[maybe_unused]] const auto index : std::views::iota(0, 180)) {
        const cv::Point location{random.uniform(10, 600), random.uniform(10, 440)};
        cv::circle(
            image,
            location,
            random.uniform(5, 25),
            cv::Scalar(random.uniform(0, 230), random.uniform(0, 230), random.uniform(0, 230)),
            -1
        );
        const cv::Point line_end{location.x + 15, location.y + 20};
        cv::line(image, location, line_end, cv::Scalar(0, 0, 0), 2);
    }
    cv::putText(image, "MEME TEST", {40, 240}, cv::FONT_HERSHEY_SIMPLEX, 1, cv::Scalar(0, 0, 0), 2);
    return image;
}

void test_image_matching() {
    const auto original = fixture(42);
    const auto feature = huoxing::extract(original);
    const auto cropped = original(cv::Rect(100, 70, 450, 350)).clone();
    const auto cropped_feature = huoxing::extract(cropped);
    require(huoxing::compare(feature, cropped_feature).has_value(), "crop");
    require(huoxing::compare(cropped_feature, feature).has_value(), "reverse crop");

    cv::Mat framed;
    cv::copyMakeBorder(
        original,
        framed,
        40,
        80,
        60,
        100,
        cv::BORDER_CONSTANT,
        cv::Scalar(255, 255, 255)
    );
    require(huoxing::compare(feature, huoxing::extract(framed)).has_value(), "border");

    auto watermarked = original.clone();
    cv::rectangle(watermarked, {350, 350, 270, 85}, cv::Scalar(255, 255, 255), -1);
    cv::putText(
        watermarked,
        "WATERMARK",
        {360, 390},
        cv::FONT_HERSHEY_SIMPLEX,
        .5,
        cv::Scalar(0, 0, 0)
    );
    require(huoxing::compare(feature, huoxing::extract(watermarked)).has_value(), "watermark");

    for (const auto seed : std::views::iota(0, 6)) {
        require(!huoxing::compare(feature, huoxing::extract(fixture(seed))), "unrelated image");
    }
}

void test_image_storage() {
    TemporaryDatabase file;
    huoxing::Detector detector(file.path());
    const auto image = fixture(42);
    require(!detector.ingest(image, "group", "1", "alice").duplicate, "first occurrence");

    const auto replay = detector.ingest(image, "group", "1");
    require(replay.replayed && !replay.duplicate, "idempotent registration");

    const auto cropped = image(cv::Rect(100, 70, 450, 350)).clone();
    const auto match = detector.ingest(cropped, "group", "2");
    require(
        match.duplicate && match.first_message_id == "1" && match.first_sender_id == "alice",
        "first occurrence metadata"
    );
    require(!detector.ingest(image, "other", "1").duplicate, "group isolation");

    detector.delete_scope("group");
    require(!detector.ingest(image, "group", "1").duplicate, "scope cleanup");
}

void test_concurrent_registration() {
    TemporaryDatabase file;
    huoxing::Detector detector(file.path());
    const auto image = fixture(42);
    std::atomic<int> originals = 0;
    std::atomic<int> errors = 0;
    std::vector<std::jthread> threads;

    for (const auto index : std::views::iota(0, 4)) {
        threads.emplace_back([&, message_id = fmt::format("{}", index)] {
            try {
                if (!detector.ingest(image, "concurrent", message_id).duplicate) {
                    ++originals;
                }
            } catch (...) {
                ++errors;
            }
        });
    }
    threads.clear(); // jthread joins before the assertions read the counters.

    require(errors == 0 && originals == 1, "atomic concurrent registration");
}

void test_injected_interfaces() {
    static_assert(std::is_abstract_v<huoxing::IDetector>);
    static_assert(std::is_abstract_v<huoxing::Repository>);
    static_assert(std::is_abstract_v<huoxing::RepositorySession>);

    TemporaryDatabase file;
    std::shared_ptr<huoxing::Repository> repository =
        std::make_shared<huoxing::SqliteRepository>(file.path());
    std::shared_ptr<huoxing::IDetector> detector = std::make_shared<huoxing::Detector>(repository);

    // Both value types enter the same virtual interface with an injected backend.
    const auto image = detector->detect(
        huoxing::Message{{"group", "image", "alice", 0}, huoxing::ImageMessage{fixture(42)}}
    );
    const auto forward = detector->detect(
        huoxing::Message{
            {"group", "forward", "bob", 0},
            huoxing::ForwardMessage{"onebot11:forward-id", "A", ""}
        }
    );
    require(image.status == huoxing::DetectionStatus::New, "image through IDetector");
    require(forward.status == huoxing::DetectionStatus::New, "forward through IDetector");

    // The legacy facade can share this exact coordinator rather than open its own.
    huoxing::ForwardDetector facade(detector);
    const auto repeated = facade.ingest("group", "forward-2", 0, "onebot11:forward-id", "A");
    require(repeated.status == "same_resource", "facade delegates to injected detector");
    detector->delete_scope("group");
    require(
        facade.ingest("group", "forward-3", 0, "onebot11:forward-id", "A").status == "new_resource",
        "scope cleanup through IDetector"
    );
}

void test_hash_compatibility() {
    require(
        huoxing::hashing::sha256_text("abc")
            == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
        "SHA256 hex format"
    );
}

void test_borrowed_sqlite_views() {
    static_assert(!std::is_copy_constructible_v<huoxing::storage::Connection>);
    static_assert(!std::is_copy_assignable_v<huoxing::storage::Statement>);

    TemporaryDatabase file;
    huoxing::storage::Connection connection(file.path());
    connection.execute("CREATE TABLE sample(value TEXT NOT NULL)");
    std::string backing = "__needle__";

    // Neither the SQL nor the text view is null-terminated at its logical end.
    const std::string wrapped_sql = "!INSERT INTO sample VALUES(?)!";
    const auto sql = std::string_view{wrapped_sql}.substr(1, wrapped_sql.size() - 2);
    huoxing::storage::Statement insert(connection, sql);
    insert.bind(1, std::string_view{backing}.substr(2, 6));
    backing[2] = 'X';
    insert.step();

    huoxing::storage::Statement query(connection, "SELECT value FROM sample");
    require(
        query.step() && query.text(0) == "needle",
        "SQLite owns a copy of a borrowed text view"
    );
}

} // namespace

int main() {
    try {
        test_hash_compatibility();
        test_borrowed_sqlite_views();
        test_image_matching();
        test_image_storage();
        test_injected_interfaces();
        test_concurrent_registration();
        fmt::println(
            "C++ tests passed: hashing, string views, matching, persistence and concurrency"
        );
        return 0;
    } catch (const std::exception& error) {
        huoxing::formatting::report_error(error.what());
        return 1;
    }
}
