module;
#include <fmt/format.h>
#include <sqlite3.h>

export module huoxing.sqlite;
import std;

export namespace huoxing::storage {

// Connections and statements are non-copyable owners. Repositories declare the
// connection before the transaction so rollback runs before the handle closes.
class Connection {
  public:

    explicit Connection(std::string_view path) {
        const std::string terminated_path{path};
        if (sqlite3_open(terminated_path.c_str(), &handle_) != SQLITE_OK) {
            const auto error = handle_ ? sqlite3_errmsg(handle_) : "allocation failed";
            const auto message = fmt::format("SQLite open failed: {}", error);
            if (handle_) {
                sqlite3_close(handle_);
            }
            throw std::runtime_error(message);
        }

        sqlite3_busy_timeout(handle_, 30000);
        try {
            execute("PRAGMA foreign_keys=ON");
        } catch (...) {
            sqlite3_close(handle_);
            throw;
        }
    }

    ~Connection() {
        sqlite3_close(handle_);
    }

    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;

    void execute(std::string_view sql) {
        // sqlite3_exec requires a terminator; callers may pass a substring view.
        const std::string terminated_sql{sql};
        char* error = nullptr;
        if (sqlite3_exec(handle_, terminated_sql.c_str(), nullptr, nullptr, &error) != SQLITE_OK) {
            const auto message =
                fmt::format("SQLite execution failed: {}", error ? error : sqlite3_errmsg(handle_));
            sqlite3_free(error);
            throw std::runtime_error(message);
        }
    }

    void rollback_noexcept() noexcept {
        // Destructors must not replace the original exception with a rollback error.
        sqlite3_exec(handle_, "ROLLBACK", nullptr, nullptr, nullptr);
    }

    sqlite3* native_handle() const noexcept {
        return handle_;
    }

    std::int64_t last_insert_id() const noexcept {
        return sqlite3_last_insert_rowid(handle_);
    }

  private:

    sqlite3* handle_ = nullptr;
};

class Statement {
  public:

    Statement(Connection& connection, std::string_view sql) {
        if (!std::in_range<int>(sql.size())) {
            throw std::length_error("SQLite query is too large");
        }

        const auto status = sqlite3_prepare_v2(
            connection.native_handle(),
            sql.data(),
            static_cast<int>(sql.size()),
            &handle_,
            nullptr
        );
        if (status != SQLITE_OK) {
            throw std::runtime_error(
                fmt::format("SQLite prepare failed: {}", sqlite3_errmsg(connection.native_handle()))
            );
        }
    }

    ~Statement() {
        sqlite3_finalize(handle_);
    }

    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;

    void bind(int parameter, std::string_view value) {
        if (!std::in_range<int>(value.size())) {
            throw std::length_error("SQLite text value is too large");
        }

        // TRANSIENT copies the exact view length. No borrowed string survives this call.
        check(sqlite3_bind_text(
            handle_,
            parameter,
            value.empty() ? "" : value.data(),
            static_cast<int>(value.size()),
            SQLITE_TRANSIENT
        ));
    }

    void bind(int parameter, std::int64_t value) {
        check(sqlite3_bind_int64(handle_, parameter, value));
    }

    void bind(int parameter, double value) {
        check(sqlite3_bind_double(handle_, parameter, value));
    }

    void bind_blob(int parameter, std::span<const std::byte> bytes) {
        if (!std::in_range<int>(bytes.size())) {
            throw std::length_error("SQLite blob is too large");
        }

        if (bytes.empty()) {
            check(sqlite3_bind_zeroblob(handle_, parameter, 0));
        } else {
            check(sqlite3_bind_blob(
                handle_,
                parameter,
                bytes.data(),
                static_cast<int>(bytes.size()),
                SQLITE_TRANSIENT
            ));
        }
    }

    bool step() {
        const auto status = sqlite3_step(handle_);
        if (status == SQLITE_ROW) {
            return true;
        }
        if (status == SQLITE_DONE) {
            return false;
        }

        throw std::runtime_error(
            fmt::format("SQLite step failed: {}", sqlite3_errmsg(sqlite3_db_handle(handle_)))
        );
    }

    std::int64_t integer(int column) const {
        return sqlite3_column_int64(handle_, column);
    }

    double number(int column) const {
        return sqlite3_column_double(handle_, column);
    }

    std::string text(int column) const {
        const auto* bytes = sqlite3_column_text(handle_, column);
        if (!bytes) {
            return {};
        }

        return {
            reinterpret_cast<const char*>(bytes),
            static_cast<std::size_t>(sqlite3_column_bytes(handle_, column))
        };
    }

    // This view is valid only until the next step or destruction of this statement.
    std::span<const std::byte> blob(int column) const {
        const auto* bytes = static_cast<const std::byte*>(sqlite3_column_blob(handle_, column));
        return {bytes, static_cast<std::size_t>(sqlite3_column_bytes(handle_, column))};
    }

  private:

    void check(int status) const {
        if (status != SQLITE_OK) {
            throw std::runtime_error(
                fmt::format("SQLite bind failed: {}", sqlite3_errmsg(sqlite3_db_handle(handle_)))
            );
        }
    }

    sqlite3_stmt* handle_ = nullptr;
};

class Transaction {
  public:

    explicit Transaction(Connection& connection) : connection_(connection) {
        connection_.execute("BEGIN IMMEDIATE");
    }

    ~Transaction() {
        if (!committed_) {
            connection_.rollback_noexcept();
        }
    }

    Transaction(const Transaction&) = delete;
    Transaction& operator=(const Transaction&) = delete;

    void commit() {
        connection_.execute("COMMIT");
        committed_ = true;
    }

  private:

    Connection& connection_;
    bool committed_ = false;
};

} // namespace huoxing::storage
