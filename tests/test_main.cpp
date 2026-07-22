#include <mzip/mzip.hpp>

#include "archive.hpp"
#include "transforms.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <limits>
#include <random>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{

using mzip::detail::Byte;
using mzip::detail::Bytes;

int failure_count = 0;

void fail(const std::string& message, const char* file, const int line)
{
    ++failure_count;
    std::cerr << file << ':' << line << ": " << message << '\n';
}

#define CHECK(condition)                                                                           \
    do                                                                                             \
    {                                                                                              \
        if (!(condition))                                                                          \
        {                                                                                          \
            fail("CHECK failed: " #condition, __FILE__, __LINE__);                                 \
        }                                                                                          \
    } while (false)

#define CHECK_EQ(left, right)                                                                      \
    do                                                                                             \
    {                                                                                              \
        const auto& check_left = (left);                                                           \
        const auto& check_right = (right);                                                         \
        if (!(check_left == check_right))                                                          \
        {                                                                                          \
            fail("CHECK_EQ failed: " #left " != " #right, __FILE__, __LINE__);                     \
        }                                                                                          \
    } while (false)

class TemporaryDirectory
{
public:
    TemporaryDirectory()
    {
        const auto suffix = std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = std::filesystem::temp_directory_path() / ("mzip-tests-" + std::to_string(suffix));
        std::filesystem::create_directories(path_);
    }

    TemporaryDirectory(const TemporaryDirectory&) = delete;
    TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;

    ~TemporaryDirectory()
    {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }

    [[nodiscard]] std::filesystem::path file(const std::string& name) const
    {
        return path_ / name;
    }

private:
    std::filesystem::path path_;
};

[[nodiscard]] Bytes to_bytes(const std::string_view text)
{
    Bytes result;
    result.reserve(text.size());
    for (const char character : text)
    {
        result.push_back(static_cast<Byte>(static_cast<unsigned char>(character)));
    }
    return result;
}

[[nodiscard]] Bytes repeated_text(const std::size_t minimum_size)
{
    constexpr std::string_view paragraph =
        "Burrows-Wheeler groups related symbols. Move-to-front exposes locality, and run-length "
        "encoding makes repeated values compact. An adaptive range coder finishes the block.\n";
    Bytes result;
    while (result.size() < minimum_size)
    {
        const Bytes part = to_bytes(paragraph);
        result.insert(result.end(), part.begin(), part.end());
    }
    return result;
}

[[nodiscard]] Bytes random_bytes(const std::size_t size, const std::uint32_t seed)
{
    std::mt19937 generator(seed);
    std::uniform_int_distribution<unsigned int> distribution(0U, 255U);
    Bytes result(size);
    for (Byte& value : result)
    {
        value = static_cast<Byte>(distribution(generator));
    }
    return result;
}

void write_file(const std::filesystem::path& path, const std::span<const Byte> data)
{
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output)
    {
        throw std::runtime_error("cannot create test file");
    }
    if (!data.empty())
    {
        output.write(reinterpret_cast<const char*>(data.data()),
                     static_cast<std::streamsize>(data.size()));
    }
    if (!output)
    {
        throw std::runtime_error("cannot write test file");
    }
}

[[nodiscard]] Bytes read_file(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    if (!input)
    {
        throw std::runtime_error("cannot open test file");
    }
    input.seekg(0, std::ios::end);
    const std::streamoff end = input.tellg();
    if (end < 0)
    {
        throw std::runtime_error("cannot determine test file size");
    }
    input.seekg(0, std::ios::beg);
    Bytes result(static_cast<std::size_t>(end));
    if (!result.empty())
    {
        input.read(reinterpret_cast<char*>(result.data()),
                   static_cast<std::streamsize>(result.size()));
    }
    if (!input && !result.empty())
    {
        throw std::runtime_error("cannot read test file");
    }
    return result;
}

void write_u32(Bytes& bytes, const std::size_t offset, const std::uint32_t value)
{
    CHECK(offset <= bytes.size() && bytes.size() - offset >= 4U);
    for (unsigned int index = 0; index < 4U; ++index)
    {
        bytes[offset + index] = static_cast<Byte>((value >> (index * 8U)) & 0xFFU);
    }
}

template <typename Function> void expect_format_error(Function&& function)
{
    bool threw = false;
    try
    {
        std::forward<Function>(function)();
    }
    catch (const mzip::FormatError&)
    {
        threw = true;
    }
    CHECK(threw);
}

void test_bwt_round_trip()
{
    std::vector<Bytes> cases{{},
                             Bytes{0U},
                             Bytes{1U, 1U, 1U, 1U},
                             to_bytes("banana"),
                             to_bytes("mississippi"),
                             random_bytes(257U, 1U),
                             random_bytes(4096U, 2U)};

    Bytes every_byte;
    for (unsigned int value = 0; value < 256U; ++value)
    {
        every_byte.push_back(static_cast<Byte>(value));
    }
    cases.push_back(every_byte);

    for (const Bytes& input : cases)
    {
        const auto encoded = mzip::detail::bwt_encode(input);
        CHECK_EQ(encoded.data.size(), input.size());
        CHECK_EQ(mzip::detail::bwt_decode(encoded.data, encoded.primary_index), input);
    }

    const auto banana = mzip::detail::bwt_encode(to_bytes("banana"));
    CHECK_EQ(banana.data, to_bytes("annbaa"));
    CHECK_EQ(banana.primary_index, 4U);

    // Valid primary range is 1..size.
    expect_format_error([] { static_cast<void>(mzip::detail::bwt_decode(Bytes{1U}, 0U)); });
    expect_format_error([] { static_cast<void>(mzip::detail::bwt_decode(Bytes{1U}, 2U)); });
}

void test_bwt_randomized()
{
    std::mt19937 generator(20260722U);
    for (unsigned int round = 0; round < 200U; ++round)
    {
        std::uniform_int_distribution<std::size_t> size_distribution(0U, 300U);
        std::uniform_int_distribution<unsigned int> alphabet_distribution(1U, 4U);
        const std::size_t size = size_distribution(generator);
        const unsigned int alphabet = 1U << alphabet_distribution(generator);

        Bytes input(size);
        std::uniform_int_distribution<unsigned int> value_distribution(0U, alphabet - 1U);
        for (Byte& value : input)
        {
            value = static_cast<Byte>(value_distribution(generator));
        }
        const auto encoded = mzip::detail::bwt_encode(input);
        CHECK_EQ(mzip::detail::bwt_decode(encoded.data, encoded.primary_index), input);
    }

    for (const std::size_t period : {1U, 2U, 3U, 5U, 16U})
    {
        Bytes periodic;
        for (std::size_t index = 0; index < 2048U; ++index)
        {
            periodic.push_back(static_cast<Byte>('a' + index % period));
        }
        const auto encoded = mzip::detail::bwt_encode(periodic);
        CHECK_EQ(mzip::detail::bwt_decode(encoded.data, encoded.primary_index), periodic);
    }
}

void test_mtf_round_trip()
{
    const std::vector<Bytes> cases{
        {}, Bytes{0U}, Bytes(1000U, 255U), to_bytes("abracadabra"), random_bytes(8192U, 3U)};
    for (const Bytes& input : cases)
    {
        Bytes data = input;
        mzip::detail::mtf_encode(data);
        mzip::detail::mtf_decode(data);
        CHECK_EQ(data, input);
    }
}

void test_stream_codec_round_trip()
{
    Bytes boundaries;
    for (const std::size_t length :
         {1U, 2U, 3U, 4U, 5U, 6U, 7U, 8U, 15U, 16U, 31U, 127U, 128U, 255U, 256U, 1000U})
    {
        boundaries.insert(boundaries.end(), length, Byte{0});
        boundaries.insert(boundaries.end(), length, static_cast<Byte>(1U + length % 255U));
        boundaries.push_back(static_cast<Byte>(1U + (length + 1U) % 255U));
    }

    Bytes dna;
    std::mt19937 dna_generator(11U);
    std::uniform_int_distribution<unsigned int> base(1U, 4U);
    for (unsigned int index = 0; index < 20'000U; ++index)
    {
        dna.push_back(static_cast<Byte>(base(dna_generator)));
    }

    Bytes mostly_zero = random_bytes(16'384U, 4U);
    for (std::size_t index = 0; index < mostly_zero.size(); ++index)
    {
        if (index % 3U != 0U)
        {
            mostly_zero[index] = 0U;
        }
    }

    constexpr std::size_t no_limit = std::numeric_limits<std::size_t>::max();
    const std::vector<Bytes> cases{
        {},          Bytes{7U}, Bytes(10'000U, 0U),       Bytes(10'000U, 9U), boundaries,
        mostly_zero, dna,       random_bytes(16'384U, 5U)};
    for (const Bytes& input : cases)
    {
        for (const std::size_t threshold : {2U, 3U})
        {
            const auto encoded = mzip::detail::rc_encode(input, threshold, no_limit);
            CHECK(encoded.has_value());
            CHECK(encoded->symbol_count <= input.size());
            CHECK_EQ(mzip::detail::rc_decode(encoded->payload, encoded->symbol_count, input.size()),
                     input);
        }
    }

    CHECK(!mzip::detail::rc_encode(random_bytes(4096U, 13U), 2U, 64U).has_value());

    const Bytes sample = repeated_text(5'000U);
    const auto encoded = mzip::detail::rc_encode(sample, 2U, no_limit);
    CHECK(encoded.has_value());
    const Bytes payload = encoded->payload;
    const std::size_t symbol_count = encoded->symbol_count;
    expect_format_error(
        [&]
        {
            const Bytes truncated(payload.begin(), payload.begin() + 3);
            static_cast<void>(mzip::detail::rc_decode(truncated, symbol_count, sample.size()));
        });
    expect_format_error(
        [&]
        {
            Bytes truncated = payload;
            truncated.resize(truncated.size() / 2U);
            static_cast<void>(mzip::detail::rc_decode(truncated, symbol_count, sample.size()));
        });
    expect_format_error(
        [&]
        {
            Bytes trailing = payload;
            trailing.push_back(0U);
            static_cast<void>(mzip::detail::rc_decode(trailing, symbol_count, sample.size()));
        });
    expect_format_error(
        [&]
        { static_cast<void>(mzip::detail::rc_decode(payload, symbol_count - 1U, sample.size())); });
    expect_format_error(
        [&]
        { static_cast<void>(mzip::detail::rc_decode(payload, symbol_count, sample.size() - 1U)); });
    expect_format_error([] { static_cast<void>(mzip::detail::rc_decode(Bytes{}, 1U, 1U)); });
    expect_format_error([] { static_cast<void>(mzip::detail::rc_decode(Bytes{1U}, 0U, 0U)); });
}

void test_file_round_trip_and_determinism()
{
    TemporaryDirectory directory;
    std::vector<Bytes> cases{{},
                             Bytes{0U},
                             Bytes(20'000U, 0U),
                             Bytes(20'000U, 255U),
                             repeated_text(40'000U),
                             random_bytes(20'000U, 6U)};

    Bytes every_byte;
    for (unsigned int repetition = 0; repetition < 20U; ++repetition)
    {
        for (unsigned int value = 0; value < 256U; ++value)
        {
            every_byte.push_back(static_cast<Byte>(value));
        }
    }
    cases.push_back(every_byte);

    mzip::CompressionOptions options;
    options.block_size = mzip::minimum_block_size;
    for (std::size_t index = 0; index < cases.size(); ++index)
    {
        const auto source = directory.file("source-" + std::to_string(index) + ".bin");
        const auto archive = directory.file("archive-" + std::to_string(index) + ".mz");
        const auto second_archive = directory.file("archive-" + std::to_string(index) + "-2.mz");
        const auto restored = directory.file("restored-" + std::to_string(index) + ".bin");
        write_file(source, cases[index]);

        const auto compression_stats = mzip::compress_file(source, archive, options);
        const auto decompression_stats = mzip::decompress_file(archive, restored);
        CHECK_EQ(read_file(restored), cases[index]);
        CHECK_EQ(compression_stats.input_size, cases[index].size());
        CHECK_EQ(decompression_stats.output_size, cases[index].size());

        static_cast<void>(mzip::compress_file(source, second_archive, options));
        CHECK_EQ(read_file(second_archive), read_file(archive));
    }
}

void test_block_mode_selection()
{
    TemporaryDirectory directory;
    mzip::CompressionOptions options;
    options.block_size = 4096U;

    const auto random_source = directory.file("random.bin");
    const auto random_archive = directory.file("random.mz");
    write_file(random_source, random_bytes(16'384U, 7U));
    const auto random_stats = mzip::compress_file(random_source, random_archive, options);
    CHECK(random_stats.raw_blocks > 0U);

    const auto text_source = directory.file("text.txt");
    const auto text_archive = directory.file("text.mz");
    write_file(text_source, repeated_text(32'768U));
    const auto text_stats = mzip::compress_file(text_source, text_archive, options);
    CHECK(text_stats.transformed_blocks > 0U);
    CHECK(text_stats.output_size < text_stats.input_size);
}

void test_corrupt_archives_do_not_replace_output()
{
    TemporaryDirectory directory;
    const auto source = directory.file("source.txt");
    const auto archive = directory.file("source.mz");
    write_file(source, repeated_text(50'000U));
    const auto stats = mzip::compress_file(source, archive);
    CHECK(stats.transformed_blocks > 0U);
    const Bytes valid = read_file(archive);
    CHECK(valid.size() > 48U);

    const Bytes sentinel = to_bytes("existing output must survive");
    std::size_t case_index = 0;
    const auto verify_failure = [&](Bytes damaged)
    {
        const auto damaged_path = directory.file("damaged-" + std::to_string(case_index) + ".mz");
        const auto output_path = directory.file("output-" + std::to_string(case_index) + ".bin");
        ++case_index;
        write_file(damaged_path, damaged);
        write_file(output_path, sentinel);
        expect_format_error(
            [&] { static_cast<void>(mzip::decompress_file(damaged_path, output_path)); });
        CHECK_EQ(read_file(output_path), sentinel);
    };

    Bytes wrong_magic = valid;
    wrong_magic[0] = static_cast<Byte>('X');
    verify_failure(std::move(wrong_magic));

    Bytes wrong_version = valid;
    wrong_version[4] = 99U;
    verify_failure(std::move(wrong_version));

    Bytes truncated = valid;
    truncated.pop_back();
    verify_failure(std::move(truncated));

    Bytes wrong_primary = valid;
    write_u32(wrong_primary, 36U, std::numeric_limits<std::uint32_t>::max());
    verify_failure(std::move(wrong_primary));

    Bytes wrong_payload_size = valid;
    write_u32(wrong_payload_size, 32U, std::numeric_limits<std::uint32_t>::max());
    verify_failure(std::move(wrong_payload_size));

    Bytes wrong_checksum = valid;
    wrong_checksum[44] ^= 1U;
    verify_failure(std::move(wrong_checksum));

    Bytes trailing = valid;
    trailing.push_back(0U);
    verify_failure(std::move(trailing));
}

void test_random_corruption_is_rejected_safely()
{
    TemporaryDirectory directory;
    const auto source = directory.file("source.bin");
    Bytes data = repeated_text(60'000U);
    const Bytes noise = random_bytes(20'000U, 9U);
    data.insert(data.end(), noise.begin(), noise.end());
    write_file(source, data);

    mzip::CompressionOptions options;
    options.block_size = 8192U;
    const auto archive = directory.file("source.mz");
    static_cast<void>(mzip::compress_file(source, archive, options));
    const Bytes valid = read_file(archive);

    // Damage must produce FormatError or the exact original, never a crash.
    std::mt19937 generator(20260722U);
    std::uniform_int_distribution<std::size_t> position_distribution(0U, valid.size() - 1U);
    std::uniform_int_distribution<unsigned int> bit_distribution(0U, 7U);
    const auto restored = directory.file("restored.bin");
    for (unsigned int round = 0; round < 300U; ++round)
    {
        Bytes damaged = valid;
        if (round % 3U == 0U)
        {
            damaged.resize(position_distribution(generator));
        }
        else
        {
            const std::size_t position = position_distribution(generator);
            damaged[position] =
                static_cast<Byte>(damaged[position] ^ (1U << bit_distribution(generator)));
        }
        const auto damaged_path = directory.file("damaged.mz");
        write_file(damaged_path, damaged);
        try
        {
            static_cast<void>(mzip::decompress_file(damaged_path, restored));
            CHECK_EQ(read_file(restored), data);
        }
        catch (const mzip::FormatError&)
        {
        }
    }
}

// Format pin: any change to these bytes is a format break and needs a version bump.
void test_golden_archive()
{
    Bytes input;
    for (unsigned int index = 0; index < 600U; ++index)
    {
        input.push_back(static_cast<Byte>('a' + index % 26U));
    }
    input.insert(input.end(), 500U, Byte{0U});
    input.insert(input.end(), 300U, Byte{'x'});
    for (unsigned int index = 0; index < 700U; ++index)
    {
        input.push_back(static_cast<Byte>(index * 37U % 251U));
    }
    const Bytes filler = to_bytes("The golden archive pins MZIP format version 1. ");
    while (input.size() < 2600U)
    {
        input.insert(input.end(), filler.begin(), filler.end());
    }
    input.resize(2600U);

    static const unsigned char golden[] = {
        0x4DU, 0x5AU, 0x49U, 0x50U, 0x01U, 0x00U, 0x00U, 0x00U, 0x00U, 0x04U, 0x00U, 0x00U, 0x28U,
        0x0AU, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x03U, 0x00U, 0x00U, 0x00U, 0x01U, 0x00U,
        0x00U, 0x00U, 0x00U, 0x04U, 0x00U, 0x00U, 0x35U, 0x00U, 0x00U, 0x00U, 0xC0U, 0x01U, 0x00U,
        0x00U, 0x8BU, 0x00U, 0x00U, 0x00U, 0x9CU, 0x00U, 0x53U, 0xD3U, 0x00U, 0x41U, 0x2FU, 0xE1U,
        0x74U, 0xE5U, 0x3AU, 0x92U, 0xF5U, 0x64U, 0x46U, 0xA9U, 0x03U, 0x70U, 0x96U, 0xBCU, 0x5DU,
        0x7AU, 0x29U, 0xBEU, 0x6AU, 0x47U, 0xC4U, 0x5DU, 0x2BU, 0xF4U, 0xB1U, 0x5FU, 0x5AU, 0xE2U,
        0x63U, 0x83U, 0x8FU, 0xA0U, 0x52U, 0x6DU, 0x49U, 0x9AU, 0xA3U, 0x11U, 0x93U, 0x81U, 0xBDU,
        0x73U, 0xCEU, 0x50U, 0x45U, 0x5CU, 0x0FU, 0xFDU, 0x21U, 0x1DU, 0x00U, 0x01U, 0x00U, 0x00U,
        0x00U, 0x00U, 0x04U, 0x00U, 0x00U, 0x1DU, 0x01U, 0x00U, 0x00U, 0x01U, 0x00U, 0x00U, 0x00U,
        0x0AU, 0x02U, 0x00U, 0x00U, 0xC0U, 0xC7U, 0xD3U, 0xDCU, 0x00U, 0xAFU, 0x60U, 0x1AU, 0x0BU,
        0xBCU, 0x17U, 0x60U, 0x9AU, 0x02U, 0x6EU, 0xD6U, 0x38U, 0x1CU, 0x83U, 0x2BU, 0x4DU, 0xB1U,
        0xFEU, 0x33U, 0x14U, 0xE7U, 0x8CU, 0x19U, 0x1CU, 0x9DU, 0xDAU, 0xB2U, 0x4BU, 0x14U, 0x9EU,
        0x7FU, 0xB2U, 0xA4U, 0x22U, 0xBDU, 0x83U, 0x39U, 0x52U, 0xE7U, 0xB6U, 0xBAU, 0x79U, 0xDAU,
        0x98U, 0x77U, 0x56U, 0x2EU, 0x1CU, 0xF6U, 0x83U, 0x71U, 0xC0U, 0x16U, 0x71U, 0xBCU, 0x02U,
        0x65U, 0x7EU, 0x4DU, 0x56U, 0x06U, 0x6BU, 0xA4U, 0xAEU, 0x4AU, 0x65U, 0xABU, 0x33U, 0xEFU,
        0xB9U, 0x44U, 0xB7U, 0x0FU, 0x77U, 0x4CU, 0x45U, 0x12U, 0xB9U, 0xCDU, 0x6FU, 0x95U, 0x48U,
        0x38U, 0xEEU, 0xE7U, 0x1FU, 0x6EU, 0x3EU, 0xCCU, 0x9EU, 0xD2U, 0x5EU, 0x7BU, 0xBCU, 0xC7U,
        0x20U, 0x99U, 0xD7U, 0x0BU, 0x2AU, 0x25U, 0x63U, 0xB7U, 0xD5U, 0x36U, 0xAAU, 0x77U, 0x87U,
        0xA5U, 0x85U, 0x05U, 0x74U, 0xE4U, 0x6DU, 0x83U, 0xC0U, 0x0BU, 0xE5U, 0xDDU, 0x7FU, 0x80U,
        0x3FU, 0x29U, 0xD2U, 0xCCU, 0x89U, 0x40U, 0xF9U, 0x62U, 0xCDU, 0x8FU, 0x78U, 0x37U, 0x9DU,
        0xA0U, 0x34U, 0x30U, 0x3FU, 0x4AU, 0xBDU, 0x72U, 0x5DU, 0xE5U, 0xB0U, 0xF2U, 0x7AU, 0x2CU,
        0x7CU, 0xBBU, 0x18U, 0x58U, 0x1AU, 0xB5U, 0x91U, 0xDDU, 0x77U, 0x75U, 0x7DU, 0x56U, 0x0AU,
        0x42U, 0x67U, 0xDDU, 0x33U, 0x84U, 0xEBU, 0xB2U, 0x2DU, 0xFBU, 0x50U, 0xFDU, 0x06U, 0x33U,
        0xFFU, 0xC7U, 0x07U, 0xCDU, 0x5BU, 0xF9U, 0x40U, 0x65U, 0x91U, 0x4EU, 0xDCU, 0x87U, 0xFBU,
        0xB3U, 0xCEU, 0xCDU, 0x1CU, 0x08U, 0x42U, 0xE6U, 0xD1U, 0x7AU, 0x70U, 0x2DU, 0x8DU, 0xEBU,
        0xE6U, 0xEDU, 0xFDU, 0x74U, 0xC2U, 0xEEU, 0x21U, 0x4AU, 0xC6U, 0x48U, 0x8CU, 0xBBU, 0x7CU,
        0x6FU, 0x76U, 0x09U, 0x1DU, 0x5EU, 0x97U, 0x3DU, 0x7DU, 0x4FU, 0x48U, 0xA6U, 0xCDU, 0x42U,
        0x4BU, 0xC0U, 0xD7U, 0x08U, 0xCAU, 0x7BU, 0x85U, 0x2AU, 0x7FU, 0x56U, 0xC1U, 0x2FU, 0x86U,
        0x9AU, 0xC2U, 0x85U, 0xC5U, 0x6FU, 0x94U, 0xDEU, 0x3CU, 0xA9U, 0xC4U, 0x47U, 0x31U, 0x24U,
        0x3CU, 0x7CU, 0x88U, 0xC8U, 0x43U, 0x5EU, 0x3AU, 0xD0U, 0x76U, 0xBEU, 0x83U, 0x30U, 0xC5U,
        0x53U, 0x2AU, 0x14U, 0x32U, 0xBBU, 0x07U, 0xCCU, 0x9CU, 0xA5U, 0x61U, 0xB2U, 0x3EU, 0x70U,
        0xE5U, 0xBDU, 0x1CU, 0xDEU, 0x63U, 0xFEU, 0x00U, 0x01U, 0x00U, 0x00U, 0x00U, 0x28U, 0x02U,
        0x00U, 0x00U, 0x90U, 0x00U, 0x00U, 0x00U, 0x0EU, 0x02U, 0x00U, 0x00U, 0xEEU, 0x00U, 0x00U,
        0x00U, 0x7BU, 0xC8U, 0xE5U, 0xE6U, 0x00U, 0xB3U, 0x7BU, 0x3DU, 0xE5U, 0xD5U, 0x26U, 0x2DU,
        0x02U, 0x4EU, 0x8FU, 0xEFU, 0xA4U, 0xF9U, 0x24U, 0x87U, 0xB4U, 0xD6U, 0x4EU, 0x50U, 0x73U,
        0xA2U, 0xA0U, 0x36U, 0x22U, 0xA1U, 0xACU, 0xC4U, 0x50U, 0x3CU, 0x06U, 0x29U, 0x6DU, 0x75U,
        0x0EU, 0x14U, 0x5BU, 0xCAU, 0x44U, 0x88U, 0x26U, 0xB1U, 0x7DU, 0x5CU, 0xF4U, 0x6FU, 0x5FU,
        0xD6U, 0xACU, 0x2AU, 0x6FU, 0x78U, 0xF9U, 0x29U, 0xB8U, 0x76U, 0x92U, 0xB5U, 0xA1U, 0xA2U,
        0xFBU, 0xAAU, 0xC6U, 0x8EU, 0x8AU, 0x15U, 0x6FU, 0x4DU, 0xAEU, 0x45U, 0x34U, 0xCAU, 0x14U,
        0x08U, 0x6EU, 0x21U, 0xB8U, 0xCCU, 0x6AU, 0x69U, 0x9AU, 0xAFU, 0xB3U, 0xFCU, 0x79U, 0x78U,
        0x0CU, 0xDBU, 0x0FU, 0xE2U, 0xDEU, 0xBEU, 0x19U, 0x7EU, 0xB7U, 0x69U, 0x26U, 0x96U, 0xBFU,
        0x31U, 0x15U, 0xFFU, 0x5EU, 0x98U, 0xC3U, 0xC7U, 0x57U, 0xF3U, 0xFAU, 0x00U, 0x30U, 0x59U,
        0xEBU, 0x77U, 0x88U, 0x6CU, 0xE2U, 0x40U, 0x2BU, 0x2DU, 0xE1U, 0x7FU, 0x30U, 0x44U, 0xD3U,
        0x7AU, 0xFAU, 0xB8U, 0xD0U, 0x79U, 0x83U, 0x92U, 0xE7U, 0xEDU, 0xF0U, 0x46U, 0x44U, 0x16U,
        0x80U, 0xD9U, 0xF2U, 0x90U, 0x34U, 0x98U};
    const Bytes expected(golden, golden + sizeof(golden));

    TemporaryDirectory directory;
    const auto source = directory.file("golden.bin");
    write_file(source, input);
    const auto archive = directory.file("golden.mz");
    mzip::CompressionOptions options;
    options.block_size = 1024U;
    options.thread_count = 1U;
    static_cast<void>(mzip::compress_file(source, archive, options));
    CHECK_EQ(read_file(archive), expected);

    const auto pinned = directory.file("pinned.mz");
    write_file(pinned, expected);
    const auto restored = directory.file("restored.bin");
    static_cast<void>(mzip::decompress_file(pinned, restored));
    CHECK_EQ(read_file(restored), input);
}

void test_directory_output_is_rejected()
{
    TemporaryDirectory directory;
    const auto source = directory.file("source.bin");
    write_file(source, repeated_text(2'000U));
    const auto directory_target = directory.file("target-dir");
    std::filesystem::create_directory(directory_target);
    const auto keepsake = directory_target / "keep.txt";
    write_file(keepsake, to_bytes("still here"));

    bool threw = false;
    try
    {
        static_cast<void>(mzip::compress_file(source, directory_target));
    }
    catch (const std::runtime_error&)
    {
        threw = true;
    }
    CHECK(threw);
    CHECK(std::filesystem::is_directory(directory_target));
    CHECK_EQ(read_file(keepsake), to_bytes("still here"));

    const auto archive = directory.file("source.mz");
    static_cast<void>(mzip::compress_file(source, archive));
    threw = false;
    try
    {
        static_cast<void>(mzip::decompress_file(archive, directory_target));
    }
    catch (const std::runtime_error&)
    {
        threw = true;
    }
    CHECK(threw);
    CHECK(std::filesystem::is_directory(directory_target));
    CHECK_EQ(read_file(keepsake), to_bytes("still here"));
}

void test_hostile_archives_are_rejected()
{
    TemporaryDirectory directory;
    const auto output = directory.file("output.bin");
    const auto archive = directory.file("hostile.mz");
    const Bytes sentinel = to_bytes("must survive");

    const auto expect_rejected = [&](const Bytes& bytes)
    {
        write_file(archive, bytes);
        write_file(output, sentinel);
        expect_format_error([&] { static_cast<void>(mzip::decompress_file(archive, output)); });
        CHECK_EQ(read_file(output), sentinel);
    };

    for (const std::uint32_t seed : {21U, 22U, 23U})
    {
        Bytes garbage = random_bytes(4096U, seed);
        expect_rejected(garbage);
        garbage[0] = 'M';
        garbage[1] = 'Z';
        garbage[2] = 'I';
        garbage[3] = 'P';
        garbage[4] = 1U;
        expect_rejected(garbage);
    }
    expect_rejected(Bytes{});
    expect_rejected(to_bytes("MZIP"));

    // Declared-size bombs must fail before any big allocation.
    Bytes bomb;
    const auto push_u32 = [&bomb](const std::uint32_t value)
    {
        for (unsigned int index = 0; index < 4U; ++index)
        {
            bomb.push_back(static_cast<Byte>((value >> (index * 8U)) & 0xFFU));
        }
    };
    bomb = to_bytes("MZIP");
    bomb.push_back(1U);
    bomb.push_back(0U);
    bomb.push_back(0U);
    bomb.push_back(0U);
    push_u32(64U * 1024U * 1024U);
    push_u32(64U * 1024U * 1024U);
    push_u32(0U);
    push_u32(1U);
    push_u32(1U);
    push_u32(64U * 1024U * 1024U);
    push_u32(64U * 1024U * 1024U);
    push_u32(1U);
    push_u32(64U * 1024U * 1024U);
    push_u32(0U);
    bomb.push_back(0U);
    expect_rejected(bomb);

    // Every header-byte flip must be caught by some layer.
    const auto source = directory.file("source.bin");
    write_file(source, repeated_text(30'000U));
    const auto valid_archive = directory.file("valid.mz");
    static_cast<void>(mzip::compress_file(source, valid_archive));
    const Bytes valid = read_file(valid_archive);
    const Bytes original = read_file(source);
    for (std::size_t offset = 0; offset < 48U && offset < valid.size(); ++offset)
    {
        Bytes damaged = valid;
        damaged[offset] ^= 0xFFU;
        write_file(archive, damaged);
        write_file(output, sentinel);
        try
        {
            static_cast<void>(mzip::decompress_file(archive, output));
            CHECK_EQ(read_file(output), original);
        }
        catch (const mzip::FormatError&)
        {
            CHECK_EQ(read_file(output), sentinel);
        }
    }
}

void test_directory_archive_round_trip()
{
    TemporaryDirectory directory;
    const auto tree = directory.file("tree");
    std::filesystem::create_directories(tree / "nested" / "deep");
    std::filesystem::create_directories(tree / "empty-dir");
    write_file(tree / "a.txt", repeated_text(3'000U));
    write_file(tree / "empty.bin", {});
    write_file(tree / "nested" / "deep" / "b.bin", random_bytes(5'000U, 30U));
    const std::string long_name(180U, 'n');
    write_file(tree / (long_name + ".txt"), to_bytes("long name content"));

    const auto archive = directory.file("tree.mz");
    const auto stats = mzip::compress_file(tree, archive);
    CHECK(stats.input_size > 0U);

    const auto archive_again = directory.file("tree2.mz");
    static_cast<void>(mzip::compress_file(tree, archive_again));
    CHECK_EQ(read_file(archive), read_file(archive_again));

    const auto restored = directory.file("restored");
    static_cast<void>(mzip::decompress_file(archive, restored));

    const auto restored_tree = restored / "tree";
    CHECK(std::filesystem::is_directory(restored_tree / "empty-dir"));
    CHECK_EQ(read_file(restored_tree / "a.txt"), read_file(tree / "a.txt"));
    CHECK_EQ(read_file(restored_tree / "empty.bin"), Bytes{});
    CHECK_EQ(read_file(restored_tree / "nested" / "deep" / "b.bin"),
             read_file(tree / "nested" / "deep" / "b.bin"));
    CHECK_EQ(read_file(restored_tree / (long_name + ".txt")), to_bytes("long name content"));

    std::size_t restored_count = 0;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(restored_tree))
    {
        static_cast<void>(entry);
        ++restored_count;
    }
    CHECK_EQ(restored_count, std::size_t{7});

    bool threw = false;
    try
    {
        static_cast<void>(mzip::decompress_file(archive, restored));
    }
    catch (const std::runtime_error&)
    {
        threw = true;
    }
    CHECK(threw);
}

void test_hostile_tar_entries_are_rejected()
{
    using mzip::detail::TarExtractor;

    const auto make_header = [](const std::string& name)
    {
        Bytes block(512U, 0U);
        std::copy(name.begin(), name.end(), block.begin());
        const std::string size_field = "00000000000";
        std::copy(size_field.begin(), size_field.end(), block.begin() + 124);
        block[156] = static_cast<Byte>('0');
        return block;
    };

    TemporaryDirectory directory;
    const auto target = directory.file("out");
    std::filesystem::create_directories(target);

    expect_format_error(
        [&]
        {
            TarExtractor extractor(target);
            extractor.feed(make_header("../escape.txt"));
        });
    expect_format_error(
        [&]
        {
            TarExtractor extractor(target);
            extractor.feed(make_header("/absolute.txt"));
        });
    expect_format_error(
        [&]
        {
            TarExtractor extractor(target);
            extractor.feed(make_header("nested/../../escape.txt"));
        });
#if defined(_WIN32)
    for (const char* name :
         {"trailing.", "trailing ", "drive:stream.txt", "CON", "sub/NUL.txt", "LPT1"})
    {
        expect_format_error(
            [&]
            {
                TarExtractor extractor(target);
                extractor.feed(make_header(name));
            });
    }

    using mzip::detail::native_long_path;
    const std::filesystem::path local_form(LR"(\\?\C:\data\file.bin)");
    const std::filesystem::path share_form(LR"(\\?\UNC\server\share\file)");
    const std::filesystem::path prefixed_form(LR"(\\?\C:\already\long)");
    CHECK_EQ(native_long_path("C:\\data\\file.bin"), local_form);
    CHECK_EQ(native_long_path(LR"(\\server\share\file)"), share_form);
    CHECK_EQ(native_long_path(prefixed_form), prefixed_form);
#endif
    expect_format_error(
        [&]
        {
            TarExtractor extractor(target);
            extractor.finish();
        });
}

void test_thread_count_does_not_change_output()
{
    TemporaryDirectory directory;
    const auto source = directory.file("threaded.bin");
    Bytes data = repeated_text(200'000U);
    const Bytes noise = random_bytes(50'000U, 8U);
    data.insert(data.end(), noise.begin(), noise.end());
    write_file(source, data);

    mzip::CompressionOptions single;
    single.block_size = 4096U;
    single.thread_count = 1U;
    mzip::CompressionOptions parallel;
    parallel.block_size = 4096U;
    parallel.thread_count = 8U;

    const auto single_archive = directory.file("single.mz");
    const auto parallel_archive = directory.file("parallel.mz");
    const auto single_stats = mzip::compress_file(source, single_archive, single);
    const auto parallel_stats = mzip::compress_file(source, parallel_archive, parallel);
    CHECK(single_stats.block_count > 8U);
    CHECK_EQ(single_stats.block_count, parallel_stats.block_count);
    CHECK_EQ(read_file(single_archive), read_file(parallel_archive));

    const auto restored = directory.file("restored.bin");
    static_cast<void>(mzip::decompress_file(parallel_archive, restored));
    CHECK_EQ(read_file(restored), data);

    mzip::DecompressionOptions sequential;
    sequential.thread_count = 1U;
    const auto restored_sequential = directory.file("restored-single.bin");
    static_cast<void>(mzip::decompress_file(single_archive, restored_sequential, sequential));
    CHECK_EQ(read_file(restored_sequential), data);
}

void test_same_input_and_output_is_rejected()
{
    TemporaryDirectory directory;
    const auto path = directory.file("same.bin");
    write_file(path, to_bytes("data"));
    bool threw = false;
    try
    {
        static_cast<void>(mzip::compress_file(path, path));
    }
    catch (const std::invalid_argument&)
    {
        threw = true;
    }
    CHECK(threw);
    CHECK_EQ(read_file(path), to_bytes("data"));
}

void run_test(const char* name, const std::function<void()>& test)
{
    const int failures_before = failure_count;
    try
    {
        test();
    }
    catch (const std::exception& error)
    {
        fail(std::string("unexpected exception: ") + error.what(), __FILE__, __LINE__);
    }
    if (failure_count == failures_before)
    {
        std::cout << "[PASS] " << name << '\n';
    }
    else
    {
        std::cout << "[FAIL] " << name << '\n';
    }
}

} // namespace

int main()
{
    run_test("BWT round-trip", test_bwt_round_trip);
    run_test("BWT randomized round-trip", test_bwt_randomized);
    run_test("MTF round-trip", test_mtf_round_trip);
    run_test("stream codec round-trip", test_stream_codec_round_trip);
    run_test("file round-trip and determinism", test_file_round_trip_and_determinism);
    run_test("block mode selection", test_block_mode_selection);
    run_test("corrupt archive handling", test_corrupt_archives_do_not_replace_output);
    run_test("random corruption safety", test_random_corruption_is_rejected_safely);
    run_test("golden archive", test_golden_archive);
    run_test("directory archive round-trip", test_directory_archive_round_trip);
    run_test("hostile tar entry rejection", test_hostile_tar_entries_are_rejected);
    run_test("directory output rejection", test_directory_output_is_rejected);
    run_test("hostile archive rejection", test_hostile_archives_are_rejected);
    run_test("thread count does not change output", test_thread_count_does_not_change_output);
    run_test("same input/output rejection", test_same_input_and_output_is_rejected);

    if (failure_count != 0)
    {
        std::cerr << failure_count << " test assertion(s) failed\n";
        return 1;
    }
    std::cout << "All tests passed\n";
    return 0;
}
