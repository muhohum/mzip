#include <mzip/mzip.hpp>

#include "archive.hpp"
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
#include <vector>

namespace mzip
{
namespace
{

using detail::Byte;
using detail::Bytes;

constexpr std::array<char, 4> archive_magic{'M', 'Z', 'I', 'P'};
constexpr std::uint8_t archive_version = 2U;
constexpr std::uint8_t oldest_readable_version = 1U;
constexpr std::uint64_t file_header_size = 24U;
constexpr std::uint64_t block_header_size = 24U;
// Bit 0: tar directory tree. Bit 1: one LZP stream under the blocks; a 16-byte header follows.
constexpr Byte archive_flag_directory = 0x01U;
constexpr Byte archive_flag_stream_lzp = 0x02U;
constexpr std::uint64_t stream_header_size = 16U;
constexpr std::uint64_t stream_lzp_limit = 512ULL * 1024U * 1024U;
constexpr unsigned int stream_hash_bits_floor = 20U;
constexpr unsigned int stream_hash_bits_ceiling = 26U;

constexpr std::uint64_t auto_block_floor = 4ULL * 1024U * 1024U;
constexpr std::uint64_t auto_block_ceiling = 16ULL * 1024U * 1024U;
constexpr std::uint64_t auto_block_divisor = 16U;

// An in-flight block costs roughly 15x its size in scratch.
constexpr std::uint64_t pipeline_byte_budget = 128ULL * 1024U * 1024U;
constexpr std::uint32_t maximum_thread_count = 256U;
constexpr std::uint32_t unique_path_attempts = 10'000U;

enum class BlockMode : std::uint8_t
{
    raw = 0,
    transformed = 1,
    mixed = 2
};

// Bit 0 of the block flags: the block bytes went through LZP before the BWT.
constexpr Byte block_flag_lzp = 0x01U;

struct BlockHeader
{
    BlockMode mode = BlockMode::raw;
    bool lzp = false;
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
                       const std::uint64_t original_size, const std::uint32_t block_count,
                       const Byte flags)
{
    output.write(archive_magic.data(), static_cast<std::streamsize>(archive_magic.size()));
    write_byte(output, archive_version);
    write_byte(output, flags);
    write_u16(output, 0U);
    write_u32(output, block_size);
    write_u64(output, original_size);
    write_u32(output, block_count);
}

void write_block_header(std::ostream& output, const BlockHeader& header)
{
    write_byte(output, static_cast<Byte>(header.mode));
    write_byte(output, header.lzp ? block_flag_lzp : Byte{0U});
    write_u16(output, 0U);
    write_u32(output, header.original_size);
    write_u32(output, header.payload_size);
    write_u32(output, header.primary_index);
    write_u32(output, header.intermediate_size);
    write_u32(output, header.checksum);
}

[[nodiscard]] BlockHeader read_block_header(std::istream& input, const std::uint8_t version)
{
    const Byte mode_value = read_byte(input, "block mode");
    const Byte flags = read_byte(input, "block flags");
    const std::uint16_t reserved_word = read_u16(input, "block flags");
    const Byte mode_limit = version >= 2U ? static_cast<Byte>(BlockMode::mixed)
                                          : static_cast<Byte>(BlockMode::transformed);
    const Byte flag_limit = version >= 2U ? block_flag_lzp : Byte{0U};
    if ((flags & static_cast<Byte>(~flag_limit)) != 0U || reserved_word != 0U)
    {
        throw FormatError("unsupported block flags");
    }
    if (mode_value > mode_limit)
    {
        throw FormatError("unknown block encoding mode");
    }

    BlockHeader header;
    header.mode = static_cast<BlockMode>(mode_value);
    header.lzp = (flags & block_flag_lzp) != 0U;
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

[[nodiscard]] std::uint32_t resolve_block_size(const CompressionOptions& options,
                                               const std::uint64_t input_size)
{
    if (options.block_size != 0U)
    {
        return options.block_size;
    }
    if (options.profile == Profile::ratio)
    {
        return static_cast<std::uint32_t>(
            std::clamp<std::uint64_t>(input_size, minimum_block_size, maximum_block_size));
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
    case BlockMode::mixed:
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

// Table entries scale to at least a quarter of the coded bytes.
[[nodiscard]] unsigned int stream_hash_bits(const std::uint64_t size) noexcept
{
    unsigned int bits = stream_hash_bits_floor;
    while (bits < stream_hash_bits_ceiling && (std::uint64_t{4} << bits) < size)
    {
        ++bits;
    }
    return bits;
}

struct EncodedBlock
{
    BlockHeader header;
    Bytes bytes;
};

constexpr std::size_t aggressive_extension_run = 2;
constexpr std::size_t conservative_extension_run = 3;

struct BlockCandidate
{
    std::optional<Bytes> payload;
    std::uint32_t primary_index = 0;
    bool context_mixed = false;
    std::uint32_t symbol_count = 0;
};

// Tries the context mixer and both run coders on one input; keeps the smallest payload.
[[nodiscard]] BlockCandidate encode_candidate(const std::span<const Byte> input,
                                              std::size_t size_limit)
{
    BlockCandidate candidate;
    Bytes mtf;
    {
        detail::BwtResult bwt = detail::bwt_encode(input);
        candidate.primary_index = bwt.primary_index;
        std::optional<Bytes> mixed = detail::cm_encode(bwt.data, size_limit);
        if (mixed)
        {
            size_limit = std::min(size_limit, mixed->size());
            candidate.payload = std::move(mixed);
            candidate.context_mixed = true;
        }
        mtf = std::move(bwt.data);
    }
    detail::mtf_encode(mtf);

    for (const std::size_t extension_run : {aggressive_extension_run, conservative_extension_run})
    {
        std::optional<detail::EncodedStream> coded =
            detail::rc_encode(mtf, extension_run, size_limit);
        if (coded && (!candidate.payload || coded->payload.size() < candidate.payload->size()))
        {
            size_limit = coded->payload.size();
            candidate.symbol_count =
                narrow_size(coded->symbol_count, "run coding intermediate stream");
            candidate.payload = std::move(coded->payload);
            candidate.context_mixed = false;
        }
    }
    return candidate;
}

[[nodiscard]] EncodedBlock encode_block(Bytes original)
{
    EncodedBlock result;
    result.header.original_size = narrow_size(original.size(), "block size");
    result.header.payload_size = result.header.original_size;
    result.header.checksum = detail::adler32(original);

    BlockCandidate best = encode_candidate(original, original.size());
    std::uint32_t intermediate = best.context_mixed ? 0U : best.symbol_count;
    bool lzp_used = false;

    const std::optional<Bytes> lzp =
        detail::lzp_encode(original, stream_hash_bits(original.size()));
    if (lzp)
    {
        const std::size_t limit =
            best.payload ? std::min(original.size(), best.payload->size()) : original.size();
        detail::BwtResult bwt = detail::bwt_encode(*lzp);
        std::optional<Bytes> mixed = detail::cm_encode(bwt.data, limit);
        if (mixed && (!best.payload || mixed->size() < best.payload->size()))
        {
            best.payload = std::move(mixed);
            best.primary_index = bwt.primary_index;
            best.context_mixed = true;
            intermediate = narrow_size(lzp->size(), "LZP intermediate stream");
            lzp_used = true;
        }
    }

    if (best.payload && best.payload->size() < original.size())
    {
        result.header.mode = best.context_mixed ? BlockMode::mixed : BlockMode::transformed;
        result.header.lzp = lzp_used;
        result.header.payload_size = narrow_size(best.payload->size(), "block payload");
        result.header.primary_index = best.primary_index;
        result.header.intermediate_size = intermediate;
        result.bytes = std::move(*best.payload);
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

[[nodiscard]] Bytes decode_block(const BlockHeader& header, Bytes payload)
{
    Bytes decoded;
    switch (header.mode)
    {
    case BlockMode::raw:
        if (header.payload_size != header.original_size || header.primary_index != 0U ||
            header.intermediate_size != 0U || header.lzp)
        {
            throw FormatError("raw block contains transform metadata");
        }
        decoded = std::move(payload);
        break;

    case BlockMode::transformed:
    {
        // Run symbols consume at least one MTF byte each.
        if (header.primary_index == 0U || header.primary_index > header.original_size ||
            header.intermediate_size == 0U || header.intermediate_size > header.original_size ||
            header.lzp)
        {
            throw FormatError("invalid transformed block metadata");
        }
        Bytes mtf = detail::rc_decode(payload, header.intermediate_size, header.original_size);
        payload = Bytes();
        detail::mtf_decode(mtf);
        decoded = detail::bwt_decode(mtf, header.primary_index);
        break;
    }

    case BlockMode::mixed:
    {
        // intermediate_size: zero without LZP, otherwise the LZP stream length.
        const std::uint32_t coded_size =
            header.lzp ? header.intermediate_size : header.original_size;
        if (header.primary_index == 0U || header.primary_index > coded_size ||
            (header.lzp ? header.intermediate_size >= header.original_size
                        : header.intermediate_size != 0U))
        {
            throw FormatError("invalid mixed block metadata");
        }
        Bytes coded = detail::cm_decode(payload, coded_size);
        payload = Bytes();
        Bytes stream = detail::bwt_decode(coded, header.primary_index);
        coded = Bytes();
        decoded = header.lzp ? detail::lzp_decode(stream, header.original_size,
                                                  stream_hash_bits(header.original_size))
                             : std::move(stream);
        break;
    }
    }
    if (detail::adler32(decoded) != header.checksum)
    {
        throw FormatError("block checksum mismatch");
    }
    return decoded;
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

[[nodiscard]] std::uint64_t encoded_archive_size(const std::vector<EncodedBlock>& blocks) noexcept
{
    std::uint64_t total = 0;
    for (const EncodedBlock& block : blocks)
    {
        total += block_header_size + block.bytes.size();
    }
    return total;
}

// Same ordered window pipeline as the streaming path, over an in-memory stream.
[[nodiscard]] std::vector<EncodedBlock>
encode_buffered(const Bytes& data, const std::uint32_t block_size, const std::uint32_t thread_count)
{
    const std::uint32_t block_count = expected_block_count(data.size(), block_size);
    std::vector<EncodedBlock> encoded;
    encoded.reserve(block_count);
    const auto slice = [&](const std::uint32_t index)
    {
        const std::size_t begin = static_cast<std::size_t>(index) * block_size;
        const std::size_t end = std::min(data.size(), begin + block_size);
        return Bytes(data.begin() + static_cast<std::ptrdiff_t>(begin),
                     data.begin() + static_cast<std::ptrdiff_t>(end));
    };

    const std::size_t window = static_cast<std::size_t>(
        std::clamp<std::uint64_t>(pipeline_byte_budget / block_size, 1U, thread_count));
    if (window <= 1U || block_count <= 1U)
    {
        for (std::uint32_t index = 0; index < block_count; ++index)
        {
            encoded.push_back(encode_block(slice(index)));
        }
        return encoded;
    }

    struct InFlightBlock
    {
        std::thread worker;
        std::future<EncodedBlock> result;
    };
    std::deque<InFlightBlock> in_flight;
    const auto drain_front = [&]
    {
        InFlightBlock front = std::move(in_flight.front());
        in_flight.pop_front();
        front.worker.join();
        encoded.push_back(front.result.get());
    };

    try
    {
        for (std::uint32_t index = 0; index < block_count; ++index)
        {
            if (in_flight.size() >= window)
            {
                drain_front();
            }
            std::packaged_task<EncodedBlock()> task([block = slice(index)]() mutable
                                                    { return encode_block(std::move(block)); });
            in_flight.emplace_back();
            InFlightBlock& slot = in_flight.back();
            slot.result = task.get_future();
            try
            {
                slot.worker = std::thread(std::move(task));
            }
            catch (...)
            {
                in_flight.pop_back();
                throw;
            }
        }
        while (!in_flight.empty())
        {
            drain_front();
        }
    }
    catch (...)
    {
        for (InFlightBlock& block : in_flight)
        {
            if (block.worker.joinable())
            {
                block.worker.join();
            }
        }
        throw;
    }
    return encoded;
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

    std::filesystem::path source_path = input_path;
    if (source_path.filename().empty())
    {
        source_path = source_path.parent_path();
    }
    std::error_code status_error;
    const std::filesystem::file_status input_status =
        std::filesystem::symlink_status(source_path, status_error);
    if (status_error)
    {
        throw std::runtime_error("cannot inspect input path: " + status_error.message());
    }
    const bool directory_input = input_status.type() == std::filesystem::file_type::directory;

    std::optional<detail::TarStreamSource> tar_source;
    std::ifstream input;
    std::uint64_t input_size = 0;
    if (directory_input)
    {
        tar_source.emplace(source_path);
        input_size = tar_source->total_size();
    }
    else
    {
        std::error_code size_error;
        input_size = std::filesystem::file_size(source_path, size_error);
        if (size_error)
        {
            throw std::runtime_error("cannot determine input size: " + size_error.message());
        }
        input.open(source_path, std::ios::binary);
        if (!input)
        {
            throw std::runtime_error("cannot open input file: " + source_path.string());
        }
    }
    const std::uint32_t block_size = resolve_block_size(options, input_size);

    // Multi-block inputs up to the limit are buffered for the stream-wide LZP pass.
    Bytes buffer;
    bool buffered = false;
    if (input_size > block_size && input_size <= stream_lzp_limit)
    {
        buffer.resize(static_cast<std::size_t>(input_size));
        if (tar_source)
        {
            std::size_t filled = 0;
            while (filled < buffer.size())
            {
                const std::size_t produced =
                    tar_source->read(buffer.data() + filled, buffer.size() - filled);
                if (produced == 0U)
                {
                    throw std::runtime_error("directory contents changed during compression");
                }
                filled += produced;
            }
            Byte probe = 0;
            if (tar_source->read(&probe, 1U) != 0U)
            {
                throw std::runtime_error("directory contents changed during compression");
            }
        }
        else
        {
            input.read(reinterpret_cast<char*>(buffer.data()),
                       static_cast<std::streamsize>(buffer.size()));
            if (input.gcount() != static_cast<std::streamsize>(buffer.size()) ||
                input.peek() != std::char_traits<char>::eof())
            {
                throw std::runtime_error("input file changed during compression");
            }
        }
        buffered = true;
    }

    const std::uint32_t thread_count = effective_thread_count(options.thread_count);

    // A decisive shrink commits to the LZP stream; a marginal one compares both archives.
    bool stream_lzp = false;
    unsigned int stream_bits = 0;
    std::uint32_t stream_checksum = 0;
    std::vector<EncodedBlock> encoded;
    if (buffered)
    {
        stream_checksum = detail::adler32(buffer);
        stream_bits = stream_hash_bits(input_size);
        std::optional<Bytes> collapsed = detail::lzp_encode(buffer, stream_bits);
        if (collapsed && collapsed->size() < buffer.size() - buffer.size() / 10U)
        {
            buffer = std::move(*collapsed);
            stream_lzp = true;
            encoded = encode_buffered(buffer, block_size, thread_count);
        }
        else if (collapsed)
        {
            std::vector<EncodedBlock> with_pass =
                encode_buffered(*collapsed, block_size, thread_count);
            encoded = encode_buffered(buffer, block_size, thread_count);
            if (encoded_archive_size(with_pass) + stream_header_size <
                encoded_archive_size(encoded))
            {
                buffer = std::move(*collapsed);
                stream_lzp = true;
                encoded = std::move(with_pass);
            }
        }
        else
        {
            encoded = encode_buffered(buffer, block_size, thread_count);
        }
    }

    const std::uint64_t coded_size = buffered ? buffer.size() : input_size;
    const std::uint32_t block_count = expected_block_count(coded_size, block_size);

    TemporaryFile temporary(unique_sibling_path(output_path, ".tmp."));
    std::ofstream output(temporary.path(), std::ios::binary | std::ios::trunc);
    if (!output)
    {
        throw std::runtime_error("cannot create output file: " + temporary.path().string());
    }

    Byte flags = directory_input ? archive_flag_directory : Byte{0U};
    if (stream_lzp)
    {
        flags |= archive_flag_stream_lzp;
    }
    write_file_header(output, block_size, input_size, block_count, flags);
    if (stream_lzp)
    {
        write_u64(output, buffer.size());
        write_u32(output, stream_checksum);
        write_byte(output, static_cast<Byte>(stream_bits));
        write_byte(output, 0U);
        write_u16(output, 0U);
    }

    CompressionStats stats;
    stats.input_size = input_size;
    stats.block_count = block_count;

    if (buffered)
    {
        for (const EncodedBlock& block : encoded)
        {
            write_encoded_block(output, block, stats);
        }
    }
    else
    {
        std::uint64_t remaining = input_size;
        const auto read_block = [&](Bytes& original)
        {
            const std::size_t current_size =
                static_cast<std::size_t>(std::min<std::uint64_t>(remaining, block_size));
            original.resize(current_size);
            if (tar_source)
            {
                std::size_t filled = 0;
                while (filled < current_size)
                {
                    const std::size_t produced =
                        tar_source->read(original.data() + filled, current_size - filled);
                    if (produced == 0U)
                    {
                        throw std::runtime_error("directory contents changed during compression");
                    }
                    filled += produced;
                }
            }
            else
            {
                input.read(reinterpret_cast<char*>(original.data()),
                           static_cast<std::streamsize>(current_size));
                if (input.gcount() != static_cast<std::streamsize>(current_size))
                {
                    throw std::runtime_error("input file changed or ended during compression");
                }
            }
            remaining -= current_size;
        };

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
            // Read in order, encode in parallel, write in order.
            struct InFlightBlock
            {
                std::thread worker;
                std::future<EncodedBlock> result;
            };
            std::deque<InFlightBlock> in_flight;
            const auto drain_front = [&]
            {
                InFlightBlock front = std::move(in_flight.front());
                in_flight.pop_front();
                front.worker.join();
                const EncodedBlock block = front.result.get();
                write_encoded_block(output, block, stats);
            };

            try
            {
                for (std::uint32_t block_index = 0; block_index < block_count; ++block_index)
                {
                    if (in_flight.size() >= window)
                    {
                        drain_front();
                    }
                    Bytes original;
                    read_block(original);
                    std::packaged_task<EncodedBlock()> task(
                        [block = std::move(original)]() mutable
                        { return encode_block(std::move(block)); });
                    in_flight.emplace_back();
                    InFlightBlock& slot = in_flight.back();
                    slot.result = task.get_future();
                    try
                    {
                        slot.worker = std::thread(std::move(task));
                    }
                    catch (...)
                    {
                        in_flight.pop_back();
                        throw;
                    }
                }
                while (!in_flight.empty())
                {
                    drain_front();
                }
            }
            catch (...)
            {
                for (InFlightBlock& block : in_flight)
                {
                    if (block.worker.joinable())
                    {
                        block.worker.join();
                    }
                }
                throw;
            }
        }

        if (tar_source)
        {
            Byte probe = 0;
            if (remaining != 0U || tar_source->read(&probe, 1U) != 0U)
            {
                throw std::runtime_error("directory contents changed during compression");
            }
        }
        else if (remaining != 0U || input.peek() != std::char_traits<char>::eof())
        {
            throw std::runtime_error("input file changed during compression");
        }
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
                                 const std::filesystem::path& output_path,
                                 const DecompressionOptions& options)
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
    if (version < oldest_readable_version || version > archive_version)
    {
        throw FormatError("unsupported MZIP version");
    }
    const Byte flags = read_byte(input, "archive flags");
    const std::uint16_t reserved = read_u16(input, "archive flags");
    const Byte flag_limit =
        version >= 2U ? static_cast<Byte>(archive_flag_directory | archive_flag_stream_lzp)
                      : archive_flag_directory;
    if ((flags & static_cast<Byte>(~flag_limit)) != 0U || reserved != 0U)
    {
        throw FormatError("unsupported MZIP archive flags");
    }
    const bool directory_output = (flags & archive_flag_directory) != 0U;
    const bool stream_lzp = (flags & archive_flag_stream_lzp) != 0U;

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

    std::uint64_t coded_size = original_size;
    std::uint32_t stream_checksum = 0;
    unsigned int stream_bits = 0;
    if (stream_lzp)
    {
        coded_size = read_u64(input, "stream size");
        stream_checksum = read_u32(input, "stream checksum");
        stream_bits = read_byte(input, "stream hash bits");
        const Byte reserved_byte = read_byte(input, "stream reserved");
        const std::uint16_t reserved_word = read_u16(input, "stream reserved");
        if (reserved_byte != 0U || reserved_word != 0U)
        {
            throw FormatError("unsupported stream header");
        }
        if (coded_size == 0U || coded_size >= original_size || original_size > stream_lzp_limit)
        {
            throw FormatError("stream size is outside the allowed range");
        }
        if (stream_bits < stream_hash_bits_floor || stream_bits > stream_hash_bits_ceiling)
        {
            throw FormatError("stream hash bits are outside the allowed range");
        }
    }

    std::uint32_t calculated_block_count = 0;
    try
    {
        calculated_block_count = expected_block_count(coded_size, block_size);
    }
    catch (const std::runtime_error& error)
    {
        throw FormatError(error.what());
    }
    if (block_count != calculated_block_count)
    {
        throw FormatError("archive block count does not match its original size");
    }

    std::optional<TemporaryFile> temporary;
    std::ofstream output;
    std::optional<detail::TarExtractor> extractor;
    std::filesystem::path staging_directory;
    bool staging_committed = false;
    if (directory_output)
    {
        std::error_code target_error;
        const auto target_status = std::filesystem::symlink_status(output_path, target_error);
        if (target_status.type() != std::filesystem::file_type::not_found)
        {
            throw std::runtime_error("output path already exists: " + output_path.string());
        }
        staging_directory = unique_sibling_path(output_path, ".tmp.");
        std::filesystem::create_directories(staging_directory);
        extractor.emplace(staging_directory);
    }
    else
    {
        temporary.emplace(unique_sibling_path(output_path, ".tmp."));
        output.open(temporary->path(), std::ios::binary | std::ios::trunc);
        if (!output)
        {
            throw std::runtime_error("cannot create output file: " + temporary->path().string());
        }
    }
    const auto remove_staging = [&]
    {
        if (directory_output && !staging_committed)
        {
            std::error_code cleanup_error;
            std::filesystem::remove_all(detail::native_long_path(staging_directory), cleanup_error);
        }
    };

    CompressionStats stats;
    stats.input_size = archive_size;
    stats.output_size = original_size;
    stats.block_count = block_count;
    std::uint64_t remaining = coded_size;
    std::uint64_t archive_remaining =
        archive_size - file_header_size - (stream_lzp ? stream_header_size : 0U);
    Bytes stream_buffer;
    if (stream_lzp)
    {
        stream_buffer.reserve(static_cast<std::size_t>(coded_size));
    }

    const auto read_block = [&]() -> std::pair<BlockHeader, Bytes>
    {
        if (archive_remaining < block_header_size)
        {
            throw FormatError("truncated archive while reading block header");
        }
        archive_remaining -= block_header_size;
        const BlockHeader header = read_block_header(input, version);
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
        if (header.payload_size > archive_remaining)
        {
            throw FormatError("block payload exceeds the archive size");
        }
        archive_remaining -= header.payload_size;
        Bytes payload = read_bytes(input, header.payload_size, "block payload");
        remaining -= header.original_size;
        increment_mode_count(stats, header.mode);
        return {header, std::move(payload)};
    };

    const auto write_decoded = [&](const Bytes& decoded)
    {
        if (stream_lzp)
        {
            stream_buffer.insert(stream_buffer.end(), decoded.begin(), decoded.end());
        }
        else if (extractor)
        {
            extractor->feed(decoded);
        }
        else
        {
            write_bytes(output, decoded);
            if (!output)
            {
                throw std::runtime_error("failed while writing decompressed output");
            }
        }
    };

    try
    {
        const std::uint32_t thread_count = effective_thread_count(options.thread_count);
        const std::size_t window = static_cast<std::size_t>(
            std::clamp<std::uint64_t>(pipeline_byte_budget / block_size, 1U, thread_count));
        if (window <= 1U || block_count <= 1U)
        {
            for (std::uint32_t block_index = 0; block_index < block_count; ++block_index)
            {
                std::pair<BlockHeader, Bytes> block = read_block();
                write_decoded(decode_block(block.first, std::move(block.second)));
            }
        }
        else
        {
            // Read in order, decode in parallel, write in order.
            struct InFlightBlock
            {
                std::thread worker;
                std::future<Bytes> result;
            };
            std::deque<InFlightBlock> in_flight;
            const auto drain_front = [&]
            {
                InFlightBlock front = std::move(in_flight.front());
                in_flight.pop_front();
                front.worker.join();
                const Bytes decoded = front.result.get();
                write_decoded(decoded);
            };

            try
            {
                for (std::uint32_t block_index = 0; block_index < block_count; ++block_index)
                {
                    if (in_flight.size() >= window)
                    {
                        drain_front();
                    }
                    std::pair<BlockHeader, Bytes> block = read_block();
                    std::packaged_task<Bytes()> task(
                        [header = block.first, payload = std::move(block.second)]() mutable
                        { return decode_block(header, std::move(payload)); });
                    in_flight.emplace_back();
                    InFlightBlock& slot = in_flight.back();
                    slot.result = task.get_future();
                    try
                    {
                        slot.worker = std::thread(std::move(task));
                    }
                    catch (...)
                    {
                        in_flight.pop_back();
                        throw;
                    }
                }
                while (!in_flight.empty())
                {
                    drain_front();
                }
            }
            catch (...)
            {
                for (InFlightBlock& block : in_flight)
                {
                    if (block.worker.joinable())
                    {
                        block.worker.join();
                    }
                }
                throw;
            }
        }

        if (remaining != 0U)
        {
            throw FormatError("archive ended before the declared original size");
        }
        if (input.peek() != std::char_traits<char>::eof())
        {
            throw FormatError("archive contains trailing data");
        }

        if (stream_lzp)
        {
            Bytes restored = detail::lzp_decode(
                stream_buffer, static_cast<std::size_t>(original_size), stream_bits);
            stream_buffer = Bytes();
            if (detail::adler32(restored) != stream_checksum)
            {
                throw FormatError("stream checksum mismatch");
            }
            if (extractor)
            {
                extractor->feed(restored);
            }
            else
            {
                write_bytes(output, restored);
                if (!output)
                {
                    throw std::runtime_error("failed while writing decompressed output");
                }
            }
        }

        if (extractor)
        {
            extractor->finish();
            std::filesystem::rename(staging_directory, output_path);
            staging_committed = true;
        }
        else
        {
            output.close();
            if (!output)
            {
                throw std::runtime_error("failed to finalize decompressed output");
            }
            commit_output(*temporary, output_path);
        }
    }
    catch (...)
    {
        remove_staging();
        throw;
    }
    stats.elapsed_seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - started_at).count();
    return stats;
}

} // namespace mzip
