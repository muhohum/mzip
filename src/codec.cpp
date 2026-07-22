#include <mzip/mzip.hpp>

#include "transforms.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <fstream>
#include <future>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <system_error>
#include <thread>
#include <utility>

namespace mzip
{
namespace
{

using detail::Byte;
using detail::Bytes;

constexpr std::array<char, 4> archive_magic{'M', 'Z', 'I', 'P'};
constexpr std::uint8_t archive_version = 1U;
constexpr std::uint64_t file_header_size = 24U;
constexpr std::uint64_t block_header_size = 24U;

// Automatic block sizing: small inputs get one 4 MiB block, larger inputs one sixteenth of
// their size, capped so files still split into enough blocks for the thread pool.
constexpr std::uint64_t auto_block_floor = 4ULL * 1024U * 1024U;
constexpr std::uint64_t auto_block_ceiling = 16ULL * 1024U * 1024U;
constexpr std::uint64_t auto_block_divisor = 16U;

// A worker holds the block plus suffix-array scratch, roughly 15x the block size, so the
// number of in-flight blocks is bounded by a byte budget rather than a fixed count.
constexpr std::uint64_t pipeline_byte_budget = 64ULL * 1024U * 1024U;
constexpr std::uint32_t maximum_thread_count = 256U;
constexpr std::uint32_t unique_path_attempts = 10'000U;

enum class BlockMode : std::uint8_t
{
    raw = 0,
    transformed = 1
};

struct BlockHeader
{
    BlockMode mode = BlockMode::raw;
    std::uint32_t original_size = 0;
    std::uint32_t payload_size = 0;
    std::uint32_t primary_index = 0;
    std::uint32_t intermediate_size = 0;
    std::uint32_t checksum = 0;
};

class TemporaryFile
{
public:
    explicit TemporaryFile(std::filesystem::path path) : path_(std::move(path)) {}

    TemporaryFile(const TemporaryFile&) = delete;
    TemporaryFile& operator=(const TemporaryFile&) = delete;

    ~TemporaryFile()
    {
        if (!committed_)
        {
            std::error_code error;
            std::filesystem::remove(path_, error);
        }
    }

    [[nodiscard]] const std::filesystem::path& path() const noexcept
    {
        return path_;
    }

