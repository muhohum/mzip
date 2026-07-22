#include "transforms.hpp"

#include <mzip/mzip.hpp>

#include <algorithm>
#include <array>
#include <limits>
#include <memory>
#include <numeric>
#include <stdexcept>
#include <utility>
#include <vector>

namespace mzip::detail
{
namespace
{

constexpr std::size_t alphabet_size = 256;

// LZMA-style binary range coder with 12-bit adaptive probabilities.
constexpr unsigned int probability_bits = 12;
constexpr std::uint16_t probability_one = 1U << probability_bits;
constexpr std::uint16_t probability_half = probability_one / 2U;
constexpr unsigned int adaptation_shift = 5;
constexpr std::uint32_t range_top = 1U << 24;

// Cap on speculative allocations driven by untrusted headers.
constexpr std::size_t speculative_reserve_limit = std::size_t{1} << 20U;

class RangeEncoder
{
public:
    void encode_bit(std::uint16_t& probability, const unsigned int bit)
    {
        const std::uint32_t bound = (range_ >> probability_bits) * probability;
        if (bit == 0U)
        {
            range_ = bound;
            probability = static_cast<std::uint16_t>(
                probability + ((probability_one - probability) >> adaptation_shift));
        }
        else
        {
            low_ += bound;
            range_ -= bound;
            probability =
                static_cast<std::uint16_t>(probability - (probability >> adaptation_shift));
        }
        while (range_ < range_top)
        {
            range_ <<= 8U;
            shift_low();
        }
    }

    void reserve(const std::size_t bytes)
    {
        output_.reserve(bytes);
    }

    [[nodiscard]] std::size_t output_size() const noexcept
    {
        return output_.size();
    }

    [[nodiscard]] Bytes finish()
    {
        for (int iteration = 0; iteration < 5; ++iteration)
        {
            shift_low();
        }
        return std::move(output_);
    }

private:
    void shift_low()
    {
        if (static_cast<std::uint32_t>(low_) < 0xFF000000U ||
            static_cast<std::uint32_t>(low_ >> 32U) != 0U)
        {
            Byte pending = cache_;
            do
            {
                output_.push_back(static_cast<Byte>(pending + static_cast<Byte>(low_ >> 32U)));
                pending = 0xFFU;
            } while (--cache_size_ != 0U);
            cache_ = static_cast<Byte>(low_ >> 24U);
        }
        ++cache_size_;
        low_ = (low_ & 0x00FFFFFFULL) << 8U;
    }

    Bytes output_;
    std::uint64_t low_ = 0;
    std::uint32_t range_ = 0xFFFFFFFFU;
    std::uint64_t cache_size_ = 1;
    Byte cache_ = 0;
};

class RangeDecoder
{
public:
    explicit RangeDecoder(const std::span<const Byte> bytes) : bytes_(bytes)
    {
        for (int iteration = 0; iteration < 5; ++iteration)
        {
            code_ = (code_ << 8U) | next_byte();
        }
    }

    [[nodiscard]] unsigned int decode_bit(std::uint16_t& probability)
    {
        const std::uint32_t bound = (range_ >> probability_bits) * probability;
        unsigned int bit = 0;
        if (code_ < bound)
        {
            range_ = bound;
            probability = static_cast<std::uint16_t>(
                probability + ((probability_one - probability) >> adaptation_shift));
        }
        else
        {
            code_ -= bound;
            range_ -= bound;
            probability =
                static_cast<std::uint16_t>(probability - (probability >> adaptation_shift));
            bit = 1;
        }
        while (range_ < range_top)
        {
            range_ <<= 8U;
            code_ = (code_ << 8U) | next_byte();
        }
        return bit;
    }

    [[nodiscard]] std::size_t position() const noexcept
    {
        return position_;
    }

private:
    [[nodiscard]] std::uint32_t next_byte()
    {
        if (position_ >= bytes_.size())
        {
            throw FormatError("truncated range-coded stream");
        }
        return bytes_[position_++];
    }

    std::span<const Byte> bytes_;
    std::size_t position_ = 0;
    std::uint32_t code_ = 0;
    std::uint32_t range_ = 0xFFFFFFFFU;
};

// Contexts: digit position within a run, previous-literal bucket, after-zero-run.
constexpr unsigned int previous_class_count = 4;
constexpr unsigned int digit_position_contexts = 4;
constexpr unsigned int literal_bucket_count = 5;
constexpr unsigned int small_literal_max = 3;
constexpr unsigned int medium_literal_max = 15;
constexpr unsigned int literal_tree_size = 256;

struct RunSymbolModel
{
    enum PreviousClass : unsigned int
    {
        at_start = 0,
        zero_digit = 1,
        repeat_digit = 2,
        literal = 3
    };

