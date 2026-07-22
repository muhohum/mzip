#include <mzip/mzip.hpp>

#include <charconv>
#include <cstdint>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>

#ifndef MZIP_VERSION
#define MZIP_VERSION "dev"
#endif

namespace
{

void print_usage(std::ostream& output)
{
    output << "mzip " MZIP_VERSION " - block-sorting file compressor\n\n"
           << "Usage:\n"
           << "  mzip compress <input> <output> [--block-size <bytes>] [--threads <count>]\n"
           << "  mzip decompress <input> <output> [--threads <count>]\n"
           << "  mzip --help\n"
           << "  mzip --version\n\n"
           << "The input may be a file or a directory; a directory archive extracts back into"
              " a directory.\n"
           << "Block size: " << mzip::minimum_block_size << ".." << mzip::maximum_block_size
           << " bytes; 0 or omitted picks one from the input size.\n"
           << "Threads: 0 selects the hardware concurrency (default). Output does not depend on"
              " the thread count.\n";
}

[[nodiscard]] std::uint32_t parse_number(const std::string_view text, const char* description)
{
    std::uint32_t result = 0;
    const char* const begin = text.data();
    const char* const end = text.data() + text.size();
    const auto conversion = std::from_chars(begin, end, result);
    if (conversion.ec != std::errc{} || conversion.ptr != end)
    {
        throw std::invalid_argument(std::string(description) + " must be a decimal number");
    }
    return result;
}

[[nodiscard]] mzip::CompressionOptions parse_compress_options(const int argc, char* argv[])
{
    mzip::CompressionOptions options;
    for (int index = 4; index < argc; index += 2)
    {
        const std::string_view flag = argv[index];
        if (index + 1 >= argc)
        {
            throw std::invalid_argument("missing value after " + std::string{flag});
        }
        const std::string_view value = argv[index + 1];
        if (flag == "--block-size")
        {
            options.block_size = parse_number(value, "block size");
        }
        else if (flag == "--threads")
        {
            options.thread_count = parse_number(value, "thread count");
        }
        else
        {
            throw std::invalid_argument("unknown option: " + std::string{flag});
        }
    }
    return options;
}

[[nodiscard]] mzip::DecompressionOptions parse_decompress_options(const int argc, char* argv[])
{
    mzip::DecompressionOptions options;
    for (int index = 4; index < argc; index += 2)
    {
        const std::string_view flag = argv[index];
        if (index + 1 >= argc)
        {
            throw std::invalid_argument("missing value after " + std::string{flag});
        }
        if (flag == "--threads")
        {
            options.thread_count = parse_number(argv[index + 1], "thread count");
        }
        else
        {
            throw std::invalid_argument("unknown option: " + std::string{flag});
        }
    }
    return options;
}

[[nodiscard]] std::string format_bytes(const std::uint64_t bytes)
{
    constexpr std::uint64_t kibibyte = 1024U;
    constexpr std::uint64_t mebibyte = kibibyte * 1024U;
    constexpr std::uint64_t gibibyte = mebibyte * 1024U;

    double value = static_cast<double>(bytes);
    const char* suffix = "B";
    if (bytes >= gibibyte)
    {
        value /= static_cast<double>(gibibyte);
        suffix = "GiB";
    }
    else if (bytes >= mebibyte)
    {
        value /= static_cast<double>(mebibyte);
        suffix = "MiB";
    }
    else if (bytes >= kibibyte)
    {
        value /= static_cast<double>(kibibyte);
        suffix = "KiB";
    }

    std::ostringstream output;
    output << std::fixed << std::setprecision(suffix == std::string_view{"B"} ? 0 : 2) << value
           << ' ' << suffix;
    return output.str();
}

void print_compression_stats(const mzip::CompressionStats& stats)
{
    std::cout << "Compressed successfully\n"
              << "  Input:  " << stats.input_size << " bytes (" << format_bytes(stats.input_size)
              << ")\n"
              << "  Output: " << stats.output_size << " bytes (" << format_bytes(stats.output_size)
              << ")\n";
    if (stats.input_size == 0U)
    {
        std::cout << "  Ratio:  n/a (empty input)\n"
                  << "  Saved:  n/a (empty input)\n";
    }
    else
    {
        std::cout << std::fixed << std::setprecision(4) << "  Ratio:  " << stats.ratio() << "\n"
                  << std::setprecision(2) << "  Saved:  " << stats.savings_percent() << "%\n";
    }
    std::cout << "  Blocks: raw=" << stats.raw_blocks
              << ", transformed=" << stats.transformed_blocks << "\n"
              << std::fixed << std::setprecision(3) << "  Time:   " << stats.elapsed_seconds
              << " s\n";
}

void print_decompression_stats(const mzip::CompressionStats& stats)
{
    std::cout << "Decompressed successfully\n"
              << "  Archive: " << stats.input_size << " bytes (" << format_bytes(stats.input_size)
              << ")\n"
              << "  Output:  " << stats.output_size << " bytes (" << format_bytes(stats.output_size)
              << ")\n"
              << "  Blocks:  raw=" << stats.raw_blocks
              << ", transformed=" << stats.transformed_blocks << "\n"
              << std::fixed << std::setprecision(3) << "  Time:    " << stats.elapsed_seconds
              << " s\n";
}

} // namespace

int main(const int argc, char* argv[])
{
    try
    {
        if (argc == 2 && std::string_view{argv[1]} == "--help")
        {
            print_usage(std::cout);
            return 0;
        }
        if (argc == 2 && std::string_view{argv[1]} == "--version")
        {
            std::cout << "mzip " MZIP_VERSION "\n";
            return 0;
        }
        if (argc < 2)
        {
            print_usage(std::cerr);
            return 2;
        }

        const std::string_view command = argv[1];
        if (command == "compress")
        {
            if (argc < 4 || argc > 8)
            {
                print_usage(std::cerr);
                return 2;
            }
            const mzip::CompressionOptions options = parse_compress_options(argc, argv);
            const auto stats = mzip::compress_file(std::filesystem::path{argv[2]},
                                                   std::filesystem::path{argv[3]}, options);
            print_compression_stats(stats);
            return 0;
        }

        if (command == "decompress")
        {
            if (argc < 4 || argc > 6)
            {
                print_usage(std::cerr);
                return 2;
            }
            const mzip::DecompressionOptions options = parse_decompress_options(argc, argv);
            const auto stats = mzip::decompress_file(std::filesystem::path{argv[2]},
                                                     std::filesystem::path{argv[3]}, options);
            print_decompression_stats(stats);
            return 0;
        }

        throw std::invalid_argument("unknown command: " + std::string{command});
    }
    catch (const mzip::FormatError& error)
    {
        std::cerr << "Invalid archive: " << error.what() << '\n';
        return 3;
    }
    catch (const std::exception& error)
    {
        std::cerr << "Error: " << error.what() << '\n';
        return 1;
    }
}