    void mark_committed() noexcept
    {
        committed_ = true;
    }

private:
    std::filesystem::path path_;
    bool committed_ = false;
};

[[nodiscard]] std::filesystem::path unique_sibling_path(const std::filesystem::path& target,
                                                        const std::string& suffix)
{
    for (std::uint32_t index = 0; index < unique_path_attempts; ++index)
    {
        std::filesystem::path candidate = target;
        candidate += suffix + std::to_string(index);
        std::error_code error;
        const bool exists = std::filesystem::exists(candidate, error);
        if (error)
        {
            throw std::runtime_error("cannot inspect temporary output path: " + error.message());
        }
        if (!exists)
        {
            return candidate;
        }
    }
    throw std::runtime_error("cannot allocate a temporary output path");
}

[[nodiscard]] bool paths_refer_to_same_file(const std::filesystem::path& left,
                                            const std::filesystem::path& right)
{
    std::error_code left_error;
    std::error_code right_error;
    const bool left_exists = std::filesystem::exists(left, left_error);
    const bool right_exists = std::filesystem::exists(right, right_error);
    if (!left_error && !right_error && left_exists && right_exists)
    {
        std::error_code equivalent_error;
        const bool equivalent = std::filesystem::equivalent(left, right, equivalent_error);
        if (!equivalent_error && equivalent)
        {
            return true;
        }
    }

    std::error_code absolute_left_error;
    std::error_code absolute_right_error;
    const auto absolute_left =
        std::filesystem::absolute(left, absolute_left_error).lexically_normal();
    const auto absolute_right =
        std::filesystem::absolute(right, absolute_right_error).lexically_normal();
    return !absolute_left_error && !absolute_right_error && absolute_left == absolute_right;
}

void commit_output(TemporaryFile& temporary, const std::filesystem::path& target)
{
    std::error_code status_error;
    const std::filesystem::file_status target_status =
        std::filesystem::symlink_status(target, status_error);
    if (target_status.type() == std::filesystem::file_type::not_found)
    {
        std::filesystem::rename(temporary.path(), target);
        temporary.mark_committed();
        return;
    }
    if (status_error)
    {
        throw std::runtime_error("cannot inspect output path: " + status_error.message());
    }
    // Only a regular file may be replaced. Renaming a directory, symlink, or device out of the
    // way would rearrange things the user never asked to touch.
    if (target_status.type() != std::filesystem::file_type::regular)
    {
        throw std::runtime_error("output path exists and is not a regular file: " +
                                 target.string());
    }

    const std::filesystem::path backup_path = unique_sibling_path(target, ".backup.");
    std::filesystem::rename(target, backup_path);
    try
    {
        std::filesystem::rename(temporary.path(), target);
        temporary.mark_committed();
    }
    catch (...)
    {
        std::error_code restore_error;
        std::filesystem::rename(backup_path, target, restore_error);
        if (restore_error)
        {
            throw std::runtime_error("failed to replace the output, and the original could "
                                     "not be restored; it remains at " +
                                     backup_path.string());
        }
        throw;
    }

    std::error_code remove_error;
    std::filesystem::remove(backup_path, remove_error);
    if (remove_error)
    {
        throw std::runtime_error(
            "output was written, but its temporary backup could not be removed");
    }
}

void write_byte(std::ostream& output, const Byte value)
{
    output.put(std::bit_cast<char>(value));
}

void write_u16(std::ostream& output, const std::uint16_t value)
{
    for (unsigned int index = 0; index < 2U; ++index)
    {
        write_byte(output, static_cast<Byte>((value >> (index * 8U)) & 0xFFU));
    }
}

void write_u32(std::ostream& output, const std::uint32_t value)
{
    for (unsigned int index = 0; index < 4U; ++index)
    {
        write_byte(output, static_cast<Byte>((value >> (index * 8U)) & 0xFFU));
    }
}

void write_u64(std::ostream& output, const std::uint64_t value)
{
    for (unsigned int index = 0; index < 8U; ++index)
    {
        write_byte(output, static_cast<Byte>((value >> (index * 8U)) & 0xFFU));
    }
}

void write_bytes(std::ostream& output, const std::span<const Byte> bytes)
{
    if (bytes.empty())
    {
        return;
    }
    output.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
}

[[nodiscard]] Byte read_byte(std::istream& input, const char* description)
{
    const int value = input.get();
    if (value == std::char_traits<char>::eof())
    {
        throw FormatError(std::string("truncated archive while reading ") + description);
    }
    return static_cast<Byte>(static_cast<unsigned int>(value));
}

[[nodiscard]] std::uint16_t read_u16(std::istream& input, const char* description)
{
    std::uint16_t result = 0;
    for (unsigned int index = 0; index < 2U; ++index)
    {
        result = static_cast<std::uint16_t>(
            result |
            static_cast<std::uint16_t>(static_cast<std::uint16_t>(read_byte(input, description))
                                       << (index * 8U)));
    }
    return result;
}

[[nodiscard]] std::uint32_t read_u32(std::istream& input, const char* description)
{
    std::uint32_t result = 0;
    for (unsigned int index = 0; index < 4U; ++index)
    {
        result |= static_cast<std::uint32_t>(read_byte(input, description)) << (index * 8U);
    }
    return result;
}

[[nodiscard]] std::uint64_t read_u64(std::istream& input, const char* description)
{
    std::uint64_t result = 0;
    for (unsigned int index = 0; index < 8U; ++index)
    {
        result |= static_cast<std::uint64_t>(read_byte(input, description)) << (index * 8U);
    }
    return result;
}

[[nodiscard]] Bytes read_bytes(std::istream& input, const std::size_t size, const char* description)
{
    Bytes bytes(size);
    if (size == 0U)
    {
        return bytes;
    }
    input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(size));
    if (input.gcount() != static_cast<std::streamsize>(size))
    {
        throw FormatError(std::string("truncated archive while reading ") + description);
    }
    return bytes;
}

void write_file_header(std::ostream& output, const std::uint32_t block_size,
                       const std::uint64_t original_size, const std::uint32_t block_count)
{
    output.write(archive_magic.data(), static_cast<std::streamsize>(archive_magic.size()));
    write_byte(output, archive_version);
    write_byte(output, 0U);
    write_u16(output, 0U);
    write_u32(output, block_size);
    write_u64(output, original_size);
    write_u32(output, block_count);
}

void write_block_header(std::ostream& output, const BlockHeader& header)
{
    write_byte(output, static_cast<Byte>(header.mode));
    write_byte(output, 0U);
    write_u16(output, 0U);
    write_u32(output, header.original_size);
    write_u32(output, header.payload_size);
    write_u32(output, header.primary_index);
    write_u32(output, header.intermediate_size);
    write_u32(output, header.checksum);
}

[[nodiscard]] BlockHeader read_block_header(std::istream& input)
{
    const Byte mode_value = read_byte(input, "block mode");
    const Byte reserved_byte = read_byte(input, "block flags");
    const std::uint16_t reserved_word = read_u16(input, "block flags");
    if (reserved_byte != 0U || reserved_word != 0U)
    {
        throw FormatError("unsupported block flags");
    }
    if (mode_value > static_cast<Byte>(BlockMode::transformed))
    {
        throw FormatError("unknown block encoding mode");
    }

    BlockHeader header;
    header.mode = static_cast<BlockMode>(mode_value);
    header.original_size = read_u32(input, "block size");
    header.payload_size = read_u32(input, "payload size");
    header.primary_index = read_u32(input, "BWT index");
    header.intermediate_size = read_u32(input, "intermediate size");
    header.checksum = read_u32(input, "checksum");
    return header;
}

[[nodiscard]] std::uint32_t expected_block_count(const std::uint64_t original_size,
                                                 const std::uint32_t block_size)
{
    if (original_size == 0U)
    {
        return 0U;
    }
    const std::uint64_t count = (original_size - 1U) / block_size + 1U;
    if (count > std::numeric_limits<std::uint32_t>::max())
    {
        throw std::runtime_error("input requires too many blocks for the MZIP format");
    }
    return static_cast<std::uint32_t>(count);
}

// A requested size of zero picks one from the input: 4 MiB up to 64 MiB inputs, then growing
// so large files still split into enough blocks to keep the thread pool busy. The choice
// depends only on the input size, so archives stay reproducible.
[[nodiscard]] std::uint32_t resolve_block_size(const std::uint32_t requested,
                                               const std::uint64_t input_size)
{
    if (requested != 0U)
    {
        return requested;
    }
    return static_cast<std::uint32_t>(std::clamp<std::uint64_t>(
        input_size / auto_block_divisor, auto_block_floor, auto_block_ceiling));
}

void validate_block_size(const std::uint32_t block_size)
{
    if (block_size < minimum_block_size || block_size > maximum_block_size)
    {
        throw std::invalid_argument("block size must be between " +
                                    std::to_string(minimum_block_size) + " and " +
                                    std::to_string(maximum_block_size) + " bytes");
    }
}

void increment_mode_count(CompressionStats& stats, const BlockMode mode)
{
    switch (mode)
    {
    case BlockMode::raw:
        ++stats.raw_blocks;
        break;
    case BlockMode::transformed:
        ++stats.transformed_blocks;
        break;
    }
}

[[nodiscard]] std::uint32_t narrow_size(const std::size_t size, const char* description)
{
    if (size > std::numeric_limits<std::uint32_t>::max())
    {
        throw std::runtime_error(std::string(description) + " does not fit the archive format");
    }
    return static_cast<std::uint32_t>(size);
}

struct EncodedBlock
{
    BlockHeader header;
    Bytes bytes;
};

// Pure function of the block bytes, so it can run on any worker thread.
// The run extension thresholds are tried one after the other, each abandoned as soon as its
// payload can no longer beat the best size seen so far.
constexpr std::size_t aggressive_extension_run = 2;
constexpr std::size_t conservative_extension_run = 3;

// Pure function of the block bytes, so it can run on any worker thread.
[[nodiscard]] EncodedBlock encode_block(Bytes original)
{
    EncodedBlock result;
    result.header.original_size = narrow_size(original.size(), "block size");
    result.header.payload_size = result.header.original_size;
    result.header.checksum = detail::adler32(original);

    std::uint32_t primary_index = 0;
    std::optional<detail::EncodedStream> best;
    {
        Bytes mtf;
        {
            detail::BwtResult bwt = detail::bwt_encode(original);
            primary_index = bwt.primary_index;
            mtf = std::move(bwt.data);
        }
        detail::mtf_encode(mtf);

        best = detail::rc_encode(mtf, aggressive_extension_run, original.size());
        const std::size_t limit =
            best ? std::min(original.size(), best->payload.size()) : original.size();
        std::optional<detail::EncodedStream> conservative =
            detail::rc_encode(mtf, conservative_extension_run, limit);
        if (conservative && (!best || conservative->payload.size() < best->payload.size()))
        {
            best = std::move(conservative);
        }
    }

    if (best && best->payload.size() < original.size())
    {
        result.header.mode = BlockMode::transformed;
        result.header.payload_size = narrow_size(best->payload.size(), "transformed payload");
        result.header.primary_index = primary_index;
        result.header.intermediate_size =
            narrow_size(best->symbol_count, "run coding intermediate stream");
        result.bytes = std::move(best->payload);
    }
    else
    {
        result.bytes = std::move(original);
    }
    return result;
}

void write_encoded_block(std::ostream& output, const EncodedBlock& block, CompressionStats& stats)
{
    write_block_header(output, block.header);
    write_bytes(output, block.bytes);
    if (!output)
    {
        throw std::runtime_error("failed while writing compressed output");
    }
    increment_mode_count(stats, block.header.mode);
}

[[nodiscard]] std::uint32_t effective_thread_count(const std::uint32_t requested)
{
    std::uint32_t threads = requested;
    if (threads == 0U)
    {
        threads = std::max(1U, std::thread::hardware_concurrency());
    }
    return std::min(threads, maximum_thread_count);
}

} // namespace

