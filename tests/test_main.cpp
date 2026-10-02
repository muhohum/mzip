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
#include <optional>
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

// The transform as a definition: suffixes sorted with a proper prefix first, the sentinel's
// own suffix in row 0, the row of suffix 0 left out as the primary index.
[[nodiscard]] mzip::detail::BwtResult naive_bwt(const Bytes& input)
{
    std::vector<std::uint32_t> suffixes(input.size());
    for (std::uint32_t index = 0; index < suffixes.size(); ++index)
    {
        suffixes[index] = index;
    }
    std::sort(suffixes.begin(), suffixes.end(),
              [&](const std::uint32_t left, const std::uint32_t right)
              {
                  return std::lexicographical_compare(input.begin() + left, input.end(),
                                                      input.begin() + right, input.end());
              });
    mzip::detail::BwtResult result;
    if (input.empty())
    {
        return result;
    }
    result.data.push_back(input.back());
    for (std::size_t rank = 0; rank < suffixes.size(); ++rank)
    {
        if (suffixes[rank] == 0U)
        {
            result.primary_index = static_cast<std::uint32_t>(rank + 1U);
            continue;
        }
        result.data.push_back(input[suffixes[rank] - 1U]);
    }
    return result;
}

void test_bwt_matches_naive_sort()
{
    // Repetitive inputs make the suffix sorter recurse several levels deep.
    std::vector<Bytes> cases;
    Bytes fibonacci_previous = to_bytes("b");
    Bytes fibonacci = to_bytes("a");
    while (fibonacci.size() < 3'000U)
    {
        Bytes next = fibonacci;
        next.insert(next.end(), fibonacci_previous.begin(), fibonacci_previous.end());
        fibonacci_previous = std::move(fibonacci);
        fibonacci = std::move(next);
    }
    cases.push_back(fibonacci);
    cases.push_back(repeated_text(5'000U));
    cases.push_back(random_bytes(6'000U, 41U));
    std::mt19937 generator(20261002U);
    for (const unsigned int alphabet : {2U, 3U, 7U, 256U})
    {
        for (const std::size_t period : {std::size_t{0}, std::size_t{13}, std::size_t{700}})
        {
            std::uniform_int_distribution<unsigned int> value_distribution(0U, alphabet - 1U);
            Bytes input(4'000U);
            for (std::size_t index = 0; index < input.size(); ++index)
            {
                input[index] = period != 0U && index >= period && index % 97U != 0U
                                   ? input[index - period]
                                   : static_cast<Byte>(255U - value_distribution(generator));
            }
            cases.push_back(input);
        }
    }

    for (const Bytes& input : cases)
    {
        const mzip::detail::BwtResult expected = naive_bwt(input);
        const mzip::detail::BwtResult encoded = mzip::detail::bwt_encode(input);
        CHECK_EQ(encoded.data, expected.data);
        CHECK_EQ(encoded.primary_index, expected.primary_index);
        CHECK_EQ(mzip::detail::bwt_decode(encoded.data, encoded.primary_index), input);
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

void test_cm_and_lzp_round_trip()
{
    const std::vector<Bytes> cases{to_bytes("banana"), repeated_text(5'000U),
                                   random_bytes(4'096U, 11U), Bytes(2'000U, 0U)};
    for (const Bytes& input : cases)
    {
        const auto coded = mzip::detail::cm_encode(input, input.size() * 2U + 64U);
        CHECK(coded.has_value());
        CHECK_EQ(mzip::detail::cm_decode(*coded, input.size()), input);
    }

    std::mt19937 generator(20260723U);
    std::uniform_int_distribution<std::size_t> size_distribution(0U, 2'000U);
    std::uniform_int_distribution<unsigned int> byte_distribution(0U, 255U);
    std::uniform_int_distribution<std::size_t> run_distribution(1U, 200U);
    for (unsigned int round = 0; round < 300U; ++round)
    {
        Bytes input(size_distribution(generator));
        const unsigned int shape = round % 4U;
        std::size_t index = 0;
        while (index < input.size())
        {
            if (shape == 0U)
            {
                input[index++] = static_cast<Byte>(byte_distribution(generator));
            }
            else if (shape == 1U)
            {
                const Byte value = static_cast<Byte>(byte_distribution(generator));
                std::size_t run = run_distribution(generator);
                while (run-- > 0U && index < input.size())
                {
                    input[index++] = value;
                }
            }
            else if (shape == 2U)
            {
                input[index] = static_cast<Byte>((index & 1U) == 0U ? 0x00U : 0xFFU);
                ++index;
            }
            else
            {
                input[index] = static_cast<Byte>(index & 0xFFU);
                ++index;
            }
        }
        const auto coded = mzip::detail::cm_encode(input, input.size() * 2U + 64U);
        CHECK(coded.has_value());
        CHECK_EQ(mzip::detail::cm_decode(*coded, input.size()), input);
    }

    const Bytes sample = repeated_text(3'000U);
    const auto full = mzip::detail::cm_encode(sample, sample.size());
    CHECK(full.has_value());
    for (const std::size_t cut :
         {std::size_t{0}, std::size_t{1}, std::size_t{3}, full->size() - 1U})
    {
        expect_format_error(
            [&]
            {
                static_cast<void>(mzip::detail::cm_decode(std::span<const Byte>(full->data(), cut),
                                                          sample.size()));
            });
    }

    Bytes repetitive = random_bytes(600U, 21U);
    const Bytes chunk = repetitive;
    for (int copy = 0; copy < 6; ++copy)
    {
        repetitive.insert(repetitive.end(), chunk.begin(), chunk.end());
    }
    const auto stream = mzip::detail::lzp_encode(repetitive);
    CHECK(stream.has_value());
    CHECK(stream->size() < repetitive.size());
    CHECK_EQ(mzip::detail::lzp_decode(*stream, repetitive.size()), repetitive);

    CHECK(!mzip::detail::lzp_encode(random_bytes(4'096U, 22U)).has_value());

    // Two 48-byte repeats (with their 8-byte contexts), one 1,000 bytes back and one 6,000:
    // too short for the plain rule, while a far rule takes the distant one, or both once its
    // threshold drops below the near distance.
    Bytes spaced = random_bytes(8'192U, 23U);
    std::copy_n(spaced.begin() + 100, 56, spaced.begin() + 1'100);
    std::copy_n(spaced.begin() + 300, 56, spaced.begin() + 6'300);
    CHECK(!mzip::detail::lzp_encode(spaced).has_value());
    const mzip::detail::LzpRule far_only{128U, 32U, 4'096U};
    const mzip::detail::LzpRule near_too{128U, 32U, 512U};
    const auto far_stream = mzip::detail::lzp_encode(spaced, 20U, far_only);
    const auto near_stream = mzip::detail::lzp_encode(spaced, 20U, near_too);
    CHECK(far_stream.has_value());
    CHECK(near_stream.has_value());
    CHECK(far_stream->size() < spaced.size());
    CHECK(near_stream->size() < far_stream->size());
    CHECK_EQ(mzip::detail::lzp_decode(*far_stream, spaced.size(), 20U, far_only), spaced);
    CHECK_EQ(mzip::detail::lzp_decode(*near_stream, spaced.size(), 20U, near_too), spaced);
}

// A pseudo program: random filler with E8/E9 branches to a few targets inside the block and
// some to addresses before it.
[[nodiscard]] Bytes synthetic_x86(const std::size_t size, const std::uint32_t seed)
{
    std::mt19937 generator(seed);
    std::uniform_int_distribution<unsigned int> byte_distribution(0U, 255U);
    std::uniform_int_distribution<std::size_t> gap_distribution(3U, 14U);
    std::uniform_int_distribution<unsigned int> target_distribution(0U, 7U);
    std::uniform_int_distribution<unsigned int> kind_distribution(0U, 15U);
    Bytes program(size);
    for (Byte& value : program)
    {
        value = static_cast<Byte>(byte_distribution(generator));
    }
    std::size_t index = gap_distribution(generator);
    while (index + 5U <= size)
    {
        const unsigned int kind = kind_distribution(generator);
        program[index] = kind == 0U ? Byte{0xE9U} : Byte{0xE8U};
        const auto next = static_cast<std::uint32_t>(index + 5U);
        const std::uint32_t target = kind == 1U ? 0U - 0x100U - target_distribution(generator) * 64U
                                                : 0x40U + target_distribution(generator) * 0x3B0U;
        const std::uint32_t operand = target - next;
        for (unsigned int byte = 0; byte < 4U; ++byte)
        {
            program[index + 1U + byte] = static_cast<Byte>((operand >> (byte * 8U)) & 0xFFU);
        }
        index += 5U + gap_distribution(generator);
    }
    return program;
}

void test_x86_filter_round_trip()
{
    // Operand + 5 at offset 0; the skip over operand bytes; a negative operand; a top byte
    // outside 0x00/0xFF left alone; a trailing opcode without room for its operand.
    const Bytes program{0xE8U, 0x10U, 0x00U, 0x00U, 0x00U, 0xE8U, 0xE8U, 0xE8U, 0xE8U,
                        0x00U, 0x90U, 0xE9U, 0xFDU, 0xFFU, 0xFFU, 0xFFU, 0xE8U, 0x01U,
                        0x02U, 0x03U, 0x12U, 0x90U, 0xE8U, 0x00U, 0x00U};
    const Bytes expected{0xE8U, 0x15U, 0x00U, 0x00U, 0x00U, 0xE8U, 0xF2U, 0xE8U, 0xE8U,
                         0x00U, 0x90U, 0xE9U, 0x0DU, 0x00U, 0x00U, 0x00U, 0xE8U, 0x01U,
                         0x02U, 0x03U, 0x12U, 0x90U, 0xE8U, 0x00U, 0x00U};
    Bytes filtered = program;
    CHECK_EQ(mzip::detail::x86_filter_encode(filtered), std::size_t{2});
    CHECK_EQ(filtered, expected);
    mzip::detail::x86_filter_decode(filtered);
    CHECK_EQ(filtered, program);

    // Both 25-bit extremes survive the sign-extended top byte.
    for (const std::uint32_t operand : {0x00FFFFFFU, 0xFF000000U, 0x00000000U, 0xFFFFFFFFU})
    {
        Bytes edge{0xE8U, 0U, 0U, 0U, 0U, 0U};
        for (unsigned int byte = 0; byte < 4U; ++byte)
        {
            edge[1U + byte] = static_cast<Byte>((operand >> (byte * 8U)) & 0xFFU);
        }
        Bytes coded = edge;
        static_cast<void>(mzip::detail::x86_filter_encode(coded));
        CHECK(coded != edge);
        mzip::detail::x86_filter_decode(coded);
        CHECK_EQ(coded, edge);
    }

    // Dense random streams exercise opcodes inside operands and every boundary.
    std::mt19937 generator(20260801U);
    std::uniform_int_distribution<std::size_t> size_distribution(0U, 3'000U);
    std::uniform_int_distribution<unsigned int> byte_distribution(0U, 255U);
    for (unsigned int round = 0; round < 300U; ++round)
    {
        Bytes input(size_distribution(generator));
        for (Byte& value : input)
        {
            const unsigned int pick = byte_distribution(generator);
            value = pick < 48U    ? Byte{0xE8U}
                    : pick < 64U  ? Byte{0xE9U}
                    : pick < 128U ? Byte{0x00U}
                    : pick < 160U ? Byte{0xFFU}
                                  : static_cast<Byte>(pick);
        }
        Bytes coded = input;
        static_cast<void>(mzip::detail::x86_filter_encode(coded));
        mzip::detail::x86_filter_decode(coded);
        CHECK_EQ(coded, input);
    }

    const Bytes synthetic = synthetic_x86(96'000U, 5U);
    Bytes synthetic_coded = synthetic;
    CHECK(mzip::detail::x86_filter_encode(synthetic_coded) > 4'000U);
    CHECK(synthetic_coded != synthetic);
    mzip::detail::x86_filter_decode(synthetic_coded);
    CHECK_EQ(synthetic_coded, synthetic);

    // A slice of a real executable, when the host has one.
    for (const char* candidate : {"/usr/bin/ls", "/bin/ls", "/usr/bin/env", "/bin/sh"})
    {
        std::error_code error;
        if (!std::filesystem::is_regular_file(candidate, error))
        {
            continue;
        }
        Bytes binary = read_file(candidate);
        binary.resize(std::min<std::size_t>(binary.size(), 512U * 1024U));
        Bytes binary_coded = binary;
        static_cast<void>(mzip::detail::x86_filter_encode(binary_coded));
        mzip::detail::x86_filter_decode(binary_coded);
        CHECK_EQ(binary_coded, binary);

        TemporaryDirectory directory;
        const auto source = directory.file("binary.bin");
        const auto archive = directory.file("binary.mz");
        const auto restored = directory.file("binary.out");
        write_file(source, binary);
        static_cast<void>(mzip::compress_file(source, archive));
        static_cast<void>(mzip::decompress_file(archive, restored));
        CHECK_EQ(read_file(restored), binary);
        break;
    }
}

void test_x86_flag_in_archives()
{
    TemporaryDirectory directory;
    const auto source = directory.file("program.bin");
    const auto archive = directory.file("program.mz");
    const auto restored = directory.file("program.out");
    const Bytes program = synthetic_x86(200'000U, 9U);
    write_file(source, program);
    const auto stats = mzip::compress_file(source, archive);
    CHECK_EQ(stats.block_count, 1U);
    Bytes bytes = read_file(archive);
    // The block header follows the 24-byte file header; the filter flag is bit 1 of its flags.
    CHECK(bytes.size() > 48U);
    CHECK_EQ(bytes[4], Byte{3U});
    CHECK_EQ(bytes[25] & 0x02U, 0x02U);
    static_cast<void>(mzip::decompress_file(archive, restored));
    CHECK_EQ(read_file(restored), program);

    // Machine code followed by repeated data takes both the x86 filter and an LZP pass; the
    // decoder has to undo them in the right order.
    Bytes mixed = synthetic_x86(60'000U, 21U);
    Bytes chunk = random_bytes(3'000U, 22U);
    for (Byte& value : chunk)
    {
        if (value == 0xE8U || value == 0xE9U)
        {
            value = 0x90U;
        }
    }
    for (unsigned int copy = 0; copy < 30U; ++copy)
    {
        mixed.insert(mixed.end(), chunk.begin(), chunk.end());
    }
    const auto mixed_source = directory.file("mixed.bin");
    const auto mixed_archive = directory.file("mixed.mz");
    write_file(mixed_source, mixed);
    static_cast<void>(mzip::compress_file(mixed_source, mixed_archive));
    const Bytes mixed_bytes = read_file(mixed_archive);
    CHECK(mixed_bytes.size() > 48U);
    CHECK_EQ(mixed_bytes[25], Byte{0x03U});
    static_cast<void>(mzip::decompress_file(mixed_archive, restored));
    CHECK_EQ(read_file(restored), mixed);

    // Version 2 archives may not carry the flag.
    Bytes downgraded = bytes;
    downgraded[4] = 2U;
    write_file(archive, downgraded);
    expect_format_error([&] { static_cast<void>(mzip::decompress_file(archive, restored)); });

    // Raw blocks may not carry it either.
    const auto random_source = directory.file("random.bin");
    const auto random_archive = directory.file("random.mz");
    write_file(random_source, random_bytes(8'192U, 31U));
    const auto random_stats = mzip::compress_file(random_source, random_archive);
    CHECK_EQ(random_stats.raw_blocks, 1U);
    Bytes raw = read_file(random_archive);
    CHECK_EQ(raw[25], Byte{0U});
    raw[25] = 0x02U;
    write_file(random_archive, raw);
    expect_format_error([&]
                        { static_cast<void>(mzip::decompress_file(random_archive, restored)); });
}

// A table of fixed-length records, like a catalogue or a database dump: an increasing 32-bit
// counter, a drifting 16-bit level, and filler from a small alphabet.
[[nodiscard]] Bytes synthetic_records(const std::size_t count, const std::size_t stride,
                                      const std::uint32_t seed)
{
    std::mt19937 generator(seed);
    std::uniform_int_distribution<unsigned int> step_distribution(0U, 40U);
    std::uniform_int_distribution<unsigned int> drift_distribution(0U, 8U);
    std::uniform_int_distribution<unsigned int> filler_distribution(0U, 3U);
    Bytes table(count * stride);
    std::uint32_t counter = 1'000U;
    std::uint32_t level = 30'000U;
    for (std::size_t record = 0; record < count; ++record)
    {
        Byte* const row = table.data() + record * stride;
        counter += step_distribution(generator);
        level = level + drift_distribution(generator) - 4U;
        for (unsigned int byte = 0; byte < 4U; ++byte)
        {
            row[byte] = static_cast<Byte>((counter >> (byte * 8U)) & 0xFFU);
        }
        row[4] = static_cast<Byte>(level & 0xFFU);
        row[5] = static_cast<Byte>((level >> 8U) & 0xFFU);
        for (std::size_t byte = 6; byte < stride; ++byte)
        {
            row[byte] = static_cast<Byte>('A' + filler_distribution(generator));
        }
    }
    return table;
}

// 16-bit little-endian samples of an image whose columns wander independently from row to
// row, so a sample is far more like the one above it than like its neighbours in the row.
[[nodiscard]] Bytes synthetic_image(const std::size_t width, const std::size_t height,
                                    const std::uint32_t seed)
{
    std::mt19937 generator(seed);
    std::uniform_int_distribution<unsigned int> start_distribution(1'000U, 3'000U);
    std::uniform_int_distribution<unsigned int> step_distribution(0U, 80U);
    std::vector<unsigned int> levels(width);
    for (unsigned int& level : levels)
    {
        level = start_distribution(generator);
    }
    Bytes image(width * height * 2U);
    for (std::size_t row = 0; row < height; ++row)
    {
        for (std::size_t column = 0; column < width; ++column)
        {
            unsigned int& level = levels[column];
            level = level + step_distribution(generator) - 40U;
            const std::size_t at = (row * width + column) * 2U;
            image[at] = static_cast<Byte>(level & 0xFFU);
            image[at + 1U] = static_cast<Byte>((level >> 8U) & 0xFFU);
        }
    }
    return image;
}

[[nodiscard]] Bytes record_filter_round_trip(const Bytes& input,
                                             const mzip::detail::RecordPlan& plan)
{
    Bytes coded = input;
    mzip::detail::record_filter_encode(coded, plan);
    Bytes restored = coded;
    mzip::detail::record_filter_decode(restored, plan);
    CHECK_EQ(restored, input);
    return coded;
}

void test_record_filter_round_trip()
{
    using mzip::detail::RecordPlan;

    // Stride 6 in 2-byte units, middle unit only: the first record stays, every later unit
    // becomes its 16-bit difference to the record before (0x0102 - 0x01FF = 0xFF03), and the
    // partial unit at the end is left alone.
    const Bytes small{0x10U, 0x11U, 0xFFU, 0x01U, 0x12U, 0x13U, 0x20U, 0x21U, 0x02U,
                      0x01U, 0x22U, 0x23U, 0x30U, 0x31U, 0x05U, 0x01U, 0x77U};
    const Bytes expected{0x10U, 0x11U, 0xFFU, 0x01U, 0x12U, 0x13U, 0x20U, 0x21U, 0x03U,
                         0xFFU, 0x22U, 0x23U, 0x30U, 0x31U, 0x03U, 0x00U, 0x77U};
    CHECK_EQ(record_filter_round_trip(small, RecordPlan{6U, 2U, Bytes{0x02U}}), expected);
    // Stride 6 in 4-byte units: the second unit is 2 bytes long (0x2322 - 0x1312 = 0x1010).
    const Bytes expected_partial{0x10U, 0x11U, 0xFFU, 0x01U, 0x12U, 0x13U, 0x20U, 0x21U, 0x02U,
                                 0x01U, 0x10U, 0x10U, 0x30U, 0x31U, 0x05U, 0x01U, 0x77U};
    CHECK_EQ(record_filter_round_trip(small, RecordPlan{6U, 4U, Bytes{0x02U}}), expected_partial);

    // Every unit width, several strides and masks, tails of every length.
    std::mt19937 generator(20261002U);
    std::uniform_int_distribution<unsigned int> byte_distribution(0U, 255U);
    for (const std::uint32_t unit : {1U, 2U, 4U, 8U})
    {
        // Whole units, and a shorter last unit when the unit does not divide the stride.
        for (const std::uint32_t stride : {unit, 3U * unit, 8U * unit, 9U * unit + 1U, 28U, 100U})
        {
            const std::uint32_t units = (stride + unit - 1U) / unit;
            RecordPlan plan{stride, unit, Bytes((units + 7U) / 8U)};
            plan.mask[0] = 0x01U;
            for (std::uint32_t column = 1; column < units; ++column)
            {
                if (byte_distribution(generator) % 3U != 0U)
                {
                    plan.mask[column / 8U] =
                        static_cast<Byte>(plan.mask[column / 8U] | (1U << (column % 8U)));
                }
            }
            for (const std::size_t tail :
                 {std::size_t{0}, std::size_t{1}, std::size_t{unit}, std::size_t{stride - 1U}})
            {
                Bytes input = synthetic_records(50U, stride + 6U, stride + unit);
                input.resize(std::size_t{stride} * 37U + tail);
                for (std::size_t index = 0; index < input.size(); index += 5U)
                {
                    input[index] = static_cast<Byte>(byte_distribution(generator));
                }
                static_cast<void>(record_filter_round_trip(input, plan));
                const Bytes serialized = mzip::detail::record_plan_write(plan);
                std::size_t consumed = 0;
                const RecordPlan parsed =
                    mzip::detail::record_plan_read(serialized, input.size(), consumed);
                CHECK_EQ(consumed, serialized.size());
                CHECK_EQ(parsed.stride, plan.stride);
                CHECK_EQ(parsed.unit, plan.unit);
                CHECK_EQ(parsed.mask, plan.mask);
            }
        }
    }
    // Data shorter than one record passes through.
    const Bytes short_input = random_bytes(5U, 3U);
    CHECK_EQ(record_filter_round_trip(short_input, RecordPlan{8U, 1U, Bytes{0xFFU}}), short_input);

    // The search finds the record length of tables and of image rows, and nothing in random
    // bytes or text.
    for (const std::size_t stride : {std::size_t{12}, std::size_t{28}, std::size_t{100}})
    {
        const Bytes table = synthetic_records(160'000U / stride, stride, 7U);
        const std::optional<RecordPlan> plan = mzip::detail::record_plan(table);
        CHECK(plan.has_value());
        if (plan)
        {
            CHECK_EQ(plan->stride, stride);
            CHECK_EQ(unsigned{plan->mask[0]} & 0x01U, 0x01U);
            static_cast<void>(record_filter_round_trip(table, *plan));
        }
    }
    const Bytes image = synthetic_image(300U, 300U, 11U);
    const std::optional<RecordPlan> image_plan = mzip::detail::record_plan(image);
    CHECK(image_plan.has_value());
    if (image_plan)
    {
        CHECK_EQ(image_plan->stride, 600U);
        CHECK_EQ(image_plan->unit, 2U);
        static_cast<void>(record_filter_round_trip(image, *image_plan));
    }
    CHECK(!mzip::detail::record_plan(random_bytes(200'000U, 41U)).has_value());
    CHECK(!mzip::detail::record_plan(repeated_text(200'000U)).has_value());
    CHECK(!mzip::detail::record_plan(Bytes(100U, Byte{7U})).has_value());

    // Slices of a real file, under a table plan and a 16-bit plan.
    for (const char* candidate : {"/usr/bin/ls", "/bin/ls", "/usr/bin/env", "/bin/sh"})
    {
        std::error_code error;
        if (!std::filesystem::is_regular_file(candidate, error))
        {
            continue;
        }
        Bytes binary = read_file(candidate);
        binary.resize(std::min<std::size_t>(binary.size(), 256U * 1024U));
        static_cast<void>(record_filter_round_trip(binary, RecordPlan{28U, 4U, Bytes{0x5BU}}));
        static_cast<void>(
            record_filter_round_trip(binary, RecordPlan{1024U, 2U, Bytes(64U, Byte{0xFFU})}));
        break;
    }

    // Plans the encoder cannot write are rejected.
    const auto expect_bad_plan = [](const Bytes& serialized, const std::size_t block_size)
    {
        std::size_t consumed = 0;
        expect_format_error(
            [&] {
                static_cast<void>(mzip::detail::record_plan_read(serialized, block_size, consumed));
            });
    };
    expect_bad_plan(Bytes{}, 1'000U);
    expect_bad_plan(Bytes{1U, 28U}, 1'000U);                   // truncated header
    expect_bad_plan(Bytes{3U, 6U, 0U, 0x01U}, 1'000U);         // unit 3
    expect_bad_plan(Bytes{0U, 6U, 0U, 0x01U}, 1'000U);         // unit 0
    expect_bad_plan(Bytes{1U, 0U, 0U}, 1'000U);                // stride 0
    expect_bad_plan(Bytes{8U, 6U, 0U, 0x01U}, 1'000U);         // unit longer than the stride
    expect_bad_plan(Bytes{1U, 0xE8U, 0x03U, 0x01U}, 1'000U);   // stride as long as the block
    expect_bad_plan(Bytes{1U, 28U, 0U, 0x01U, 0x00U}, 1'000U); // truncated mask
    expect_bad_plan(Bytes{1U, 4U, 0U, 0x00U}, 1'000U);         // nothing selected
    expect_bad_plan(Bytes{1U, 4U, 0U, 0x10U}, 1'000U);         // a unit past the record
    expect_bad_plan(Bytes{2U, 6U, 0U, 0x08U}, 1'000U);         // a unit past the record
}

void test_record_flag_in_archives()
{
    TemporaryDirectory directory;
    const auto source = directory.file("table.bin");
    const auto archive = directory.file("table.mz");
    const auto restored = directory.file("table.out");
    const Bytes table = synthetic_records(8'000U, 28U, 5U);
    write_file(source, table);
    const auto stats = mzip::compress_file(source, archive);
    CHECK_EQ(stats.block_count, 1U);
    const Bytes bytes = read_file(archive);
    // Flag bit 2 of the block header; the plan opens the payload after the 48 header bytes.
    CHECK(bytes.size() > 52U);
    CHECK_EQ(bytes[24], Byte{2U});
    CHECK_EQ(bytes[25], Byte{0x04U});
    CHECK_EQ(bytes[49], Byte{28U});
    static_cast<void>(mzip::decompress_file(archive, restored));
    CHECK_EQ(read_file(restored), table);

    // Rows of 16-bit samples take the filter with 2-byte units.
    const auto image_source = directory.file("image.bin");
    const Bytes image = synthetic_image(256U, 400U, 13U);
    write_file(image_source, image);
    static_cast<void>(mzip::compress_file(image_source, archive));
    const Bytes image_bytes = read_file(archive);
    CHECK(image_bytes.size() > 52U);
    CHECK_EQ(image_bytes[25], Byte{0x04U});
    CHECK_EQ(image_bytes[48], Byte{2U});
    static_cast<void>(mzip::decompress_file(archive, restored));
    CHECK_EQ(read_file(restored), image);

    const auto expect_rejected = [&](const Bytes& damaged)
    {
        write_file(archive, damaged);
        expect_format_error([&] { static_cast<void>(mzip::decompress_file(archive, restored)); });
    };
    // Version 2 archives may not carry the flag.
    Bytes downgraded = bytes;
    downgraded[4] = 2U;
    expect_rejected(downgraded);
    // It combines with neither LZP nor the x86 filter, and needs the mixer.
    for (const Byte flags : {Byte{0x05U}, Byte{0x06U}, Byte{0x07U}})
    {
        Bytes combined = bytes;
        combined[25] = flags;
        expect_rejected(combined);
    }
    Bytes transformed = bytes;
    transformed[24] = 1U;
    expect_rejected(transformed);
    // A damaged plan: bad unit, zero stride, stride beyond the block, empty mask.
    const auto damaged_plan = [&](const std::size_t offset, const Byte value)
    {
        Bytes damaged = bytes;
        damaged[offset] = value;
        return damaged;
    };
    expect_rejected(damaged_plan(48U, Byte{5U}));
    Bytes zero_stride = damaged_plan(49U, Byte{0U});
    zero_stride[50] = 0U;
    expect_rejected(zero_stride);
    expect_rejected(damaged_plan(50U, Byte{0xFFU}));
    expect_rejected(damaged_plan(51U, Byte{0U}));
    // A payload too short to hold the plan.
    Bytes truncated(bytes.begin(), bytes.begin() + 50);
    write_u32(truncated, 32U, 2U);
    expect_rejected(truncated);

    // Raw blocks may not carry the flag.
    const auto random_source = directory.file("random.bin");
    const auto random_archive = directory.file("random.mz");
    write_file(random_source, random_bytes(8'192U, 33U));
    const auto random_stats = mzip::compress_file(random_source, random_archive);
    CHECK_EQ(random_stats.raw_blocks, 1U);
    Bytes raw = read_file(random_archive);
    CHECK_EQ(raw[25], Byte{0U});
    raw[25] = 0x04U;
    write_file(random_archive, raw);
    expect_format_error([&]
                        { static_cast<void>(mzip::decompress_file(random_archive, restored)); });
}

// Words from a small vocabulary, then the same words spelled backwards, then forwards again:
// one alphabet whose statistics change twice, exactly at multiples of `part_size`.
[[nodiscard]] Bytes mixed_content(const std::size_t part_size, const std::uint32_t seed)
{
    constexpr std::array<std::string_view, 8> forward{"alpha ", "beta ", "gamma ", "delta ",
                                                      "block ", "sort ", "model ", "mixer\n"};
    constexpr std::array<std::string_view, 8> backward{"ahpla ", "ateb ", "ammag ", "atled ",
                                                       "kcolb ", "tros ", "ledom ", "rexim\n"};
    std::mt19937 generator(seed);
    std::uniform_int_distribution<std::size_t> word_distribution(0U, forward.size() - 1U);
    Bytes data;
    for (const auto* words : {&forward, &backward, &forward})
    {
        const std::size_t end = data.size() + part_size;
        while (data.size() < end)
        {
            const Bytes word = to_bytes((*words)[word_distribution(generator)]);
            data.insert(data.end(), word.begin(), word.end());
        }
        data.resize(end);
    }
    return data;
}

void test_content_boundaries()
{
    constexpr std::size_t part = 320U * 1024U;
    const Bytes mixed = mixed_content(part, 3U);
    const std::vector<std::size_t> cuts = mzip::detail::content_boundaries(mixed);
    CHECK_EQ(cuts.size(), std::size_t{2});
    if (cuts.size() == 2U)
    {
        CHECK(cuts[0] + 64U > part && cuts[0] < part + 64U);
        CHECK(cuts[1] + 64U > 2U * part && cuts[1] < 2U * part + 64U);
    }

    // Uniform data stays whole, and so does a change too close to the end.
    Bytes text;
    for (std::uint32_t seed = 10U; seed < 13U; ++seed)
    {
        const Bytes more = mixed_content(part, seed);
        text.insert(text.end(), more.begin(), more.begin() + static_cast<std::ptrdiff_t>(part));
    }
    CHECK(mzip::detail::content_boundaries(text).empty());
    CHECK(mzip::detail::content_boundaries(random_bytes(3U * part, 5U)).empty());
    const Bytes small(mixed.begin(), mixed.begin() + static_cast<std::ptrdiff_t>(part + 1000U));
    CHECK(mzip::detail::content_boundaries(small).empty());
    CHECK(mzip::detail::content_boundaries(Bytes{}).empty());

    // A change whose content repeats on both sides of the cut, like a tar of similar files,
    // codes better whole.
    Bytes repeated = mixed;
    repeated.insert(repeated.end(), mixed.begin(), mixed.end());
    CHECK(mzip::detail::content_boundaries(repeated).empty());
}

void test_segmented_archives()
{
    constexpr std::size_t part = 320U * 1024U;
    const Bytes data = mixed_content(part, 6U);
    TemporaryDirectory directory;
    const auto source = directory.file("mixed.bin");
    const auto archive = directory.file("mixed.mz");
    const auto restored = directory.file("mixed.out");
    write_file(source, data);

    mzip::CompressionOptions single;
    single.thread_count = 1U;
    const auto stats = mzip::compress_file(source, archive, single);
    CHECK_EQ(stats.block_count, 3U);
    const Bytes bytes = read_file(archive);
    CHECK(bytes.size() > 48U);
    // Flags byte: bit 2 marks blocks that end before the block size; the count follows.
    CHECK_EQ(bytes[5], Byte{0x04U});
    CHECK_EQ(bytes[20], Byte{3U});
    static_cast<void>(mzip::decompress_file(archive, restored));
    CHECK_EQ(read_file(restored), data);

    mzip::CompressionOptions parallel;
    parallel.thread_count = 8U;
    const auto parallel_archive = directory.file("parallel.mz");
    static_cast<void>(mzip::compress_file(source, parallel_archive, parallel));
    CHECK_EQ(read_file(parallel_archive), bytes);
    mzip::DecompressionOptions sequential;
    sequential.thread_count = 1U;
    const auto restored_sequential = directory.file("sequential.out");
    static_cast<void>(mzip::decompress_file(archive, restored_sequential, sequential));
    CHECK_EQ(read_file(restored_sequential), data);

    // The ratio profile cuts its single chunk the same way.
    mzip::CompressionOptions ratio;
    ratio.profile = mzip::Profile::ratio;
    const auto ratio_archive = directory.file("ratio.mz");
    const auto ratio_stats = mzip::compress_file(source, ratio_archive, ratio);
    CHECK_EQ(ratio_stats.block_count, 3U);
    const auto ratio_restored = directory.file("ratio.out");
    static_cast<void>(mzip::decompress_file(ratio_archive, ratio_restored));
    CHECK_EQ(read_file(ratio_restored), data);

    // Several chunks, each cut where it changes, under a stream LZP pass: a directory holding
    // the data twice, in chunks that do not line up with the content.
    const auto tree = directory.file("tree");
    std::filesystem::create_directories(tree);
    write_file(tree / "a.bin", data);
    write_file(tree / "b.bin", data);
    mzip::CompressionOptions chunked;
    chunked.block_size = 700U * 1024U;
    const auto tree_archive = directory.file("tree.mz");
    const auto tree_stats = mzip::compress_file(tree, tree_archive, chunked);
    const Bytes tree_bytes = read_file(tree_archive);
    CHECK_EQ(tree_bytes[5], Byte{0x07U});
    CHECK(tree_stats.block_count > 2U);
    const auto tree_restored = directory.file("tree-out");
    static_cast<void>(mzip::decompress_file(tree_archive, tree_restored));
    CHECK_EQ(read_file(tree_restored / "tree" / "a.bin"), data);
    CHECK_EQ(read_file(tree_restored / "tree" / "b.bin"), data);

    // A cut chunk followed by one that stays whole.
    const Bytes noise = random_bytes(3U * part, 7U);
    Bytes two_chunks = data;
    two_chunks.insert(two_chunks.end(), noise.begin(), noise.end());
    const auto two_chunks_source = directory.file("two-chunks.bin");
    write_file(two_chunks_source, two_chunks);
    mzip::CompressionOptions halves;
    halves.block_size = static_cast<std::uint32_t>(3U * part);
    const auto two_chunks_archive = directory.file("two-chunks.mz");
    const auto two_chunks_stats =
        mzip::compress_file(two_chunks_source, two_chunks_archive, halves);
    CHECK_EQ(two_chunks_stats.block_count, 4U);
    const auto two_chunks_restored = directory.file("two-chunks.out");
    static_cast<void>(mzip::decompress_file(two_chunks_archive, two_chunks_restored));
    CHECK_EQ(read_file(two_chunks_restored), two_chunks);

    const auto expect_rejected = [&](const Bytes& damaged)
    {
        const auto hostile = directory.file("hostile.mz");
        const auto output = directory.file("hostile.out");
        write_file(hostile, damaged);
        expect_format_error([&] { static_cast<void>(mzip::decompress_file(hostile, output)); });
        CHECK(!std::filesystem::exists(output));
    };
    // Short blocks need the flag, the flag needs version 3, and the count must fit the sizes.
    Bytes unflagged = bytes;
    unflagged[5] = 0U;
    expect_rejected(unflagged);
    Bytes downgraded = bytes;
    downgraded[4] = 2U;
    expect_rejected(downgraded);
    for (const std::uint32_t count : {0U, 1U, 2U, 4U, static_cast<std::uint32_t>(data.size() + 1U)})
    {
        Bytes recounted = bytes;
        write_u32(recounted, 20U, count);
        expect_rejected(recounted);
    }
    // A first block larger than the block size, or empty.
    for (const std::uint32_t size : {0U, static_cast<std::uint32_t>(data.size() + 1U)})
    {
        Bytes resized = bytes;
        write_u32(resized, 28U, size);
        expect_rejected(resized);
    }
    // Blocks that no longer add up to the original size.
    const std::size_t second_header = 48U + (std::size_t{bytes[32]} | std::size_t{bytes[33]} << 8U |
                                             std::size_t{bytes[34]} << 16U);
    CHECK(bytes.size() > second_header + 24U);
    for (const std::uint32_t delta : {1U, 0xFFFF'FFFFU})
    {
        Bytes resized = bytes;
        const std::uint32_t size = static_cast<std::uint32_t>(resized[second_header + 4U] |
                                                              resized[second_header + 5U] << 8U |
                                                              resized[second_header + 6U] << 16U);
        write_u32(resized, second_header + 4U, size + delta);
        expect_rejected(resized);
    }

    // Full-size blocks are still valid under the flag.
    const auto plain_source = directory.file("plain.txt");
    write_file(plain_source, repeated_text(50'000U));
    const auto plain_archive = directory.file("plain.mz");
    static_cast<void>(mzip::compress_file(plain_source, plain_archive));
    Bytes flagged = read_file(plain_archive);
    CHECK_EQ(flagged[5], Byte{0U});
    flagged[5] = 0x04U;
    write_file(plain_archive, flagged);
    const auto plain_restored = directory.file("plain.out");
    static_cast<void>(mzip::decompress_file(plain_archive, plain_restored));
    CHECK_EQ(read_file(plain_restored), repeated_text(50'000U));
}

void test_stream_lzp_round_trip()
{
    const Bytes chunk = random_bytes(3'000U, 33U);
    Bytes data;
    for (unsigned int copy = 0; copy < 8U; ++copy)
    {
        data.insert(data.end(), chunk.begin(), chunk.end());
        const Bytes separator = random_bytes(500U, 40U + copy);
        data.insert(data.end(), separator.begin(), separator.end());
    }

    TemporaryDirectory directory;
    const auto source = directory.file("versions.bin");
    write_file(source, data);
    mzip::CompressionOptions options;
    options.block_size = 1024U;
    const auto archive = directory.file("versions.mz");
    const auto stats = mzip::compress_file(source, archive, options);
    CHECK(stats.output_size < data.size() / 2U);

    const auto restored = directory.file("versions.out");
    static_cast<void>(mzip::decompress_file(archive, restored));
    CHECK_EQ(read_file(restored), data);

    mzip::DecompressionOptions sequential;
    sequential.thread_count = 1U;
    const auto restored_single = directory.file("versions-single.out");
    static_cast<void>(mzip::decompress_file(archive, restored_single, sequential));
    CHECK_EQ(read_file(restored_single), data);

    mzip::CompressionOptions whole;
    whole.profile = mzip::Profile::ratio;
    const auto whole_archive = directory.file("whole.mz");
    const auto whole_stats = mzip::compress_file(source, whole_archive, whole);
    CHECK_EQ(whole_stats.block_count, 1U);
    const auto whole_restored = directory.file("whole.out");
    static_cast<void>(mzip::decompress_file(whole_archive, whole_restored));
    CHECK_EQ(read_file(whole_restored), data);

    // Repeats too short for the block passes count for the stream pass once their source lies
    // a block or more back; version 2 decoders never took them, so a downgraded archive fails.
    Bytes distant = random_bytes(24'000U, 41U);
    for (std::size_t target = 2'000U; target + 64U < distant.size(); target += 700U)
    {
        const std::size_t origin = target - 1'500U - target % 300U;
        std::copy_n(distant.begin() + static_cast<std::ptrdiff_t>(origin), 8U + 48U,
                    distant.begin() + static_cast<std::ptrdiff_t>(target));
    }
    const auto distant_source = directory.file("distant.bin");
    const auto distant_archive = directory.file("distant.mz");
    write_file(distant_source, distant);
    const auto distant_stats = mzip::compress_file(distant_source, distant_archive, options);
    CHECK(distant_stats.output_size < distant.size());
    Bytes distant_bytes = read_file(distant_archive);
    CHECK_EQ(distant_bytes[5] & 0x02U, 0x02U);
    const auto distant_restored = directory.file("distant.out");
    static_cast<void>(mzip::decompress_file(distant_archive, distant_restored));
    CHECK_EQ(read_file(distant_restored), distant);
    distant_bytes[4] = 2U;
    write_file(distant_archive, distant_bytes);
    expect_format_error(
        [&] { static_cast<void>(mzip::decompress_file(distant_archive, distant_restored)); });

    const auto tree = directory.file("tree");
    std::filesystem::create_directories(tree);
    write_file(tree / "a.bin", data);
    write_file(tree / "b.bin", data);
    const auto tree_archive = directory.file("tree.mz");
    static_cast<void>(mzip::compress_file(tree, tree_archive, options));
    const auto tree_restored = directory.file("tree-out");
    static_cast<void>(mzip::decompress_file(tree_archive, tree_restored));
    CHECK_EQ(read_file(tree_restored / "tree" / "a.bin"), data);
    CHECK_EQ(read_file(tree_restored / "tree" / "b.bin"), data);
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

    static const unsigned char golden_v3[] = {
        0x4DU, 0x5AU, 0x49U, 0x50U, 0x03U, 0x02U, 0x00U, 0x00U, 0x00U, 0x04U, 0x00U, 0x00U, 0x28U,
        0x0AU, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x01U, 0x00U, 0x00U, 0x00U, 0x7EU, 0x01U,
        0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0xE4U, 0x90U, 0xB6U, 0x1EU, 0x14U, 0x00U, 0x00U,
        0x00U, 0x02U, 0x00U, 0x00U, 0x00U, 0x7EU, 0x01U, 0x00U, 0x00U, 0x16U, 0x01U, 0x00U, 0x00U,
        0x79U, 0x01U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x12U, 0xADU, 0xA1U, 0x37U, 0xFEU,
        0x02U, 0xE1U, 0xD5U, 0x9FU, 0xE0U, 0x9EU, 0x8EU, 0xF5U, 0x76U, 0x78U, 0x67U, 0x4DU, 0x55U,
        0x92U, 0x27U, 0x1FU, 0xE1U, 0x63U, 0x63U, 0x4CU, 0xF1U, 0x52U, 0x5AU, 0x8BU, 0xA2U, 0x87U,
        0xECU, 0xCEU, 0x24U, 0x03U, 0x3AU, 0x54U, 0xA9U, 0xF6U, 0x38U, 0xECU, 0xBDU, 0xBEU, 0x6FU,
        0x43U, 0xD8U, 0xEDU, 0xC2U, 0x29U, 0x7AU, 0xD4U, 0x1CU, 0x0DU, 0x70U, 0x87U, 0x52U, 0x15U,
        0xEFU, 0xD4U, 0x6CU, 0xC1U, 0x0BU, 0x1AU, 0xC8U, 0xF0U, 0x15U, 0x63U, 0x6DU, 0xD0U, 0x94U,
        0x1BU, 0xFEU, 0x92U, 0x05U, 0xFCU, 0x7DU, 0x1FU, 0x04U, 0xBCU, 0xD6U, 0x04U, 0xABU, 0x07U,
        0x21U, 0x84U, 0x99U, 0x00U, 0xA2U, 0x4DU, 0x71U, 0xFCU, 0xAEU, 0xF6U, 0xCEU, 0xEEU, 0xC8U,
        0xCCU, 0x07U, 0x4AU, 0x9BU, 0x18U, 0xCDU, 0x6AU, 0xB6U, 0xDFU, 0x8DU, 0x9DU, 0xD7U, 0x60U,
        0x3CU, 0x0EU, 0x5BU, 0x91U, 0x98U, 0x3CU, 0xE3U, 0xCBU, 0xF6U, 0xDAU, 0xC4U, 0x84U, 0x76U,
        0x6CU, 0xAFU, 0xD4U, 0xE4U, 0x9AU, 0xB2U, 0x9FU, 0x44U, 0x24U, 0xD1U, 0x2EU, 0x45U, 0xF6U,
        0xE6U, 0x7EU, 0x16U, 0xACU, 0x99U, 0xB6U, 0xB5U, 0x59U, 0xEFU, 0x8DU, 0xFAU, 0x73U, 0xF8U,
        0x20U, 0xDEU, 0x4DU, 0x11U, 0xC5U, 0xFFU, 0x38U, 0xCAU, 0x74U, 0x7FU, 0x88U, 0x1DU, 0x11U,
        0x1CU, 0x03U, 0xA6U, 0x2AU, 0x6EU, 0xB0U, 0x6DU, 0xA8U, 0x94U, 0xC1U, 0x11U, 0xA5U, 0xE5U,
        0x31U, 0x81U, 0x8FU, 0x81U, 0xA3U, 0xF0U, 0x4AU, 0x8AU, 0x2EU, 0xA8U, 0x4BU, 0x0AU, 0x4BU,
        0xF8U, 0xFAU, 0x45U, 0x5DU, 0xE6U, 0xB1U, 0x40U, 0x54U, 0xD5U, 0x7FU, 0x41U, 0x01U, 0x2AU,
        0x22U, 0xE2U, 0xE2U, 0x3BU, 0x05U, 0x7EU, 0xAFU, 0x14U, 0x6BU, 0x92U, 0xDAU, 0x3BU, 0x1CU,
        0x0DU, 0xE8U, 0x8FU, 0x2EU, 0xACU, 0xEAU, 0xD5U, 0x64U, 0x3AU, 0x2FU, 0xBAU, 0x66U, 0x7DU,
        0x9EU, 0x5AU, 0x0FU, 0x31U, 0x6CU, 0x40U, 0x68U, 0x2DU, 0xE5U, 0xBCU, 0x55U, 0x26U, 0xC1U,
        0x7EU, 0x3AU, 0xD0U, 0x34U, 0x95U, 0x6DU, 0xC7U, 0x4EU, 0x1EU, 0xD0U, 0x68U, 0xDBU, 0x58U,
        0x9CU, 0x22U, 0x14U, 0xBCU, 0xF4U, 0x59U, 0x86U, 0xADU, 0x1BU, 0x34U, 0xE1U, 0x00U, 0x29U,
        0x4AU, 0xDBU, 0x48U, 0xB3U, 0x5FU, 0xECU, 0x86U, 0xE6U, 0x7CU, 0xDDU, 0x72U, 0x34U, 0xF3U,
        0x4EU, 0xF0U, 0x01U, 0x93U};

    static const unsigned char golden_v2[] = {
        0x4DU, 0x5AU, 0x49U, 0x50U, 0x02U, 0x02U, 0x00U, 0x00U, 0x00U, 0x04U, 0x00U, 0x00U, 0x28U,
        0x0AU, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x01U, 0x00U, 0x00U, 0x00U, 0x7EU, 0x01U,
        0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0xE4U, 0x90U, 0xB6U, 0x1EU, 0x14U, 0x00U, 0x00U,
        0x00U, 0x02U, 0x00U, 0x00U, 0x00U, 0x7EU, 0x01U, 0x00U, 0x00U, 0x29U, 0x01U, 0x00U, 0x00U,
        0x79U, 0x01U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x12U, 0xADU, 0xA1U, 0x37U, 0xFDU,
        0xFAU, 0x22U, 0x2EU, 0xEBU, 0xFFU, 0x4EU, 0x29U, 0xDEU, 0x65U, 0x4EU, 0x33U, 0x6BU, 0x36U,
        0x85U, 0xD0U, 0x7AU, 0xB4U, 0x54U, 0xB7U, 0x9FU, 0x38U, 0x06U, 0x23U, 0xFCU, 0x7EU, 0x2AU,
        0xE7U, 0xADU, 0x59U, 0x80U, 0x50U, 0x58U, 0xC8U, 0xC7U, 0xFEU, 0x62U, 0xEFU, 0x99U, 0x83U,
        0x1DU, 0x8DU, 0x38U, 0x07U, 0x2CU, 0x75U, 0x14U, 0x64U, 0x3FU, 0x45U, 0xB8U, 0x2BU, 0x65U,
        0x50U, 0x7BU, 0x16U, 0xD3U, 0xE5U, 0xD8U, 0x94U, 0xD6U, 0xF6U, 0xFDU, 0x0DU, 0xC2U, 0x5EU,
        0x79U, 0xB0U, 0xA3U, 0xE9U, 0x07U, 0x12U, 0x5FU, 0x18U, 0xD9U, 0xCFU, 0x86U, 0x17U, 0x43U,
        0x0DU, 0xAAU, 0xF1U, 0x91U, 0xD1U, 0x4AU, 0xFEU, 0xA5U, 0x08U, 0x3CU, 0x9DU, 0xC7U, 0xCCU,
        0x93U, 0x0FU, 0xD8U, 0xABU, 0x38U, 0xF5U, 0x64U, 0xA7U, 0xE8U, 0x68U, 0x68U, 0xD8U, 0x7AU,
        0xBAU, 0xA0U, 0x16U, 0x71U, 0x58U, 0xC5U, 0x54U, 0x46U, 0x66U, 0xD3U, 0x54U, 0xA5U, 0x91U,
        0x0AU, 0x3EU, 0x83U, 0x04U, 0xA3U, 0x12U, 0xAEU, 0x87U, 0xEBU, 0x3CU, 0x2DU, 0x00U, 0xC6U,
        0xB6U, 0x55U, 0xFEU, 0xD7U, 0xC7U, 0xCAU, 0x05U, 0x5CU, 0x49U, 0xB4U, 0x72U, 0x52U, 0xCDU,
        0x3AU, 0xB7U, 0x94U, 0x50U, 0xC5U, 0x36U, 0xE5U, 0xA8U, 0xEDU, 0x97U, 0x15U, 0x5AU, 0xAEU,
        0x70U, 0x3BU, 0x19U, 0xBAU, 0xA6U, 0x6FU, 0x86U, 0x4BU, 0xFAU, 0x80U, 0xD8U, 0xADU, 0xCAU,
        0xFEU, 0xB2U, 0x7BU, 0xF3U, 0x60U, 0x81U, 0xC0U, 0x05U, 0x66U, 0x87U, 0x2DU, 0xC5U, 0x0DU,
        0x80U, 0x8BU, 0xB8U, 0x03U, 0xB5U, 0xCFU, 0x7EU, 0x61U, 0xE0U, 0x34U, 0xFEU, 0x43U, 0x54U,
        0x55U, 0x78U, 0x94U, 0x3FU, 0x89U, 0x9FU, 0xAFU, 0x03U, 0x25U, 0x1EU, 0xA8U, 0x5FU, 0x56U,
        0x09U, 0x8FU, 0x56U, 0x9BU, 0x08U, 0x56U, 0xBFU, 0x41U, 0x4DU, 0x86U, 0x48U, 0x40U, 0x6FU,
        0xA5U, 0x79U, 0xCAU, 0x13U, 0x8EU, 0x6FU, 0x8DU, 0xBAU, 0xD5U, 0x25U, 0x7AU, 0xC7U, 0xD4U,
        0x5AU, 0x0CU, 0x0DU, 0x1DU, 0x25U, 0x3CU, 0x32U, 0xE7U, 0xF6U, 0x50U, 0x97U, 0x1BU, 0x6EU,
        0xF2U, 0x7FU, 0xA5U, 0x2AU, 0x11U, 0x16U, 0xDDU, 0xABU, 0x90U, 0x06U, 0x56U, 0x19U, 0x55U,
        0xE1U, 0xEBU, 0x80U, 0x08U, 0xA8U, 0x86U, 0x9FU, 0xD3U, 0xDFU, 0xB5U, 0xFDU, 0xDDU, 0xFFU,
        0x10U, 0x30U, 0xFCU, 0x8EU, 0xA1U, 0x0DU, 0x1EU, 0x39U, 0x93U, 0x1CU, 0xAAU, 0xB3U, 0xDBU,
        0x03U, 0xBAU, 0x13U, 0xBAU, 0x1BU, 0x86U, 0x95U, 0x47U, 0x17U, 0x65U};

    static const unsigned char golden_v1[] = {
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
    const Bytes expected(golden_v3, golden_v3 + sizeof(golden_v3));

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

    // Version 1 and 2 archives stay readable.
    const Bytes legacy_v1(golden_v1, golden_v1 + sizeof(golden_v1));
    const Bytes legacy_v2(golden_v2, golden_v2 + sizeof(golden_v2));
    unsigned int legacy_index = 1;
    for (const Bytes* legacy : {&legacy_v1, &legacy_v2})
    {
        const std::string suffix = std::to_string(legacy_index++);
        const auto pinned_legacy = directory.file("pinned-v" + suffix + ".mz");
        write_file(pinned_legacy, *legacy);
        const auto restored_legacy = directory.file("restored-v" + suffix + ".bin");
        static_cast<void>(mzip::decompress_file(pinned_legacy, restored_legacy));
        CHECK_EQ(read_file(restored_legacy), input);
    }
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

// A single-block input whose candidates all compete: the archive must not depend on the thread
// count, so threads may only ever be spent on other blocks.
void test_thread_count_does_not_change_single_block()
{
    TemporaryDirectory directory;
    // Machine code, then text, then repeats: the x86, LZP and plain candidates all compete.
    Bytes mixed = synthetic_x86(120'000U, 51U);
    const Bytes text = repeated_text(60'000U);
    mixed.insert(mixed.end(), text.begin(), text.end());
    const Bytes chunk = random_bytes(4'000U, 52U);
    for (unsigned int copy = 0; copy < 12U; ++copy)
    {
        mixed.insert(mixed.end(), chunk.begin(), chunk.end());
    }
    // Stepped values, where the run coders compete with the mixer.
    Bytes table;
    for (std::uint32_t row = 0; row < 40'000U; ++row)
    {
        table.push_back(static_cast<Byte>(row / 300U));
        table.push_back(static_cast<Byte>((row * 7U) / 1000U));
    }

    for (const Bytes& data : {mixed, table, repeated_text(100'000U), random_bytes(30'000U, 53U)})
    {
        const auto source = directory.file("single.bin");
        write_file(source, data);
        for (const mzip::Profile profile : {mzip::Profile::balanced, mzip::Profile::ratio})
        {
            Bytes reference;
            for (const std::uint32_t threads : {1U, 2U, 8U})
            {
                mzip::CompressionOptions options;
                options.profile = profile;
                options.thread_count = threads;
                const auto archive = directory.file("single-" + std::to_string(threads) + ".mz");
                const auto stats = mzip::compress_file(source, archive, options);
                CHECK_EQ(stats.block_count, 1U);
                const Bytes bytes = read_file(archive);
                if (threads == 1U)
                {
                    reference = bytes;
                }
                CHECK_EQ(bytes, reference);

                const auto restored = directory.file("single.out");
                static_cast<void>(mzip::decompress_file(archive, restored));
                CHECK_EQ(read_file(restored), data);
                std::filesystem::remove(restored);
            }
        }
    }
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
    run_test("BWT matches a naive suffix sort", test_bwt_matches_naive_sort);
    run_test("MTF round-trip", test_mtf_round_trip);
    run_test("stream codec round-trip", test_stream_codec_round_trip);
    run_test("context mixer and LZP round-trip", test_cm_and_lzp_round_trip);
    run_test("stream LZP round-trip", test_stream_lzp_round_trip);
    run_test("x86 filter round-trip", test_x86_filter_round_trip);
    run_test("x86 flag in archives", test_x86_flag_in_archives);
    run_test("record filter round-trip", test_record_filter_round_trip);
    run_test("record flag in archives", test_record_flag_in_archives);
    run_test("content boundaries", test_content_boundaries);
    run_test("segmented archives", test_segmented_archives);
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
    run_test("thread count does not change a single block",
             test_thread_count_does_not_change_single_block);
    run_test("same input/output rejection", test_same_input_and_output_is_rejected);

    if (failure_count != 0)
    {
        std::cerr << failure_count << " test assertion(s) failed\n";
        return 1;
    }
    std::cout << "All tests passed\n";
    return 0;
}
