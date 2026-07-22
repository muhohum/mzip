#pragma once

#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <string>

namespace mzip
{

inline constexpr std::uint32_t minimum_block_size = 1024U;
inline constexpr std::uint32_t maximum_block_size = 64U * 1024U * 1024U;

struct CompressionOptions
{
    // 0 = pick a block size from the input size (larger inputs get larger blocks).
    std::uint32_t block_size = 0;
    // 0 = hardware concurrency. The archive does not depend on the thread count.
    std::uint32_t thread_count = 0;
};

struct CompressionStats
{
    std::uint64_t input_size = 0;
    std::uint64_t output_size = 0;
    std::uint32_t block_count = 0;
    std::uint32_t raw_blocks = 0;
    std::uint32_t transformed_blocks = 0;
    double elapsed_seconds = 0.0;

    [[nodiscard]] double ratio() const noexcept;
    [[nodiscard]] double savings_percent() const noexcept;
};

class FormatError : public std::runtime_error
{
public:
    explicit FormatError(const std::string& message);
};

[[nodiscard]] CompressionStats compress_file(const std::filesystem::path& input_path,
                                             const std::filesystem::path& output_path,
                                             const CompressionOptions& options = {});

[[nodiscard]] CompressionStats decompress_file(const std::filesystem::path& input_path,
                                               const std::filesystem::path& output_path);

} // namespace mzip