double CompressionStats::ratio() const noexcept
{
    if (input_size == 0U)
    {
        return 0.0;
    }
    return static_cast<double>(output_size) / static_cast<double>(input_size);
}

double CompressionStats::savings_percent() const noexcept
{
    if (input_size == 0U)
    {
        return 0.0;
    }
    return (1.0 - ratio()) * 100.0;
}

FormatError::FormatError(const std::string& message) : std::runtime_error(message) {}

CompressionStats compress_file(const std::filesystem::path& input_path,
                               const std::filesystem::path& output_path,
                               const CompressionOptions& options)
{
    const auto started_at = std::chrono::steady_clock::now();
    if (options.block_size != 0U)
    {
        validate_block_size(options.block_size);
    }
    if (paths_refer_to_same_file(input_path, output_path))
    {
        throw std::invalid_argument("input and output paths must be different");
    }

    std::error_code size_error;
    const std::uint64_t input_size = std::filesystem::file_size(input_path, size_error);
    if (size_error)
    {
        throw std::runtime_error("cannot determine input size: " + size_error.message());
    }
    const std::uint32_t block_size = resolve_block_size(options.block_size, input_size);
    const std::uint32_t block_count = expected_block_count(input_size, block_size);

    std::ifstream input(input_path, std::ios::binary);
    if (!input)
    {
        throw std::runtime_error("cannot open input file: " + input_path.string());
    }

    TemporaryFile temporary(unique_sibling_path(output_path, ".tmp."));
    std::ofstream output(temporary.path(), std::ios::binary | std::ios::trunc);
    if (!output)
    {
        throw std::runtime_error("cannot create output file: " + temporary.path().string());
    }

    write_file_header(output, block_size, input_size, block_count);

    CompressionStats stats;
    stats.input_size = input_size;
    stats.block_count = block_count;
    std::uint64_t remaining = input_size;

    const auto read_block = [&](Bytes& original)
    {
        const std::size_t current_size =
            static_cast<std::size_t>(std::min<std::uint64_t>(remaining, block_size));
        original.resize(current_size);
        input.read(reinterpret_cast<char*>(original.data()),
                   static_cast<std::streamsize>(current_size));
        if (input.gcount() != static_cast<std::streamsize>(current_size))
        {
            throw std::runtime_error("input file changed or ended during compression");
        }
        remaining -= current_size;
    };

    const std::uint32_t thread_count = effective_thread_count(options.thread_count);
    const std::size_t window = static_cast<std::size_t>(
        std::clamp<std::uint64_t>(pipeline_byte_budget / block_size, 1U, thread_count));
    if (window <= 1U || block_count <= 1U)
    {
        for (std::uint32_t block_index = 0; block_index < block_count; ++block_index)
        {
            Bytes original;
            read_block(original);
            write_encoded_block(output, encode_block(std::move(original)), stats);
        }
    }
    else
    {
        // Read in order, encode in parallel, write in order; the window bounds memory use and
        // keeps the archive layout independent of scheduling.
        std::deque<std::future<EncodedBlock>> in_flight;
        for (std::uint32_t block_index = 0; block_index < block_count; ++block_index)
        {
            if (in_flight.size() >= window)
            {
                write_encoded_block(output, in_flight.front().get(), stats);
                in_flight.pop_front();
            }
            Bytes original;
            read_block(original);
            in_flight.push_back(std::async(std::launch::async,
                                           [block = std::move(original)]() mutable
                                           { return encode_block(std::move(block)); }));
        }
        while (!in_flight.empty())
        {
            write_encoded_block(output, in_flight.front().get(), stats);
            in_flight.pop_front();
        }
    }

    if (remaining != 0U || input.peek() != std::char_traits<char>::eof())
    {
        throw std::runtime_error("input file changed during compression");
    }

    output.close();
    if (!output)
    {
        throw std::runtime_error("failed to finalize compressed output");
    }

    std::error_code output_size_error;
    stats.output_size = std::filesystem::file_size(temporary.path(), output_size_error);
    if (output_size_error)
    {
        throw std::runtime_error("cannot determine output size: " + output_size_error.message());
    }
    commit_output(temporary, output_path);
    stats.elapsed_seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - started_at).count();
    return stats;
}