    std::array<std::uint16_t, previous_class_count> is_zero_digit;
    std::array<std::uint16_t, digit_position_contexts> zero_digit_value;
    std::array<std::uint16_t, digit_position_contexts> is_repeat_digit;
    std::array<std::uint16_t, digit_position_contexts> repeat_digit_value;
    std::array<std::array<std::uint16_t, literal_tree_size>, literal_bucket_count * 2U>
        literal_tree;

    unsigned int previous_class = at_start;
    unsigned int digit_position = 0;
    unsigned int last_literal_bucket = 0;
    bool extension_allowed = false;

    RunSymbolModel()
    {
        is_zero_digit.fill(probability_half);
        zero_digit_value.fill(probability_half);
        is_repeat_digit.fill(probability_half);
        repeat_digit_value.fill(probability_half);
        for (auto& tree : literal_tree)
        {
            tree.fill(probability_half);
        }
    }

    [[nodiscard]] unsigned int zero_digit_context() const noexcept
    {
        const unsigned int position = previous_class == zero_digit ? digit_position + 1U : 0U;
        return std::min(position, digit_position_contexts - 1U);
    }

    [[nodiscard]] unsigned int repeat_digit_context() const noexcept
    {
        const unsigned int position = previous_class == repeat_digit ? digit_position + 1U : 0U;
        return std::min(position, digit_position_contexts - 1U);
    }

    [[nodiscard]] std::uint16_t& literal_probability(const unsigned int node) noexcept
    {
        const unsigned int after_zero = previous_class == zero_digit ? literal_bucket_count : 0U;
        return literal_tree[after_zero + last_literal_bucket][node];
    }

