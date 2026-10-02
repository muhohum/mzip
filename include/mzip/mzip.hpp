#pragma once

#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <string>

namespace mzip
{

inline constexpr std::uint32_t minimum_block_size = 1024U;
inline constexpr std::uint32_t maximum_block_size = 1024U * 1024U * 1024U;

// ratio codes the whole input as one chunk, capped at the maximum block size and cut only where
// its content changes, and takes no shortcut in choosing its coding. A block costs about 15x
// its size in memory while it is being encoded, plus a fixed amount for the context-mixing
// model.
enum class Profile : std::uint8_t
{
    balanced = 0,
    ratio = 1
};

struct CompressionOptions
{
    // 0 = 16 MiB blocks, or the whole input with the ratio profile; either is cut further
    // where the content changes.
    std::uint32_t block_size = 0;
    // 0 = hardware concurrency. The archive does not depend on the thread count.
    std::uint32_t thread_count = 0;
    Profile profile = Profile::balanced;
};

struct DecompressionOptions
{
    // 0 = hardware concurrency. The output does not depend on the thread count.
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
                                               const std::filesystem::path& output_path,
                                               const DecompressionOptions& options = {});

} // namespace mzip
