module;
#include <fmt/format.h>
#include <opencv2/core.hpp>
#include <openssl/evp.h>

export module huoxing.hashing;
import std;

namespace {

// An incremental context lets RGB rows be hashed without copying the whole image.
class Sha256 {
  public:

    Sha256() : context_(EVP_MD_CTX_new(), EVP_MD_CTX_free) {
        if (!context_ || EVP_DigestInit_ex(context_.get(), EVP_sha256(), nullptr) != 1) {
            throw std::runtime_error("SHA256 initialization failed");
        }
    }

    void update(std::span<const std::byte> bytes) {
        if (EVP_DigestUpdate(context_.get(), bytes.data(), bytes.size()) != 1) {
            throw std::runtime_error("SHA256 update failed");
        }
    }

    std::string finish() {
        std::array<unsigned char, EVP_MAX_MD_SIZE> bytes{};
        unsigned int size = 0;

        if (EVP_DigestFinal_ex(context_.get(), bytes.data(), &size) != 1) {
            throw std::runtime_error("SHA256 final failed");
        }

        // Preserve the lowercase, zero-padded encoding used by existing databases.
        fmt::memory_buffer output;
        for (const auto byte : bytes | std::views::take(size)) {
            fmt::format_to(std::back_inserter(output), "{:02x}", byte);
        }

        return fmt::to_string(output);
    }

  private:

    std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> context_;
};

} // namespace

export namespace huoxing::hashing {

std::string sha256_text(std::string_view text) {
    Sha256 hash;
    hash.update(std::as_bytes(std::span{text.data(), text.size()}));
    return hash.finish();
}

std::string sha256_rgb(const cv::Mat& rgb) {
    Sha256 hash;

    // Dimensions are part of the hash format; keep the original "widthxheight:" prefix.
    const auto header = fmt::format("{}x{}:", rgb.cols, rgb.rows);
    hash.update(std::as_bytes(std::span{header.data(), header.size()}));

    const auto row_bytes = static_cast<std::size_t>(rgb.cols) * 3;
    for (const auto row : std::views::iota(0, rgb.rows)) {
        hash.update(std::as_bytes(std::span{rgb.ptr(row), row_bytes}));
    }

    return hash.finish();
}

} // namespace huoxing::hashing