    void advance(const Symbol symbol) noexcept
    {
        if (symbol <= run_b)
        {
            digit_position = previous_class == zero_digit ? digit_position + 1U : 0U;
            previous_class = zero_digit;
            extension_allowed = false;
        }
        else if (symbol <= run_d)
        {
            digit_position = previous_class == repeat_digit ? digit_position + 1U : 0U;
            previous_class = repeat_digit;
        }
        else
        {
            const unsigned int value = symbol - run_literal_offset;
            if (value <= small_literal_max)
            {
                last_literal_bucket = value;
            }
            else
            {
                last_literal_bucket = value <= medium_literal_max ? literal_bucket_count - 1U : 0U;
            }
            previous_class = literal;
            extension_allowed = true;
        }
    }
};

constexpr std::uint32_t empty_slot = std::numeric_limits<std::uint32_t>::max();

template <typename Char> class SuffixArraySorter
{
public:
    // `text` must end with a unique sentinel 0; every value must be below `alphabet`.
    [[nodiscard]] static std::vector<std::uint32_t> sort(const std::span<const Char> text,
                                                         const std::uint32_t alphabet)
    {
        std::vector<std::uint32_t> suffix_array(text.size(), empty_slot);
        if (text.size() == 1U)
        {
            suffix_array[0] = 0U;
            return suffix_array;
        }
        SuffixArraySorter sorter(text, alphabet);
        sorter.run(suffix_array);
        return suffix_array;
    }

private:
    SuffixArraySorter(const std::span<const Char> text, const std::uint32_t alphabet)
        : text_(text), size_(static_cast<std::uint32_t>(text.size())), is_s_type_(text.size()),
          counts_(alphabet, 0U), boundaries_(alphabet, 0U)
    {
        is_s_type_[size_ - 1U] = 1U;
        for (std::uint32_t index = size_ - 1U; index-- > 0U;)
        {
            is_s_type_[index] =
                text_[index] < text_[index + 1U] ||
                        (text_[index] == text_[index + 1U] && is_s_type_[index + 1U] != 0U)
                    ? 1U
                    : 0U;
        }
        for (const Char value : text_)
        {
            ++counts_[value];
        }
    }

    [[nodiscard]] bool is_lms(const std::uint32_t index) const noexcept
    {
        return index > 0U && is_s_type_[index] != 0U && is_s_type_[index - 1U] == 0U;
    }

    void reset_to_bucket_heads() noexcept
    {
        std::uint32_t sum = 0U;
        for (std::size_t value = 0; value < counts_.size(); ++value)
        {
            boundaries_[value] = sum;
            sum += counts_[value];
        }
    }

    void reset_to_bucket_tails() noexcept
    {
        std::uint32_t sum = 0U;
        for (std::size_t value = 0; value < counts_.size(); ++value)
        {
            sum += counts_[value];
            boundaries_[value] = sum;
        }
    }

    // L-suffixes left to right from bucket heads, then S-suffixes right to left from tails.
    void induce(std::vector<std::uint32_t>& suffix_array)
    {
        reset_to_bucket_heads();
        for (std::uint32_t index = 0; index < size_; ++index)
        {
            const std::uint32_t suffix = suffix_array[index];
            if (suffix == empty_slot || suffix == 0U || is_s_type_[suffix - 1U] != 0U)
            {
                continue;
            }
            suffix_array[boundaries_[text_[suffix - 1U]]++] = suffix - 1U;
        }
        reset_to_bucket_tails();
        for (std::uint32_t index = size_; index-- > 0U;)
        {
            const std::uint32_t suffix = suffix_array[index];
            if (suffix == empty_slot || suffix == 0U || is_s_type_[suffix - 1U] == 0U)
            {
                continue;
            }
            suffix_array[--boundaries_[text_[suffix - 1U]]] = suffix - 1U;
        }
    }

    [[nodiscard]] bool lms_substrings_equal(const std::uint32_t left,
                                            const std::uint32_t right) const
    {
        for (std::uint32_t offset = 0;; ++offset)
        {
            const std::uint32_t left_index = left + offset;
            const std::uint32_t right_index = right + offset;
            if (left_index >= size_ || right_index >= size_ ||
                text_[left_index] != text_[right_index] ||
                is_s_type_[left_index] != is_s_type_[right_index])
            {
                return false;
            }
            if (offset > 0U && (is_lms(left_index) || is_lms(right_index)))
            {
                return is_lms(left_index) && is_lms(right_index);
            }
        }
    }

    void run(std::vector<std::uint32_t>& suffix_array)
    {
        // One induction round over LMS seeds sorts the LMS substrings.
        reset_to_bucket_tails();
        for (std::uint32_t index = 1; index < size_; ++index)
        {
            if (is_lms(index))
            {
                suffix_array[--boundaries_[text_[index]]] = index;
            }
        }
        induce(suffix_array);

        std::vector<std::uint32_t> lms_positions;
        std::vector<std::uint32_t> reduced_text;
        std::uint32_t name_count = 1U;
        {
            std::vector<std::uint32_t> sorted_lms;
            for (const std::uint32_t suffix : suffix_array)
            {
                if (suffix != empty_slot && is_lms(suffix))
                {
                    sorted_lms.push_back(suffix);
                }
            }
            std::vector<std::uint32_t> name_by_position(size_, empty_slot);
            name_by_position[sorted_lms[0]] = 0U;
            for (std::size_t rank = 1; rank < sorted_lms.size(); ++rank)
            {
                if (!lms_substrings_equal(sorted_lms[rank - 1U], sorted_lms[rank]))
                {
                    ++name_count;
                }
                name_by_position[sorted_lms[rank]] = name_count - 1U;
            }

            lms_positions.reserve(sorted_lms.size());
            reduced_text.reserve(sorted_lms.size());
            for (std::uint32_t index = 1; index < size_; ++index)
            {
                if (is_lms(index))
                {
                    lms_positions.push_back(index);
                    reduced_text.push_back(name_by_position[index]);
                }
            }
        }

        const auto lms_count = static_cast<std::uint32_t>(lms_positions.size());
        std::vector<std::uint32_t> reduced_order;
        if (name_count == lms_count)
        {
            reduced_order.assign(lms_count, 0U);
            for (std::uint32_t index = 0; index < lms_count; ++index)
            {
                reduced_order[reduced_text[index]] = index;
            }
        }
        else
        {
            reduced_order = SuffixArraySorter<std::uint32_t>::sort(reduced_text, name_count);
        }
        reduced_text = std::vector<std::uint32_t>();

        // Place the sorted LMS suffixes and induce the complete order.
        std::fill(suffix_array.begin(), suffix_array.end(), empty_slot);
        reset_to_bucket_tails();
        for (std::uint32_t rank = lms_count; rank-- > 0U;)
        {
            const std::uint32_t position = lms_positions[reduced_order[rank]];
            suffix_array[--boundaries_[text_[position]]] = position;
        }
        induce(suffix_array);
    }

    std::span<const Char> text_;
    std::uint32_t size_;
    std::vector<std::uint8_t> is_s_type_;
    std::vector<std::uint32_t> counts_;
    std::vector<std::uint32_t> boundaries_;
};

} // namespace

BwtResult bwt_encode(const std::span<const Byte> input)
{
    const std::size_t size = input.size();
    if (size == 0U)
    {
        return {};
    }
    if (size >= std::numeric_limits<std::uint32_t>::max())
    {
        throw std::invalid_argument("BWT block is too large");
    }

    // Values are shifted by one; 0 is the sentinel.
    std::vector<std::uint32_t> suffix_array;
    {
        std::vector<std::uint16_t> text(size + 1U);
        for (std::size_t index = 0; index < size; ++index)
        {
            text[index] = static_cast<std::uint16_t>(input[index] + 1U);
        }
        text[size] = 0U;
        suffix_array = SuffixArraySorter<std::uint16_t>::sort(text, alphabet_size + 1U);
    }

    // The row preceded by the sentinel (suffix 0) is omitted and kept as the primary index.
    BwtResult result;
    result.data.resize(size);
    std::size_t output_index = 0;
    for (std::size_t row = 0; row < suffix_array.size(); ++row)
    {
        const std::uint32_t suffix = suffix_array[row];
        if (suffix == 0U)
        {
            result.primary_index = static_cast<std::uint32_t>(row);
            continue;
        }
        result.data[output_index++] = input[suffix - 1U];
    }
    return result;
}

Bytes bwt_decode(const std::span<const Byte> input, const std::uint32_t primary_index)
{
    const std::size_t size = input.size();
    if (size == 0U)
    {
        if (primary_index != 0U)
        {
            throw FormatError("invalid BWT index for an empty block");
        }
        return {};
    }
    if (primary_index == 0U || static_cast<std::size_t>(primary_index) > size)
    {
        throw FormatError("BWT primary index is outside the block");
    }

    // LF-mapping inversion; the sentinel is row primary_index in L and row 0 in F.
    std::array<std::uint32_t, alphabet_size> counts{};
    std::vector<std::uint32_t> occurrence(size);
    for (std::size_t index = 0; index < size; ++index)
    {
        occurrence[index] = counts[input[index]]++;
    }

    std::array<std::uint32_t, alphabet_size> starts{};
    std::uint32_t total = 1;
    for (std::size_t symbol = 0; symbol < alphabet_size; ++symbol)
    {
        starts[symbol] = total;
        total += counts[symbol];
    }

    Bytes output(size);
    std::size_t row = 0;
    for (std::size_t index = size; index-- > 0U;)
    {
        const std::size_t stored = row < primary_index ? row : row - 1U;
        const Byte value = input[stored];
        output[index] = value;
        row = starts[value] + occurrence[stored];
    }
    return output;
}

void mtf_encode(Bytes& data)
{
    std::array<Byte, alphabet_size> symbols{};
    std::iota(symbols.begin(), symbols.end(), Byte{0});

    for (Byte& value : data)
    {
        const Byte current = value;
        std::size_t position = 0;
        while (symbols[position] != current)
        {
            ++position;
        }
        value = static_cast<Byte>(position);
        for (std::size_t index = position; index > 0U; --index)
        {
            symbols[index] = symbols[index - 1U];
        }
        symbols[0] = current;
    }
}

void mtf_decode(Bytes& data)
{
    std::array<Byte, alphabet_size> symbols{};
    std::iota(symbols.begin(), symbols.end(), Byte{0});

    for (Byte& value : data)
    {
        const std::size_t position = value;
        const Byte decoded = symbols[position];
        value = decoded;
        for (std::size_t index = position; index > 0U; --index)
        {
            symbols[index] = symbols[index - 1U];
        }
        symbols[0] = decoded;
    }
}

namespace
{

void encode_symbol(RangeEncoder& encoder, RunSymbolModel& model, const Symbol symbol)
{
    const unsigned int not_zero_digit = symbol <= run_b ? 0U : 1U;
    encoder.encode_bit(model.is_zero_digit[model.previous_class], not_zero_digit);
    if (not_zero_digit == 0U)
    {
        encoder.encode_bit(model.zero_digit_value[model.zero_digit_context()],
                           symbol == run_b ? 1U : 0U);
    }
    else
    {
        unsigned int is_literal = 1U;
        if (model.extension_allowed)
        {
            is_literal = symbol > run_d ? 1U : 0U;
            encoder.encode_bit(model.is_repeat_digit[model.repeat_digit_context()], is_literal);
            if (is_literal == 0U)
            {
                encoder.encode_bit(model.repeat_digit_value[model.repeat_digit_context()],
                                   symbol == run_d ? 1U : 0U);
            }
        }
        if (is_literal == 1U)
        {
            const unsigned int value = symbol - run_literal_offset;
            unsigned int node = 1;
            for (unsigned int shift = 8U; shift-- > 0U;)
            {
                const unsigned int bit = (value >> shift) & 1U;
                encoder.encode_bit(model.literal_probability(node), bit);
                node = node * 2U + bit;
            }
        }
    }
    model.advance(symbol);
}

[[nodiscard]] Symbol decode_symbol(RangeDecoder& decoder, RunSymbolModel& model)
{
    Symbol symbol = 0;
    if (decoder.decode_bit(model.is_zero_digit[model.previous_class]) == 0U)
    {
        symbol = decoder.decode_bit(model.zero_digit_value[model.zero_digit_context()]) != 0U
                     ? run_b
                     : run_a;
    }
    else
    {
        unsigned int is_literal = 1U;
        if (model.extension_allowed)
        {
            is_literal = decoder.decode_bit(model.is_repeat_digit[model.repeat_digit_context()]);
            if (is_literal == 0U)
            {
                symbol =
                    decoder.decode_bit(model.repeat_digit_value[model.repeat_digit_context()]) != 0U
                        ? run_d
                        : run_c;
            }
        }
        if (is_literal == 1U)
        {
            unsigned int node = 1;
            for (unsigned int count = 0; count < 8U; ++count)
            {
                node = node * 2U + decoder.decode_bit(model.literal_probability(node));
            }
            const unsigned int value = node - literal_tree_size;
            if (value == 0U)
            {
                throw FormatError("invalid literal in range-coded stream");
            }
            symbol = static_cast<Symbol>(value + run_literal_offset);
        }
    }
    model.advance(symbol);
    return symbol;
}

} // namespace

std::optional<EncodedStream> rc_encode(const std::span<const Byte> input,
                                       const std::size_t min_extension_run,
                                       const std::size_t size_limit)
{
    if (input.empty())
    {
        return EncodedStream{};
    }

    RangeEncoder encoder;
    encoder.reserve(std::min(size_limit, input.size() / 4U + 64U));
    RunSymbolModel model;
    std::size_t symbol_count = 0;

    const auto emit = [&](const Symbol symbol)
    {
        encode_symbol(encoder, model, symbol);
        ++symbol_count;
    };
    // Bijective base 2, least significant digit first.
    const auto emit_run_length =
        [&](std::size_t length, const Symbol digit_one, const Symbol digit_two)
    {
        while (length > 0U)
        {
            --length;
            emit((length & 1U) != 0U ? digit_two : digit_one);
            length /= 2U;
        }
    };

    std::size_t index = 0;
    while (index < input.size())
    {
        const Byte value = input[index];
        std::size_t run_length = 0;
        while (index < input.size() && input[index] == value)
        {
            ++run_length;
            ++index;
        }

        if (value == 0U)
        {
            emit_run_length(run_length, run_a, run_b);
        }
        else if (run_length < min_extension_run)
        {
            // Short repeats stay as literals; both spellings decode identically.
            for (std::size_t repeat = 0; repeat < run_length; ++repeat)
            {
                emit(static_cast<Symbol>(value + run_literal_offset));
            }
        }
        else
        {
            emit(static_cast<Symbol>(value + run_literal_offset));
            emit_run_length(run_length - 1U, run_c, run_d);
        }

        if (encoder.output_size() >= size_limit)
        {
            return std::nullopt;
        }
    }

    EncodedStream result;
    result.payload = encoder.finish();
    result.symbol_count = symbol_count;
    return result;
}

Bytes rc_decode(const std::span<const Byte> payload, const std::size_t symbol_count,
                const std::size_t expected_size)
{
    Bytes output;
    if (symbol_count == 0U)
    {
        if (!payload.empty() || expected_size != 0U)
        {
            throw FormatError("empty run stream with content");
        }
        return output;
    }
    output.reserve(std::min(expected_size, speculative_reserve_limit));

    RangeDecoder decoder(payload);
    RunSymbolModel model;

    std::uint64_t run_length = 0;
    std::uint64_t magnitude = 1;
    Byte run_value = 0;
    const auto flush_run = [&]
    {
        if (run_length != 0U)
        {
            output.insert(output.end(), static_cast<std::size_t>(run_length), run_value);
            run_length = 0U;
        }
        magnitude = 1U;
    };

    for (std::size_t remaining = symbol_count; remaining-- > 0U;)
    {
        const Symbol symbol = decode_symbol(decoder, model);
        if (symbol <= run_b)
        {
            if (run_value != 0U)
            {
                flush_run();
                run_value = 0U;
            }
            run_length += symbol == run_a ? magnitude : 2U * magnitude;
            magnitude *= 2U;
            if (run_length > expected_size - output.size())
            {
                throw FormatError("run length exceeds the block size");
            }
        }
        else if (symbol <= run_d)
        {
            // The model only offers extension bits right after a literal.
            run_length += symbol == run_c ? magnitude : 2U * magnitude;
            magnitude *= 2U;
            if (run_length > expected_size - output.size())
            {
                throw FormatError("run length exceeds the block size");
            }
        }
        else
        {
            flush_run();
            if (output.size() >= expected_size)
            {
                throw FormatError("run coding output exceeds the block size");
            }
            run_value = static_cast<Byte>(symbol - run_literal_offset);
            output.push_back(run_value);
        }
    }
    flush_run();

    if (output.size() != expected_size)
    {
        throw FormatError("run coding output has an unexpected size");
    }
    if (decoder.position() != payload.size())
    {
        throw FormatError("range-coded stream contains trailing bytes");
    }
    return output;
}

namespace
{

constexpr std::uint16_t cm_half = 1U << 15U;
constexpr unsigned int cm_apm_columns = 17;

void cm_toward_one(std::uint16_t& probability, const unsigned int shift)
{
    probability = static_cast<std::uint16_t>(probability + ((probability ^ 0xFFFFU) >> shift));
}

void cm_toward_zero(std::uint16_t& probability, const unsigned int shift)
{
    probability = static_cast<std::uint16_t>(probability - (probability >> shift));
}

// Order-0 and two order-1 counters mixed 7:7:2, then an adaptive map per tree node.
struct CmModel
{
    std::vector<std::uint16_t> order0;
    std::vector<std::uint16_t> order1;
    std::vector<std::uint16_t> apm;
    unsigned int previous = 0;
    unsigned int before_previous = 0;
    unsigned int run = 0;

