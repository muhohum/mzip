#include "archive.hpp"

#include <mzip/mzip.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <system_error>
#include <utility>

namespace mzip::detail
{
namespace
{

constexpr std::size_t tar_block = 512;
constexpr std::size_t tar_name_field = 100;
constexpr std::size_t tar_prefix_field = 155;
// 8 GiB and up use GNU base-256.
constexpr std::uint64_t octal_size_limit = 1ULL << 33U;
constexpr char long_name_marker[] = "././@LongLink";

[[nodiscard]] std::size_t padded(const std::uint64_t size)
{
    return static_cast<std::size_t>((tar_block - size % tar_block) % tar_block);
}

void write_octal(char* field, const std::size_t width, const std::uint64_t value)
{
    std::uint64_t remaining = value;
    field[width - 1U] = '\0';
    for (std::size_t index = width - 1U; index-- > 0U;)
    {
        field[index] = static_cast<char>('0' + (remaining & 7U));
        remaining >>= 3U;
    }
}

void write_size(char* field, const std::uint64_t value)
{
    if (value < octal_size_limit)
    {
        write_octal(field, 12U, value);
        return;
    }
    std::uint64_t remaining = value;
    for (std::size_t index = 12U; index-- > 1U;)
    {
        field[index] = static_cast<char>(remaining & 0xFFU);
        remaining >>= 8U;
    }
    field[0] = static_cast<char>(0x80);
}

[[nodiscard]] std::uint64_t parse_size(const Byte* field)
{
    if ((field[0] & 0x80U) != 0U)
    {
        std::uint64_t value = field[0] & 0x7FU;
        for (std::size_t index = 1; index < 12U; ++index)
        {
            if (value > (std::numeric_limits<std::uint64_t>::max() >> 8U))
            {
                throw FormatError("archived file size does not fit 64 bits");
            }
            value = (value << 8U) | field[index];
        }
        return value;
    }
    std::uint64_t value = 0;
    for (std::size_t index = 0; index < 12U; ++index)
    {
        const Byte digit = field[index];
        if (digit == 0U || digit == ' ')
        {
            break;
        }
        if (digit < '0' || digit > '7')
        {
            throw FormatError("malformed size field in archived entry");
        }
        value = (value << 3U) | (digit - static_cast<Byte>('0'));
    }
    return value;
}

void finalize_header(std::array<char, tar_block>& header)
{
    std::memset(header.data() + 148, ' ', 8U);
    unsigned int checksum = 0;
    for (const char value : header)
    {
        checksum += static_cast<unsigned char>(value);
    }
    write_octal(header.data() + 148, 7U, checksum);
    header[155] = ' ';
}

[[nodiscard]] std::array<char, tar_block> make_header(const std::string& name,
                                                      const std::uint64_t size, const char type)
{
    std::array<char, tar_block> header{};
    std::string body = name;
    std::string prefix;
    if (body.size() > tar_name_field)
    {
        const std::size_t split = body.rfind('/', tar_prefix_field);
        if (split != std::string::npos && body.size() - split - 1U <= tar_name_field)
        {
            prefix = body.substr(0, split);
            body = body.substr(split + 1U);
        }
    }
    std::memcpy(header.data(), body.data(), std::min(body.size(), tar_name_field));
    std::memcpy(header.data() + 345, prefix.data(), std::min(prefix.size(), tar_prefix_field));
    write_octal(header.data() + 100, 8U, type == '5' ? 0755U : 0644U);
    write_octal(header.data() + 108, 8U, 0U);
    write_octal(header.data() + 116, 8U, 0U);
    write_size(header.data() + 124, size);
    write_octal(header.data() + 136, 12U, 0U);
    header[156] = type;
    std::memcpy(header.data() + 257, "ustar", 6U);
    std::memcpy(header.data() + 263, "00", 2U);
    finalize_header(header);
    return header;
}

[[nodiscard]] bool needs_long_name(const std::string& name)
{
    if (name.size() <= tar_name_field)
    {
        return false;
    }
    const std::size_t split = name.rfind('/', tar_prefix_field);
    return split == std::string::npos || name.size() - split - 1U > tar_name_field;
}

[[nodiscard]] std::uint64_t entry_stream_size(const std::string& name, const std::uint64_t size)
{
    std::uint64_t total = tar_block + size + padded(size);
    if (needs_long_name(name))
    {
        total += tar_block + name.size() + 1U + padded(name.size() + 1U);
    }
    return total;
}

[[nodiscard]] std::string to_utf8(const std::filesystem::path& path)
{
    const std::u8string text = path.generic_u8string();
    return std::string(reinterpret_cast<const char*>(text.data()), text.size());
}

[[nodiscard]] std::filesystem::path from_utf8(const std::string& text)
{
    return std::filesystem::path(
        std::u8string(reinterpret_cast<const char8_t*>(text.data()), text.size()));
}

#if defined(_WIN32)
// Trailing dots or spaces, ':' streams and device names alias other paths on Windows.
[[nodiscard]] bool safe_windows_component(const std::string& text)
{
    if (text.back() == '.' || text.back() == ' ')
    {
        return false;
    }
    for (const char value : text)
    {
        if (static_cast<unsigned char>(value) < 0x20U || value == '<' || value == '>' ||
            value == ':' || value == '"' || value == '|' || value == '?' || value == '*')
        {
            return false;
        }
    }
    std::string base = text.substr(0, text.find('.'));
    std::transform(base.begin(), base.end(), base.begin(), [](const unsigned char value)
                   { return static_cast<char>(std::toupper(value)); });
    if (base == "CON" || base == "PRN" || base == "AUX" || base == "NUL")
    {
        return false;
    }
    const bool numbered_device =
        base.size() == 4U && (base.compare(0, 3, "COM") == 0 || base.compare(0, 3, "LPT") == 0) &&
        base[3] >= '1' && base[3] <= '9';
    return !numbered_device;
}
#endif

} // namespace

std::filesystem::path native_long_path(const std::filesystem::path& path)
{
#if defined(_WIN32)
    const std::filesystem::path absolute = std::filesystem::absolute(path).lexically_normal();
    const std::wstring& native = absolute.native();
    if (native.starts_with(LR"(\\?\)") || native.starts_with(LR"(\\.\)"))
    {
        return absolute;
    }
    if (native.starts_with(LR"(\\)"))
    {
        return std::filesystem::path(LR"(\\?\UNC)" + native.substr(1U));
    }
    return std::filesystem::path(LR"(\\?\)" + native);
#else
    return path;
#endif
}

TarStreamSource::TarStreamSource(const std::filesystem::path& root)
{
    const std::string base = to_utf8(root.filename());
    if (base.empty())
    {
        throw std::runtime_error("cannot archive a root without a directory name");
    }

    entries_.push_back(Entry{root, base + "/", 0U, true});
    for (std::filesystem::recursive_directory_iterator
             iterator(root, std::filesystem::directory_options::none),
         end = std::filesystem::recursive_directory_iterator();
         iterator != end; ++iterator)
    {
        const std::filesystem::file_status status = iterator->symlink_status();
        const std::string stored = base + "/" + to_utf8(iterator->path().lexically_relative(root));
        if (status.type() == std::filesystem::file_type::directory)
        {
            entries_.push_back(Entry{iterator->path(), stored + "/", 0U, true});
        }
        else if (status.type() == std::filesystem::file_type::regular)
        {
            entries_.push_back(Entry{iterator->path(), stored, iterator->file_size(), false});
        }
        else
        {
            throw std::runtime_error("only regular files and directories can be archived: " +
                                     iterator->path().string());
        }
    }
    std::sort(entries_.begin(), entries_.end(), [](const Entry& left, const Entry& right)
              { return left.stored_path < right.stored_path; });

    for (const Entry& entry : entries_)
    {
        total_size_ += entry_stream_size(entry.stored_path, entry.size);
    }
    total_size_ += 2U * tar_block;
}

void TarStreamSource::stage_entry_header(const Entry& entry)
{
    staged_.clear();
    staged_offset_ = 0;
    if (needs_long_name(entry.stored_path))
    {
        const std::uint64_t name_size = entry.stored_path.size() + 1U;
        const auto long_header = make_header(long_name_marker, name_size, 'L');
        staged_.insert(staged_.end(), long_header.begin(), long_header.end());
        staged_.insert(staged_.end(), entry.stored_path.begin(), entry.stored_path.end());
        staged_.push_back(0U);
        staged_.insert(staged_.end(), padded(name_size), Byte{0U});
    }
    const auto header = make_header(entry.stored_path, entry.directory ? 0U : entry.size,
                                    entry.directory ? '5' : '0');
    staged_.insert(staged_.end(), header.begin(), header.end());
}

std::size_t TarStreamSource::read(Byte* out, const std::size_t capacity)
{
    std::size_t produced = 0;
    while (produced < capacity)
    {
        if (staged_offset_ < staged_.size())
        {
            const std::size_t chunk =
                std::min(capacity - produced, staged_.size() - staged_offset_);
            std::memcpy(out + produced, staged_.data() + staged_offset_, chunk);
            staged_offset_ += chunk;
            produced += chunk;
            continue;
        }
        if (current_remaining_ > 0U)
        {
            const std::size_t chunk = static_cast<std::size_t>(
                std::min<std::uint64_t>(capacity - produced, current_remaining_));
            current_file_.read(reinterpret_cast<char*>(out + produced),
                               static_cast<std::streamsize>(chunk));
            if (current_file_.gcount() != static_cast<std::streamsize>(chunk))
            {
                throw std::runtime_error("a file changed while it was being archived");
            }
            current_remaining_ -= chunk;
            produced += chunk;
            if (current_remaining_ == 0U)
            {
                if (current_file_.peek() != std::char_traits<char>::eof())
                {
                    throw std::runtime_error("a file changed while it was being archived");
                }
                current_file_.close();
                staged_.assign(current_padding_, Byte{0U});
                staged_offset_ = 0;
            }
            continue;
        }
        if (entry_index_ < entries_.size())
        {
            const Entry& entry = entries_[entry_index_++];
            stage_entry_header(entry);
            if (!entry.directory && entry.size > 0U)
            {
                current_file_.open(native_long_path(entry.source), std::ios::binary);
                if (!current_file_)
                {
                    throw std::runtime_error("cannot open file for archiving: " +
                                             entry.source.string());
                }
                current_remaining_ = entry.size;
                current_padding_ = padded(entry.size);
            }
            continue;
        }
        if (!finished_)
        {
            staged_.assign(2U * tar_block, Byte{0U});
            staged_offset_ = 0;
            finished_ = true;
            continue;
        }
        break;
    }
    return produced;
}

TarExtractor::TarExtractor(std::filesystem::path root) : root_(std::move(root)) {}

std::filesystem::path TarExtractor::resolve_stored_path(const std::string& stored) const
{
    if (stored.empty())
    {
        throw FormatError("archived entry has an empty path");
    }
    const std::filesystem::path relative = from_utf8(stored);
    if (relative.is_absolute() || relative.has_root_name() || relative.has_root_directory())
    {
        throw FormatError("archived entry uses an absolute path");
    }
    std::filesystem::path result = root_;
    for (const std::filesystem::path& component : relative)
    {
        const std::string text = to_utf8(component);
        if (text == "..")
        {
            throw FormatError("archived entry escapes the output directory");
        }
        if (text.empty() || text == ".")
        {
            continue;
        }
#if defined(_WIN32)
        if (!safe_windows_component(text))
        {
            throw FormatError("archived entry name is unsafe on this platform");
        }
#endif
        result /= component;
    }
    return result;
}

void TarExtractor::handle_header(const Byte* block)
{
    const bool all_zero = std::all_of(block, block + tar_block, [](Byte b) { return b == 0U; });
    if (all_zero)
    {
        ++zero_blocks_;
        if (zero_blocks_ >= 2U)
        {
            done_ = true;
        }
        return;
    }
    if (zero_blocks_ != 0U)
    {
        throw FormatError("archived data resumes after an end-of-archive block");
    }

    const char type = static_cast<char>(block[156]);
    const std::uint64_t size = parse_size(block + 124);

    if (type == 'L')
    {
        if (!long_name_.empty())
        {
            throw FormatError("consecutive long-name entries in archived data");
        }
        if (size == 0U || size > 65536U)
        {
            throw FormatError("archived long name has an invalid size");
        }
        long_name_data_.clear();
        data_remaining_ = size;
        padding_remaining_ = padded(size);
        state_ = State::long_name_data;
        return;
    }

    std::string stored;
    if (!long_name_.empty())
    {
        stored = std::exchange(long_name_, {});
    }
    else
    {
        const char* name = reinterpret_cast<const char*>(block);
        const char* prefix = reinterpret_cast<const char*>(block) + 345;
        const std::string prefix_text(prefix, strnlen(prefix, tar_prefix_field));
        stored.assign(name, strnlen(name, tar_name_field));
        if (!prefix_text.empty())
        {
            stored = prefix_text + "/" + stored;
        }
    }

    if (type == '5')
    {
        if (size != 0U)
        {
            throw FormatError("archived directory entry carries data");
        }
        std::filesystem::create_directories(native_long_path(resolve_stored_path(stored)));
        return;
    }
    if (type != '0' && type != '\0')
    {
        throw FormatError("unsupported entry type in archived data");
    }

    const std::filesystem::path target = native_long_path(resolve_stored_path(stored));
    std::filesystem::create_directories(target.parent_path());
    current_file_.open(target, std::ios::binary | std::ios::trunc);
    if (!current_file_)
    {
        throw std::runtime_error("cannot create extracted file: " + target.string());
    }
    if (size == 0U)
    {
        current_file_.close();
        return;
    }
    data_remaining_ = size;
    padding_remaining_ = padded(size);
    state_ = State::file_data;
}

void TarExtractor::feed(const std::span<const Byte> data)
{
    std::size_t offset = 0;
    while (offset < data.size())
    {
        if (done_)
        {
            throw FormatError("archived data continues past the end of the archive");
        }
        switch (state_)
        {
        case State::header:
        {
            const std::size_t chunk = std::min(tar_block - pending_.size(), data.size() - offset);
            pending_.insert(pending_.end(), data.begin() + static_cast<std::ptrdiff_t>(offset),
                            data.begin() + static_cast<std::ptrdiff_t>(offset + chunk));
            offset += chunk;
            if (pending_.size() == tar_block)
            {
                handle_header(pending_.data());
                pending_.clear();
            }
            break;
        }
        case State::file_data:
        case State::long_name_data:
        {
            const std::size_t chunk = static_cast<std::size_t>(
                std::min<std::uint64_t>(data.size() - offset, data_remaining_));
            if (state_ == State::long_name_data)
            {
                long_name_data_.insert(long_name_data_.end(),
                                       data.begin() + static_cast<std::ptrdiff_t>(offset),
                                       data.begin() + static_cast<std::ptrdiff_t>(offset + chunk));
            }
            else
            {
                current_file_.write(reinterpret_cast<const char*>(data.data() + offset),
                                    static_cast<std::streamsize>(chunk));
                if (!current_file_)
                {
                    throw std::runtime_error("failed while writing an extracted file");
                }
            }
            data_remaining_ -= chunk;
            offset += chunk;
            if (data_remaining_ == 0U)
            {
                if (state_ == State::long_name_data)
                {
                    long_name_data_.push_back(0U);
                    long_name_.assign(reinterpret_cast<const char*>(long_name_data_.data()));
                    if (long_name_.empty())
                    {
                        throw FormatError("archived long name is empty");
                    }
                }
                else
                {
                    current_file_.close();
                }
                state_ = State::padding;
            }
            break;
        }
        case State::padding:
        {
            const std::size_t chunk = std::min(data.size() - offset, padding_remaining_);
            offset += chunk;
            padding_remaining_ -= chunk;
            break;
        }
        }
        if (state_ == State::padding && padding_remaining_ == 0U)
        {
            state_ = State::header;
        }
    }
}

void TarExtractor::finish()
{
    if (!done_ || state_ != State::header || !pending_.empty() || !long_name_.empty())
    {
        throw FormatError("archived data ended before the end-of-archive marker");
    }
}

} // namespace mzip::detail
