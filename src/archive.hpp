#pragma once

#include "transforms.hpp"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <vector>

namespace mzip::detail
{

// On Windows, absolute form with the \\?\ prefix so paths may exceed MAX_PATH.
[[nodiscard]] std::filesystem::path native_long_path(const std::filesystem::path& path);

// Streams a directory tree as deterministic ustar.
class TarStreamSource
{
public:
    explicit TarStreamSource(const std::filesystem::path& root);

    [[nodiscard]] std::uint64_t total_size() const noexcept
    {
        return total_size_;
    }

    // Produces up to `capacity` bytes; 0 means end of stream.
    [[nodiscard]] std::size_t read(Byte* out, std::size_t capacity);

private:
    struct Entry
    {
        std::filesystem::path source;
        std::string stored_path;
        std::uint64_t size = 0;
        bool directory = false;
    };

    void stage_entry_header(const Entry& entry);

    std::vector<Entry> entries_;
    std::uint64_t total_size_ = 0;
    std::size_t entry_index_ = 0;
    Bytes staged_;
    std::size_t staged_offset_ = 0;
    std::ifstream current_file_;
    std::uint64_t current_remaining_ = 0;
    std::size_t current_padding_ = 0;
    bool finished_ = false;
};

// Streaming extraction; rejects paths that would land outside the target root.
class TarExtractor
{
public:
    explicit TarExtractor(std::filesystem::path root);

    void feed(std::span<const Byte> data);
    void finish();

private:
    enum class State
    {
        header,
        file_data,
        long_name_data,
        padding
    };

    void handle_header(const Byte* block);
    [[nodiscard]] std::filesystem::path resolve_stored_path(const std::string& stored) const;

    std::filesystem::path root_;
    State state_ = State::header;
    Bytes pending_;
    std::string long_name_;
    Bytes long_name_data_;
    std::ofstream current_file_;
    std::uint64_t data_remaining_ = 0;
    std::size_t padding_remaining_ = 0;
    unsigned int zero_blocks_ = 0;
    bool done_ = false;
};

} // namespace mzip::detail