    CmModel() : order0(256U, cm_half), order1(256U * 256U, cm_half), apm(512U * cm_apm_columns)
    {
        for (unsigned int row = 0; row < 512U; ++row)
        {
            for (unsigned int step = 0; step < cm_apm_columns; ++step)
            {
                apm[row * cm_apm_columns + step] =
                    static_cast<std::uint16_t>((step << 12U) - (step == 16U ? 1U : 0U));
            }
        }
    }

    struct Slots
    {
        std::uint16_t* zero_order;
        std::uint16_t* first_order;
        std::uint16_t* map_low;
        std::uint16_t* map_high;
        std::uint32_t scaled;
    };

    [[nodiscard]] Slots predict(const unsigned int node, const unsigned int run_flag)
    {
        Slots slots{};
        slots.zero_order = &order0[node];
        slots.first_order = &order1[previous * 256U + node];
        const std::uint32_t mixed =
            ((static_cast<std::uint32_t>(*slots.zero_order) + *slots.first_order) * 7U +
             2U * order1[before_previous * 256U + node]) >>
            4U;
        std::uint16_t* row = &apm[(node * 2U + run_flag) * cm_apm_columns];
        slots.map_low = row + (mixed >> 12U);
        slots.map_high = slots.map_low + 1U;
        const int left = *slots.map_low;
        const int right = *slots.map_high;
        const int interpolated = left + (((right - left) * static_cast<int>(mixed & 4095U)) >> 12);
        slots.scaled = static_cast<std::uint32_t>(interpolated * 3 + static_cast<int>(mixed));
        return slots;
    }