CompressionStats decompress_file(const std::filesystem::path& input_path,
                                 const std::filesystem::path& output_path)
{
    const auto started_at = std::chrono::steady_clock::now();
    if (paths_refer_to_same_file(input_path, output_path))
    {
        throw std::invalid_argument("input and output paths must be different");
    }

    std::error_code archive_size_error;
    const std::uint64_t archive_size = std::filesystem::file_size(input_path, archive_size_error);
    if (archive_size_error)
    {
        throw std::runtime_error("cannot determine archive size: " + archive_size_error.message());
    }
    if (archive_size < file_header_size)
    {
        throw FormatError("archive is smaller than the MZIP header");
    }

    std::ifstream input(input_path, std::ios::binary);
    if (!input)
    {
        throw std::runtime_error("cannot open archive: " + input_path.string());
    }

    const Bytes magic = read_bytes(input, archive_magic.size(), "archive magic");
    if (!std::equal(magic.begin(), magic.end(), archive_magic.begin(), archive_magic.end()))
    {
        throw FormatError("not an MZIP archive");
    }
    const Byte version = read_byte(input, "archive version");
    if (version != archive_version)
    {
        throw FormatError("unsupported MZIP version");
    }
    const Byte flags = read_byte(input, "archive flags");
    const std::uint16_t reserved = read_u16(input, "archive flags");
    if (flags != 0U || reserved != 0U)
    {
        throw FormatError("unsupported MZIP archive flags");
    }

    const std::uint32_t block_size = read_u32(input, "archive block size");
    try
    {
        validate_block_size(block_size);
    }
    catch (const std::invalid_argument& error)
    {
        throw FormatError(error.what());
    }
    const std::uint64_t original_size = read_u64(input, "archive original size");
    const std::uint32_t block_count = read_u32(input, "archive block count");
    std::uint32_t calculated_block_count = 0;
    try
    {
        calculated_block_count = expected_block_count(original_size, block_size);
    }
    catch (const std::runtime_error& error)
    {
        throw FormatError(error.what());
    }
    if (block_count != calculated_block_count)
    {
        throw FormatError("archive block count does not match its original size");
    }

    TemporaryFile temporary(unique_sibling_path(output_path, ".tmp."));
    std::ofstream output(temporary.path(), std::ios::binary | std::ios::trunc);
    if (!output)
    {
        throw std::runtime_error("cannot create output file: " + temporary.path().string());
    }

    CompressionStats stats;
    stats.input_size = archive_size;
    stats.output_size = original_size;
    stats.block_count = block_count;
    std::uint64_t remaining = original_size;
    std::uint64_t archive_remaining = archive_size - file_header_size;

    for (std::uint32_t block_index = 0; block_index < block_count; ++block_index)
    {
        if (archive_remaining < block_header_size)
        {
            throw FormatError("truncated archive while reading block header");
        }
        archive_remaining -= block_header_size;
        const BlockHeader header = read_block_header(input);
        const std::uint32_t expected_size =
            static_cast<std::uint32_t>(std::min<std::uint64_t>(remaining, block_size));
        if (header.original_size != expected_size || header.original_size == 0U)
        {
            throw FormatError("block has an unexpected original size");
        }
        if (header.payload_size == 0U || header.payload_size > header.original_size)
        {
            throw FormatError("block payload size is outside the allowed range");
        }
        // Checked against the real file size before any allocation, so declared sizes cannot
        // make a tiny hostile archive balloon.
        if (header.payload_size > archive_remaining)
        {
            throw FormatError("block payload exceeds the archive size");
        }
        archive_remaining -= header.payload_size;

        Bytes payload = read_bytes(input, header.payload_size, "block payload");
        Bytes decoded;
        switch (header.mode)
        {
        case BlockMode::raw:
            if (header.payload_size != header.original_size || header.primary_index != 0U ||
                header.intermediate_size != 0U)
            {
                throw FormatError("raw block contains transform metadata");
            }
            decoded = std::move(payload);
            break;

        case BlockMode::transformed:
        {
            // A run symbol consumes at least one MTF byte, so the intermediate stream can never
            // be longer than the block it describes.
            if (header.primary_index == 0U || header.primary_index > header.original_size ||
                header.intermediate_size == 0U || header.intermediate_size > header.original_size)
            {
                throw FormatError("invalid transformed block metadata");
            }
            Bytes mtf = detail::rc_decode(payload, header.intermediate_size, header.original_size);
            payload = Bytes();
            detail::mtf_decode(mtf);
            decoded = detail::bwt_decode(mtf, header.primary_index);
            break;
        }
        }

        if (detail::adler32(decoded) != header.checksum)
        {
            throw FormatError("block checksum mismatch");
        }
        write_bytes(output, decoded);
        if (!output)
        {
            throw std::runtime_error("failed while writing decompressed output");
        }
        remaining -= header.original_size;
        increment_mode_count(stats, header.mode);
    }

    if (remaining != 0U)
    {
        throw FormatError("archive ended before the declared original size");
    }
    if (input.peek() != std::char_traits<char>::eof())
    {
        throw FormatError("archive contains trailing data");
    }

    output.close();
    if (!output)
    {
        throw std::runtime_error("failed to finalize decompressed output");
    }
    commit_output(temporary, output_path);
    stats.elapsed_seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - started_at).count();
    return stats;
}

} // namespace mzip