    void update(const Slots& slots, const unsigned int bit)
    {
        if (bit != 0U)
        {
            cm_toward_one(*slots.zero_order, 2U);
            cm_toward_one(*slots.first_order, 4U);
            cm_toward_one(*slots.map_low, 6U);
            cm_toward_one(*slots.map_high, 6U);
        }
        else
        {
            cm_toward_zero(*slots.zero_order, 2U);
            cm_toward_zero(*slots.first_order, 4U);
            cm_toward_zero(*slots.map_low, 6U);
            cm_toward_zero(*slots.map_high, 6U);
        }
    }

    [[nodiscard]] unsigned int run_flag() noexcept
    {
        if (previous == before_previous)
        {
            ++run;
        }
        else
        {
            run = 0;
        }
        return run > 2U ? 1U : 0U;
    }

    void advance(const unsigned int byte) noexcept
    {
        before_previous = previous;
        previous = byte;
    }
};

constexpr std::size_t lzp_min_match = 128;
constexpr std::size_t lzp_context_size = 8;

[[nodiscard]] std::uint32_t lzp_slot(const std::uint64_t context,
                                     const unsigned int hash_bits) noexcept
{
    return static_cast<std::uint32_t>((context * 0x9E3779B97F4A7C15ULL) >> (64U - hash_bits));
}

void lzp_put_length(Bytes& output, std::size_t value)
{
    while (value >= 128U)
    {
        output.push_back(static_cast<Byte>((value & 127U) | 128U));
        value >>= 7U;
    }
    output.push_back(static_cast<Byte>(value));
}

} // namespace

std::optional<Bytes> cm_encode(const std::span<const Byte> input, const std::size_t size_limit)
{
    Bytes output;
    output.reserve(std::min(size_limit, input.size() / 2U + 64U));
    const auto model = std::make_unique<CmModel>();
    std::uint32_t low = 0;
    std::uint32_t high = 0xFFFFFFFFU;

    for (const Byte value : input)
    {
        const unsigned int flag = model->run_flag();
        unsigned int node = 1;
        for (unsigned int shift = 8U; shift-- > 0U;)
        {
            const unsigned int bit = (value >> shift) & 1U;
            const CmModel::Slots slots = model->predict(node, flag);
            const std::uint32_t mid =
                low + static_cast<std::uint32_t>(
                          (static_cast<std::uint64_t>(high - low) * slots.scaled) >> 18U);
            if (bit != 0U)
            {
                high = mid;
            }
            else
            {
                low = mid + 1U;
            }
            while (((low ^ high) & 0xFF000000U) == 0U)
            {
                output.push_back(static_cast<Byte>(low >> 24U));
                low <<= 8U;
                high = (high << 8U) | 0xFFU;
            }
            model->update(slots, bit);
            node = node * 2U + bit;
        }
        model->advance(node & 255U);
        if (output.size() > size_limit)
        {
            return std::nullopt;
        }
    }
    for (int iteration = 0; iteration < 4; ++iteration)
    {
        output.push_back(static_cast<Byte>(low >> 24U));
        low <<= 8U;
    }
    if (output.size() > size_limit)
    {
        return std::nullopt;
    }
    return output;
}

Bytes cm_decode(const std::span<const Byte> payload, const std::size_t expected_size)
{
    Bytes output;
    output.reserve(expected_size);
    const auto model = std::make_unique<CmModel>();
    std::uint32_t low = 0;
    std::uint32_t high = 0xFFFFFFFFU;
    std::uint32_t code = 0;
    std::size_t position = 0;
    const auto next_byte = [&]() -> std::uint32_t
    {
        if (position >= payload.size())
        {
            throw FormatError("truncated mixed payload");
        }
        return payload[position++];
    };

    for (int iteration = 0; iteration < 4; ++iteration)
    {
        code = (code << 8U) | next_byte();
    }
    for (std::size_t index = 0; index < expected_size; ++index)
    {
        const unsigned int flag = model->run_flag();
        unsigned int node = 1;
        for (unsigned int shift = 8U; shift-- > 0U;)
        {
            const CmModel::Slots slots = model->predict(node, flag);
            const std::uint32_t mid =
                low + static_cast<std::uint32_t>(
                          (static_cast<std::uint64_t>(high - low) * slots.scaled) >> 18U);
            const unsigned int bit = code <= mid ? 1U : 0U;
            if (bit != 0U)
            {
                high = mid;
            }
            else
            {
                low = mid + 1U;
            }
            while (((low ^ high) & 0xFF000000U) == 0U)
            {
                low <<= 8U;
                high = (high << 8U) | 0xFFU;
                code = (code << 8U) | next_byte();
            }
            model->update(slots, bit);
            node = node * 2U + bit;
        }
        const unsigned int byte = node & 255U;
        model->advance(byte);
        output.push_back(static_cast<Byte>(byte));
    }
    if (position != payload.size())
    {
        throw FormatError("mixed payload contains trailing bytes");
    }
    return output;
}

std::optional<Bytes> lzp_encode(const std::span<const Byte> input, const unsigned int hash_bits)
{
    if (input.size() <= lzp_min_match + lzp_context_size)
    {
        return std::nullopt;
    }
    std::array<std::size_t, 256> frequency{};
    for (const Byte value : input)
    {
        ++frequency[value];
    }
    Byte marker = 0;
    for (unsigned int value = 1; value < 256U; ++value)
    {
        if (frequency[value] < frequency[marker])
        {
            marker = static_cast<Byte>(value);
        }
    }

    Bytes output;
    output.reserve(input.size());
    output.push_back(marker);
    std::vector<std::uint32_t> table(std::size_t{1} << hash_bits, 0U);
    std::uint64_t context = 0;
    const std::size_t total = input.size();
    std::size_t index = 0;
    while (index < total)
    {
        std::uint32_t predicted = 0;
        if (index >= lzp_context_size)
        {
            const std::uint32_t slot = lzp_slot(context, hash_bits);
            predicted = table[slot];
            table[slot] = static_cast<std::uint32_t>(index) + 1U;
        }
        if (predicted != 0U)
        {
            const std::size_t source = predicted - 1U;
            std::size_t match = 0;
            while (index + match < total && input[source + match] == input[index + match])
            {
                ++match;
            }
            if (match >= lzp_min_match)
            {
                output.push_back(marker);
                lzp_put_length(output, match - lzp_min_match + 1U);
                for (std::size_t step = index; step < index + match; ++step)
                {
                    context = (context << 8U) | input[step];
                    if (step + 1U >= lzp_context_size && step + 1U < index + match)
                    {
                        table[lzp_slot(context, hash_bits)] = static_cast<std::uint32_t>(step) + 2U;
                    }
                }
                index += match;
                if (output.size() >= total)
                {
                    return std::nullopt;
                }
                continue;
            }
        }
        const Byte value = input[index];
        output.push_back(value);
        if (value == marker)
        {
            lzp_put_length(output, 0U);
        }
        context = (context << 8U) | value;
        ++index;
        if (output.size() >= total)
        {
            return std::nullopt;
        }
    }
    return output;
}

Bytes lzp_decode(const std::span<const Byte> input, const std::size_t expected_size,
                 const unsigned int hash_bits)
{
    if (input.empty())
    {
        throw FormatError("LZP stream is empty");
    }
    const Byte marker = input[0];
    Bytes output;
    output.reserve(expected_size);
    std::vector<std::uint32_t> table(std::size_t{1} << hash_bits, 0U);
    std::uint64_t context = 0;
    std::size_t position = 1;
    const auto take = [&]() -> Byte
    {
        if (position >= input.size())
        {
            throw FormatError("truncated LZP stream");
        }
        return input[position++];
    };

    while (output.size() < expected_size)
    {
        std::uint32_t predicted = 0;
        const std::size_t index = output.size();
        if (index >= lzp_context_size)
        {
            const std::uint32_t slot = lzp_slot(context, hash_bits);
            predicted = table[slot];
            table[slot] = static_cast<std::uint32_t>(index) + 1U;
        }
        const Byte value = take();
        if (value != marker)
        {
            output.push_back(value);
            context = (context << 8U) | value;
            continue;
        }
        std::uint64_t coded = 0;
        unsigned int shift = 0;
        while (true)
        {
            const Byte digit = take();
            coded |= static_cast<std::uint64_t>(digit & 127U) << shift;
            shift += 7U;
            if ((digit & 128U) == 0U)
            {
                break;
            }
            if (shift > 35U)
            {
                throw FormatError("malformed LZP length");
            }
        }
        if (coded == 0U)
        {
            output.push_back(marker);
            context = (context << 8U) | marker;
            continue;
        }
        const std::size_t match = static_cast<std::size_t>(coded) - 1U + lzp_min_match;
        if (predicted == 0U || match > expected_size - index)
        {
            throw FormatError("LZP match escapes the block");
        }
        const std::size_t source = predicted - 1U;
        for (std::size_t step = 0; step < match; ++step)
        {
            const Byte copied = output[source + step];
            output.push_back(copied);
            context = (context << 8U) | copied;
            const std::size_t written = output.size() - 1U;
            if (written + 1U >= lzp_context_size && written + 1U < index + match)
            {
                table[lzp_slot(context, hash_bits)] = static_cast<std::uint32_t>(written) + 2U;
            }
        }
    }
    if (position != input.size())
    {
        throw FormatError("LZP stream contains trailing bytes");
    }
    return output;
}

std::uint32_t adler32(const std::span<const Byte> input) noexcept
{
    constexpr std::uint32_t modulus = 65521U;
    // Largest run without 32-bit overflow before the deferred modulo.
    constexpr std::size_t max_deferred = 5552U;

    std::uint32_t first = 1U;
    std::uint32_t second = 0U;
    std::size_t index = 0;
    while (index < input.size())
    {
        const std::size_t chunk_end = std::min(input.size(), index + max_deferred);
        for (; index < chunk_end; ++index)
        {
            first += input[index];
            second += first;
        }
        first %= modulus;
        second %= modulus;
    }
    return (second << 16U) | first;
}

} // namespace mzip::detail
