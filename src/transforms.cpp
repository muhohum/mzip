#include "transforms.hpp"

#include <mzip/mzip.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <limits>
#include <memory>
#include <numeric>
#include <stdexcept>
#include <utility>
#include <vector>

#if defined(_MSC_VER) && (defined(_M_IX86) || defined(_M_X64))
#include <intrin.h> // _mm_prefetch
#endif

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

// ---- Context mixing over BWT output ----------------------------------------------------------
//
// Each bit of a byte is predicted by adaptive counters under several contexts, blended in the
// logistic domain by two small gated linear networks, refined by two secondary estimation
// stages, and coded with a carryless binary arithmetic coder. Everything is integer
// arithmetic, so the model evolves identically on every platform.

// The per-bit model steps must inline into the coder loops to keep their state in registers;
// optimisers decline that on their own at moderate settings, so ask explicitly where the
// compiler lets us. A pure hint: the output is the same.
#if defined(_MSC_VER)
#define MZIP_CM_INLINE __forceinline
#elif defined(__GNUC__) || defined(__clang__)
#define MZIP_CM_INLINE inline __attribute__((always_inline))
#else
#define MZIP_CM_INLINE inline
#endif

// Hints that a cache line will be needed soon; purely advisory and a no-op where unsupported.
// (Inlined by force: a call whose only effect is a prefetch may otherwise be dropped.)
MZIP_CM_INLINE void prefetch([[maybe_unused]] const void* address) noexcept
{
#if defined(__GNUC__) || defined(__clang__)
    __builtin_prefetch(address);
#elif defined(_MSC_VER) && (defined(_M_IX86) || defined(_M_X64))
    _mm_prefetch(static_cast<const char*>(address), _MM_HINT_T0);
#endif
}

// stretch(p) = ln(p / (1 - p)) with +-2047 spanning +-8 nats; squash is its inverse. 12-bit.
constexpr int logistic_limit = 2047;
constexpr std::array<int, 33> squash_knots{1,    2,    3,    6,    10,   16,   27,   45,   73,
                                           120,  194,  310,  488,  747,  1101, 1546, 2047, 2549,
                                           2994, 3348, 3607, 3785, 3901, 3975, 4024, 4050, 4068,
                                           4079, 4085, 4089, 4092, 4093, 4094};

[[nodiscard]] constexpr int squash(const int value) noexcept
{
    if (value > logistic_limit)
    {
        return 4095;
    }
    if (value < -logistic_limit)
    {
        return 0;
    }
    const int weight = value & 127;
    const std::size_t knot = static_cast<std::size_t>((value >> 7) + 16);
    return (squash_knots[knot] * (128 - weight) + squash_knots[knot + 1U] * weight + 64) >> 7;
}

[[nodiscard]] constexpr std::array<std::int16_t, 4096> make_stretch_table() noexcept
{
    std::array<std::int16_t, 4096> table{};
    std::size_t filled = 0;
    for (int value = -logistic_limit; value <= logistic_limit; ++value)
    {
        const auto probability = static_cast<std::size_t>(squash(value));
        for (std::size_t index = filled; index <= probability; ++index)
        {
            table[index] = static_cast<std::int16_t>(value);
        }
        filled = std::max(filled, probability + 1U);
    }
    for (std::size_t index = filled; index < table.size(); ++index)
    {
        table[index] = logistic_limit;
    }
    return table;
}

constexpr std::array<std::int16_t, 4096> stretch_table = make_stretch_table();

[[nodiscard]] inline int stretch(const int probability) noexcept
{
    return stretch_table[static_cast<std::size_t>(probability)];
}

// squash over the clamped domain as a single lookup, with the same values as squash().
[[nodiscard]] constexpr std::array<std::int16_t, 2 * logistic_limit + 1>
make_squash_table() noexcept
{
    std::array<std::int16_t, 2 * logistic_limit + 1> table{};
    for (int value = -logistic_limit; value <= logistic_limit; ++value)
    {
        table[static_cast<std::size_t>(value + logistic_limit)] =
            static_cast<std::int16_t>(squash(value));
    }
    return table;
}

constexpr std::array<std::int16_t, 2 * logistic_limit + 1> squash_table = make_squash_table();

// squash(value) for a value already within +-logistic_limit.
[[nodiscard]] inline int squash_in_domain(const int value) noexcept
{
    return squash_table[static_cast<std::size_t>(value + logistic_limit)];
}

// Two kinds of counter cell predict a bit under a context.
//
// A 32-bit adaptive cell holds a 22-bit probability over a 10-bit hit count. Its step shrinks
// with the count, so a context seen for the first time learns quickly and then settles down
// to 1/(2 * limit + offset). It pays for the sparsely visited tables.
//
// A 16-bit fixed cell holds a probability only and moves by a fixed shift. It suits the small,
// densely visited tables, where every context is seen often enough that a fixed rate matches
// an adaptive one, at half the memory and a cheaper update.
[[nodiscard]] constexpr std::array<std::int32_t, 1024> make_counter_rates() noexcept
{
    std::array<std::int32_t, 1024> rates{};
    for (std::size_t count = 0; count < rates.size(); ++count)
    {
        rates[count] = static_cast<std::int32_t>(16384U / (2U * count + 3U));
    }
    return rates;
}

constexpr std::array<std::int32_t, 1024> counter_rates = make_counter_rates();
constexpr std::uint32_t adaptive_initial = 1U << 31U;
constexpr std::uint16_t fixed_initial = 1U << 15U;

[[nodiscard]] inline int counter_probability(const std::uint32_t cell) noexcept
{
    return static_cast<int>(cell >> 20U);
}

inline void counter_update(std::uint32_t& cell, const unsigned int bit,
                           const std::uint32_t limit) noexcept
{
    const std::uint32_t count = cell & 1023U;
    const auto probability = static_cast<std::int64_t>(cell >> 10U);
    const std::int64_t target = static_cast<std::int64_t>(bit) << 22U;
    const std::int64_t updated =
        probability + (((target - probability) * counter_rates[count]) >> 14U);
    cell = (static_cast<std::uint32_t>(updated) << 10U) | std::min(count + 1U, limit);
}

[[nodiscard]] inline int counter_probability(const std::uint16_t cell) noexcept
{
    return static_cast<int>(cell >> 4U);
}

inline void counter_update(std::uint16_t& cell, const unsigned int bit,
                           const unsigned int shift) noexcept
{
    if (bit != 0U)
    {
        cell = static_cast<std::uint16_t>(cell + ((cell ^ 0xFFFFU) >> shift));
    }
    else
    {
        cell = static_cast<std::uint16_t>(cell - (cell >> shift));
    }
}

// Gated linear mixing in the logistic domain; one weight vector per selector value. The
// per-bit steps are static and act on the caller's copy of the inputs, so that copy can stay
// in registers; the loops are fold expressions so they unroll at any optimisation level.
template <std::size_t Inputs> class Mixer
{
public:
    using InputArray = std::array<int, Inputs>;

    Mixer(const std::size_t selectors, const InputArray& initial_weights)
        : weights_(selectors * Inputs)
    {
        for (std::size_t selector = 0; selector < selectors; ++selector)
        {
            std::copy(initial_weights.begin(), initial_weights.end(),
                      weights_.begin() + static_cast<std::ptrdiff_t>(selector * Inputs));
        }
    }

    // The weight vector of `selector`; those of the following selectors come right after it.
    [[nodiscard]] std::int32_t* weights(const std::size_t selector) noexcept
    {
        return &weights_[selector * Inputs];
    }

    // The mixed estimate in the logistic domain (+-2047).
    [[nodiscard]] static MZIP_CM_INLINE int mix(const std::int32_t* weights,
                                                const InputArray& inputs) noexcept
    {
        const std::int64_t dot = dot_product(weights, inputs, std::make_index_sequence<Inputs>{});
        return static_cast<int>(
            std::clamp<std::int64_t>(dot >> 16U, -logistic_limit, logistic_limit));
    }

    // `probability` is squash() of what mix() returned for these weights. `rate` is in
    // sixteenths: 16 moves a weight by (input * error) >> 13 per bit, and must keep every
    // input * error product within 32 bits (see the check next to mixer_rate).
    static MZIP_CM_INLINE void update(std::int32_t* weights, const InputArray& inputs,
                                      const int probability, const unsigned int bit,
                                      const int rate) noexcept
    {
        const int error = ((static_cast<int>(bit) << 12U) - probability) * rate;
        update_weights(weights, inputs, error, std::make_index_sequence<Inputs>{});
    }

private:
    static constexpr std::int32_t weight_limit = 1 << 22U;

    template <std::size_t... Index>
    [[nodiscard]] static MZIP_CM_INLINE std::int64_t
    dot_product(const std::int32_t* weights, const InputArray& inputs,
                std::index_sequence<Index...>) noexcept
    {
        return ((static_cast<std::int64_t>(inputs[Index]) * weights[Index]) + ...);
    }

    template <std::size_t... Index>
    static MZIP_CM_INLINE void update_weights(std::int32_t* weights, const InputArray& inputs,
                                              const int error,
                                              std::index_sequence<Index...>) noexcept
    {
        ((weights[Index] = std::clamp(weights[Index] + ((inputs[Index] * error) >> 17U),
                                      -weight_limit, weight_limit)),
         ...);
    }

    std::vector<std::int32_t> weights_;
};

// Secondary estimation: maps a logistic-domain estimate to a refined 16-bit probability
// under a context, by interpolating 33 adaptive cells spread over the domain.
class Apm
{
public:
    // One spare context at the end keeps prefetches past the last context in bounds.
    explicit Apm(const std::size_t contexts) : cells_((contexts + 1U) * cells_per_context)
    {
        for (std::size_t context = 0; context <= contexts; ++context)
        {
            for (std::size_t step = 0; step < cells_per_context; ++step)
            {
                cells_[context * cells_per_context + step] =
                    static_cast<std::uint16_t>(squash((static_cast<int>(step) - 16) * 128) * 16);
            }
        }
    }

    // The cells of `context`; those of the following contexts come right after them.
    [[nodiscard]] std::uint16_t* context(const std::size_t index) noexcept
    {
        return &cells_[index * cells_per_context];
    }

    // The pair of cells around `position` (the domain value + 2048) within `cells`.
    [[nodiscard]] static MZIP_CM_INLINE std::uint16_t* pair(std::uint16_t* cells,
                                                            const int position) noexcept
    {
        return cells + (position >> 7);
    }

    // Interpolates between a pair of cells; `fraction` is the position within the pair.
    [[nodiscard]] static MZIP_CM_INLINE int refine(const std::uint16_t* pair,
                                                   const int fraction) noexcept
    {
        return ((pair[0] << 7U) + (pair[1] - pair[0]) * fraction) >> 7;
    }

    // The value cells move towards after `bit`, shared by every stage.
    [[nodiscard]] static constexpr int target(const unsigned int bit) noexcept
    {
        return (static_cast<int>(bit) << 16U) + (static_cast<int>(bit) << rate) -
               static_cast<int>(bit) - static_cast<int>(bit);
    }

    static MZIP_CM_INLINE void update(std::uint16_t* pair, const int target) noexcept
    {
        pair[0] = static_cast<std::uint16_t>(pair[0] + ((target - pair[0]) >> rate));
        pair[1] = static_cast<std::uint16_t>(pair[1] + ((target - pair[1]) >> rate));
    }

    static constexpr std::size_t cells_per_context = 33;

private:
    static constexpr int rate = 7;

    std::vector<std::uint16_t> cells_;
};

// A context's cells, laid out so that the cells one nibble can touch sit together: the first
// nibble uses cells 1..15 of block 0 and the second, after high nibble h, cells 1..15 of block
// h + 1, each block being 16 cells. Every node still has a cell of its own, so predictions
// are unchanged; only their placement is. A context therefore spans 17 blocks, and the storage
// is aligned so that no block straddles a 64-byte cache line.
constexpr std::size_t context_cells = 17U * 16U;

template <typename Cell> class NibbleTable
{
public:
    NibbleTable(const std::size_t contexts, const Cell initial)
        : storage_(contexts * context_cells + line_bytes / sizeof(Cell), initial)
    {
        const auto address = reinterpret_cast<std::uintptr_t>(storage_.data());
        const std::uintptr_t padding = (line_bytes - address % line_bytes) % line_bytes;
        cells_ = storage_.data() + padding / sizeof(Cell);
    }

    NibbleTable(const NibbleTable&) = delete;
    NibbleTable& operator=(const NibbleTable&) = delete;

    [[nodiscard]] Cell* context(const std::size_t index) noexcept
    {
        return cells_ + index * context_cells;
    }

private:
    static constexpr std::size_t line_bytes = 64;

    std::vector<Cell> storage_;
    Cell* cells_ = nullptr;
};

// Where each node's cell sits within a context, in the layout described above.
[[nodiscard]] constexpr std::array<std::uint16_t, 256> make_node_slots() noexcept
{
    std::array<std::uint16_t, 256> slots{};
    for (unsigned int node = 1; node < 256U; ++node)
    {
        if (node < 16U)
        {
            slots[node] = static_cast<std::uint16_t>(node);
            continue;
        }
        // node = (16 + high) << depth | partial, with `depth` bits of the second nibble known.
        const unsigned int depth = static_cast<unsigned int>(std::bit_width(node)) - 5U;
        const unsigned int high = (node >> depth) & 15U;
        const unsigned int partial = node & ((1U << depth) - 1U);
        slots[node] = static_cast<std::uint16_t>(((high + 1U) << 4U) | (1U << depth) | partial);
    }
    return slots;
}

constexpr std::array<std::uint16_t, 256> node_slots = make_node_slots();

// A bit history is the last bits coded under one context, newest in the low bit, behind a
// leading 1 that marks its length. 1 is the empty history; seven bits is the most it keeps.
constexpr std::uint8_t history_empty = 1;

[[nodiscard]] constexpr std::uint8_t history_push(const std::uint8_t history,
                                                  const unsigned int bit) noexcept
{
    const unsigned int shifted = (static_cast<unsigned int>(history) << 1U) | bit;
    return static_cast<std::uint8_t>(shifted < 256U ? shifted : (shifted & 127U) | 128U);
}

// Indirect model: keeps a bit history per context and learns, across all contexts, what each
// history says about the next bit. Where a direct counter has to be dragged toward a change,
// the map already knows that "1110" tends to continue with 0 or that "0101" alternates.
class HistoryModel
{
public:
    explicit HistoryModel(const std::size_t contexts)
        : histories_(contexts, history_empty), map_(256U, adaptive_initial)
    {
    }

    // The histories kept under `context`, one per node slot.
    [[nodiscard]] std::uint8_t* context(const std::size_t index) noexcept
    {
        return histories_.context(index);
    }

    // The counter that says what `history` tends to continue with.
    [[nodiscard]] MZIP_CM_INLINE std::uint32_t* cell(const std::uint8_t history) noexcept
    {
        return &map_[history];
    }

private:
    NibbleTable<std::uint8_t> histories_;
    std::vector<std::uint32_t> map_;
};

// Model parameters. Fixed cells adapt by 1/2^shift; the count limit of an adaptive cell sets
// how slowly a seasoned context adapts; the mixer rate is in sixteenths.
constexpr unsigned int order0_shift = 2;
constexpr unsigned int sparse_shift = 4;
constexpr std::uint32_t history_limit = 30;
constexpr std::uint32_t slow_limit = 1023;
constexpr std::uint32_t order2_limit = 20;
constexpr std::uint32_t run_limit = 80;
constexpr int mixer_rate = 32;
// The mixer update multiplies a 12-bit input by error * rate in 32 bits.
static_assert(std::int64_t{logistic_limit} * 4095 * mixer_rate < (std::int64_t{1} << 31U),
              "mixer_rate is too large for the mixer's 32-bit update");
constexpr std::int32_t mixer_initial_weight = 13107;
constexpr unsigned int order2_bits_floor = 16;
constexpr unsigned int order2_bits_ceiling = 22;
constexpr std::size_t mixer_inputs = 6;
// Run lengths are exact below 4 and share a bucket per doubling above that.
constexpr unsigned int run_exact_bits = 2;
constexpr std::size_t run_buckets = 12;
constexpr std::size_t run_classes = 4;

// Order-2 contexts are hashed into a table sized from the block.
[[nodiscard]] unsigned int order2_bits(const std::size_t block_size) noexcept
{
    const auto width = static_cast<unsigned int>(std::bit_width(block_size | 1U));
    return std::clamp(width + 4U, order2_bits_floor, order2_bits_ceiling);
}

[[nodiscard]] constexpr unsigned int run_bucket_of(const unsigned int run) noexcept
{
    constexpr unsigned int exact = 1U << run_exact_bits;
    if (run < exact)
    {
        return run;
    }
    const auto doubling =
        exact - 1U + static_cast<unsigned int>(std::bit_width(run >> run_exact_bits));
    return std::min(doubling, static_cast<unsigned int>(run_buckets) - 1U);
}

// Coarse run-length class (1, 2-3, 4-15, 16 and longer) that selects the mixer weights.
[[nodiscard]] constexpr unsigned int run_class_of(const unsigned int run) noexcept
{
    const auto width = static_cast<unsigned int>(std::bit_width(run));
    return width <= 1U ? 0U : width == 2U ? 1U : width <= 4U ? 2U : 3U;
}

// BWT output is runs of a few alternating symbols. The contexts are the previous byte c1, the
// byte before it c2, the symbol d1 that preceded the current run (inside a run c2 == c1 adds
// nothing to c1, while d1 keeps a real second symbol of context), and the run length.
class MixedModel
{
public:
    using Inputs = Mixer<mixer_inputs>::InputArray;

    // What predict() finds for one bit and update() needs again, passed by value so that the
    // compiler may keep it in registers while the coder works.
    struct Slots
    {
        Inputs inputs;             // the stretched estimates the mixers blend
        std::int32_t* weights_run; // each mixer's selected weight vector
        std::int32_t* weights_class;
        std::uint32_t* history_cell; // the order-1 history's counter
        std::uint16_t* pair1;        // the cells each refinement interpolates
        std::uint16_t* pair2;
        std::size_t slot; // the node's cell within each context
        int mixed_run;    // each mixer's own 12-bit estimate, for its update
        int mixed_class;
        int probability; // the final 16-bit estimate
    };

    explicit MixedModel(const std::size_t block_size)
        : order0_(1U, fixed_initial), order1_(256U), order1_slow_(256U, adaptive_initial),
          order2_(std::size_t{1} << (order2_bits(block_size) - 8U), adaptive_initial),
          sparse_(256U, fixed_initial), runs_(run_buckets * 256U, adaptive_initial),
          order2_shift_(32U - (order2_bits(block_size) - 8U)),
          by_run_(run_classes * 256U, initial_weights()), by_class_(4U * 256U, initial_weights()),
          apm_order1_(256U * 256U), apm_run_(run_buckets * 256U)
    {
    }

    MixedModel(const MixedModel&) = delete;
    MixedModel& operator=(const MixedModel&) = delete;

    // Call once per byte before its first bit: fixes the rows every bit of the byte reads.
    void begin_byte() noexcept
    {
        const unsigned int run_bucket = run_bucket_of(run_);
        row1_ = order1_.context(c1_);
        row1_slow_ = order1_slow_.context(c1_);
        row2_ = order2_.context(order2_row(d1_, c1_));
        row_sparse_ = sparse_.context(c2_);
        row_run_ = runs_.context((run_bucket << 8U) | c1_);
        weights_run_ = by_run_.weights(run_class_of(run_) << 8U);
        weights_class_ = by_class_.weights((c1_ >> 6U) << 8U);
        apm_order1_row_ = apm_order1_.context(c1_ << 8U);
        apm_run_row_ = apm_run_.context(run_bucket << 8U);
    }

    // For an encoder, which knows what follows: warms the lines that `next`, the byte after
    // `current`, will need, a whole byte ahead of their use. The model itself is untouched.
    MZIP_CM_INLINE void prefetch_next(const unsigned int current, const unsigned int next) noexcept
    {
        const bool continues = current == c1_;
        const unsigned int run = continues ? run_ + 1U : 1U;
        const std::size_t high_block = static_cast<std::size_t>((next >> 4U) + 1U) << 4U;
        prefetch_row(order1_.context(current), high_block);
        prefetch_row(order1_slow_.context(current), high_block);
        prefetch_row(order2_.context(order2_row(continues ? d1_ : c1_, current)), high_block);
        prefetch_row(sparse_.context(c1_), high_block);
        prefetch_row(runs_.context((run_bucket_of(run) << 8U) | current), high_block);
        unsigned int node = 1;
        for (unsigned int shift = 8U; shift-- > 0U;)
        {
            prefetch_refinement(apm_order1_.context((current << 8U) | node));
            node = node * 2U + ((next >> shift) & 1U);
        }
    }

    // Estimates the next bit given the partial byte `node` (1..255).
    [[nodiscard]] MZIP_CM_INLINE Slots predict(const unsigned int node) noexcept
    {
        prefetch_ahead(node);
        return step<false>(node, 0U);
    }

    MZIP_CM_INLINE void update(const Slots& slots, const unsigned int bit) noexcept
    {
        counter_update(row0_[slots.slot], bit, order0_shift);
        counter_update(*slots.history_cell, bit, history_limit);
        row1_[slots.slot] = history_push(row1_[slots.slot], bit);
        counter_update(row1_slow_[slots.slot], bit, slow_limit);
        counter_update(row2_[slots.slot], bit, order2_limit);
        counter_update(row_sparse_[slots.slot], bit, sparse_shift);
        counter_update(row_run_[slots.slot], bit, run_limit);
        learn(slots, bit);
    }

    // Both steps at once for an encoder, which knows the bit before coding it: each counter
    // is then read and updated in one go. Returns the 16-bit probability of a one.
    [[nodiscard]] MZIP_CM_INLINE int predict_and_update(const unsigned int node,
                                                        const unsigned int bit) noexcept
    {
        const Slots slots = step<true>(node, bit);
        learn(slots, bit);
        return slots.probability;
    }

    void end_byte(const unsigned int byte) noexcept
    {
        if (byte == c1_)
        {
            ++run_;
        }
        else
        {
            run_ = 1U;
            d1_ = c1_;
        }
        c2_ = c1_;
        c1_ = byte;
    }

private:
    // The slow order-1 estimate starts weightless and only matters once the mixer learns it.
    [[nodiscard]] static Inputs initial_weights() noexcept
    {
        Inputs weights{};
        weights.fill(mixer_initial_weight);
        weights[5] = 0;
        return weights;
    }

    [[nodiscard]] std::size_t order2_row(const unsigned int d1,
                                         const unsigned int c1) const noexcept
    {
        const std::uint32_t pair = (d1 << 8U) | c1;
        return static_cast<std::size_t>((pair * 0x9E3779B1U) >> order2_shift_);
    }

    // Warms a context's first-nibble block and the second-nibble block `high_block` starts.
    template <typename Cell>
    static MZIP_CM_INLINE void prefetch_row(const Cell* row, const std::size_t high_block) noexcept
    {
        prefetch(row);
        prefetch(row + high_block);
    }

    // Warms one refinement context (66 bytes, so up to two lines) and the one after it.
    static MZIP_CM_INLINE void prefetch_refinement(const std::uint16_t* cells) noexcept
    {
        prefetch(cells);
        prefetch(cells + 32);
        prefetch(cells + 64);
    }

    // For a decoder, which cannot look a byte ahead: warms what the coming bits may touch, as
    // far as the bits decoded so far narrow it down. The next bit's two candidate refinement
    // contexts are adjacent (on the last bit this lands on a neighbouring context instead,
    // harmlessly and within the table); after three bits, the second nibble's block in the two
    // largest tables is one of four; and on the last bit, the next byte is one of two, so its
    // rows in those tables and its first refinement context can be warmed too.
    MZIP_CM_INLINE void prefetch_ahead(const unsigned int node) noexcept
    {
        prefetch_refinement(apm_order1_.context((c1_ << 8U) | (node * 2U)));
        if (node >= 4U && node < 8U)
        {
            const std::size_t block = static_cast<std::size_t>((node & 3U) * 4U + 1U) << 4U;
            for (std::size_t candidate = 0; candidate < 4U; ++candidate)
            {
                prefetch(row2_ + block + candidate * 16U);
                prefetch(row_run_ + block + candidate * 16U);
            }
        }
        else if (node >= 128U)
        {
            const unsigned int first = (node * 2U) & 255U;
            for (unsigned int next = first; next < first + 2U; ++next)
            {
                const bool continues = next == c1_;
                prefetch(order2_.context(order2_row(continues ? d1_ : c1_, next)));
                prefetch(runs_.context((run_bucket_of(continues ? run_ + 1U : 1U) << 8U) | next));
                prefetch(apm_order1_.context((next << 8U) | 1U));
            }
        }
    }

    template <bool Update>
    [[nodiscard]] MZIP_CM_INLINE Slots step(const unsigned int node,
                                            const unsigned int bit) noexcept
    {
        Slots slots{};
        slots.slot = node_slots[node];
        std::uint8_t& history = row1_[slots.slot];
        slots.history_cell = order1_.cell(history);
        slots.inputs[0] = sample<Update>(row0_[slots.slot], bit, order0_shift);
        slots.inputs[1] = sample<Update>(*slots.history_cell, bit, history_limit);
        slots.inputs[2] = sample<Update>(row2_[slots.slot], bit, order2_limit);
        slots.inputs[3] = sample<Update>(row_sparse_[slots.slot], bit, sparse_shift);
        slots.inputs[4] = sample<Update>(row_run_[slots.slot], bit, run_limit);
        slots.inputs[5] = sample<Update>(row1_slow_[slots.slot], bit, slow_limit);
        if constexpr (Update)
        {
            history = history_push(history, bit);
        }

        slots.weights_run = weights_run_ + node * mixer_inputs;
        int domain = Mixer<mixer_inputs>::mix(slots.weights_run, slots.inputs);
        slots.mixed_run = squash_in_domain(domain);
        slots.weights_class = weights_class_ + node * mixer_inputs;
        const int by_class = Mixer<mixer_inputs>::mix(slots.weights_class, slots.inputs);
        slots.mixed_class = squash_in_domain(by_class);
        domain = (domain + by_class + 1) >> 1U;
        const int mixed = squash_in_domain(domain);

        const int position = domain + 2048;
        const int fraction = position & 127;
        slots.pair1 = Apm::pair(apm_order1_row_ + node * Apm::cells_per_context, position);
        slots.pair2 = Apm::pair(apm_run_row_ + node * Apm::cells_per_context, position);
        const int refined1 = Apm::refine(slots.pair1, fraction);
        const int refined2 = Apm::refine(slots.pair2, fraction);
        const int probability = ((mixed << 4U) + refined1 + 2 * refined2 + 2) >> 2U;
        slots.probability = std::clamp(probability, 1, 65535);
        return slots;
    }

    // A counter's stretched probability, updating the counter on the way when the bit is known.
    template <bool Update, typename Cell>
    [[nodiscard]] static MZIP_CM_INLINE int
    sample(Cell& cell, [[maybe_unused]] const unsigned int bit,
           [[maybe_unused]] const unsigned int rate) noexcept
    {
        const int input = stretch(counter_probability(cell));
        if constexpr (Update)
        {
            counter_update(cell, bit, rate);
        }
        return input;
    }

    // The mixers and both refinement stages learn from the coded bit.
    static MZIP_CM_INLINE void learn(const Slots& slots, const unsigned int bit) noexcept
    {
        Mixer<mixer_inputs>::update(slots.weights_run, slots.inputs, slots.mixed_run, bit,
                                    mixer_rate);
        Mixer<mixer_inputs>::update(slots.weights_class, slots.inputs, slots.mixed_class, bit,
                                    mixer_rate);
        const int target = Apm::target(bit);
        Apm::update(slots.pair1, target);
        Apm::update(slots.pair2, target);
    }

    NibbleTable<std::uint16_t> order0_;
    HistoryModel order1_;
    NibbleTable<std::uint32_t> order1_slow_;
    NibbleTable<std::uint32_t> order2_;
    NibbleTable<std::uint16_t> sparse_;
    NibbleTable<std::uint32_t> runs_;
    unsigned int order2_shift_;
    Mixer<mixer_inputs> by_run_;
    Mixer<mixer_inputs> by_class_;
    Apm apm_order1_;
    Apm apm_run_;

    std::uint16_t* row0_ = order0_.context(0);
    std::uint8_t* row1_ = nullptr;
    std::uint32_t* row1_slow_ = nullptr;
    std::uint32_t* row2_ = nullptr;
    std::uint16_t* row_sparse_ = nullptr;
    std::uint32_t* row_run_ = nullptr;
    std::int32_t* weights_run_ = nullptr;
    std::int32_t* weights_class_ = nullptr;
    std::uint16_t* apm_order1_row_ = nullptr;
    std::uint16_t* apm_run_row_ = nullptr;
    unsigned int c1_ = 0;
    unsigned int c2_ = 0;
    unsigned int d1_ = 0;
    unsigned int run_ = 0;
};

// ---- Version 2 model, kept so that existing archives stay readable --------------------------

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
struct LegacyCmModel
{
    std::vector<std::uint16_t> order0;
    std::vector<std::uint16_t> order1;
    std::vector<std::uint16_t> apm;
    unsigned int previous = 0;
    unsigned int before_previous = 0;
    unsigned int run = 0;

    LegacyCmModel()
        : order0(256U, cm_half), order1(256U * 256U, cm_half), apm(512U * cm_apm_columns)
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
    MixedModel model(input.size());
    std::uint32_t low = 0;
    std::uint32_t high = 0xFFFFFFFFU;

    for (std::size_t index = 0; index < input.size(); ++index)
    {
        const Byte value = input[index];
        model.begin_byte();
        if (index + 1U < input.size())
        {
            model.prefetch_next(value, input[index + 1U]);
        }
        unsigned int node = 1;
        for (unsigned int shift = 8U; shift-- > 0U;)
        {
            const unsigned int bit = (value >> shift) & 1U;
            const auto probability =
                static_cast<std::uint32_t>(model.predict_and_update(node, bit));
            const std::uint32_t mid =
                low + static_cast<std::uint32_t>(
                          (static_cast<std::uint64_t>(high - low) * probability) >> 16U);
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
            node = node * 2U + bit;
        }
        model.end_byte(node & 255U);
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
    MixedModel model(expected_size);
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
        model.begin_byte();
        unsigned int node = 1;
        for (unsigned int shift = 8U; shift-- > 0U;)
        {
            const MixedModel::Slots slots = model.predict(node);
            const auto probability = static_cast<std::uint32_t>(slots.probability);
            const std::uint32_t mid =
                low + static_cast<std::uint32_t>(
                          (static_cast<std::uint64_t>(high - low) * probability) >> 16U);
            // The bit is only known after the comparison, so a real branch lets the processor
            // run ahead on its prediction instead of waiting for the whole chain.
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
            model.update(slots, bit);
            node = node * 2U + bit;
        }
        const unsigned int byte = node & 255U;
        model.end_byte(byte);
        output.push_back(static_cast<Byte>(byte));
    }
    if (position != payload.size())
    {
        throw FormatError("mixed payload contains trailing bytes");
    }
    return output;
}

Bytes cm_decode_v2(const std::span<const Byte> payload, const std::size_t expected_size)
{
    Bytes output;
    output.reserve(expected_size);
    const auto model = std::make_unique<LegacyCmModel>();
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
            const LegacyCmModel::Slots slots = model->predict(node, flag);
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

// ---- x86 branch-target filter ---------------------------------------------------------------

namespace
{

// Opcode byte plus a little-endian rel32 operand.
constexpr std::size_t x86_branch_size = 5;

[[nodiscard]] bool x86_branch_opcode(const Byte value) noexcept
{
    return value == 0xE8U || value == 0xE9U;
}

// Converted operands are 25-bit signed values: the top byte is 0x00 or 0xFF before the
// transform, and the stored form sign-extends bit 24 so the same test holds afterwards.
[[nodiscard]] bool x86_operand_fits(const Byte top) noexcept
{
    return top == 0x00U || top == 0xFFU;
}

[[nodiscard]] std::uint32_t x86_read_operand(const std::span<const Byte> data,
                                             const std::size_t at) noexcept
{
    return std::uint32_t{data[at]} | (std::uint32_t{data[at + 1U]} << 8U) |
           (std::uint32_t{data[at + 2U]} << 16U) | (std::uint32_t{data[at + 3U]} << 24U);
}

void x86_write_operand(const std::span<Byte> data, const std::size_t at,
                       const std::uint32_t value) noexcept
{
    data[at] = static_cast<Byte>(value & 0xFFU);
    data[at + 1U] = static_cast<Byte>((value >> 8U) & 0xFFU);
    data[at + 2U] = static_cast<Byte>((value >> 16U) & 0xFFU);
    data[at + 3U] = (value & 0x0100'0000U) != 0U ? Byte{0xFFU} : Byte{0x00U};
}

enum class X86Pass
{
    count,
    encode,
    decode
};

// Every pass walks the same positions: opcode bytes are never rewritten and the operand
// bytes after an opcode are skipped whether or not they were converted. Returns how many
// converted operands name a target inside the block.
template <X86Pass Pass, typename Span> std::size_t x86_filter(const Span data) noexcept
{
    std::size_t targets = 0;
    std::size_t index = 0;
    while (index + x86_branch_size <= data.size())
    {
        if (!x86_branch_opcode(data[index]))
        {
            ++index;
            continue;
        }
        const std::size_t operand = index + 1U;
        if (x86_operand_fits(data[operand + 3U]))
        {
            const auto next = static_cast<std::uint32_t>(index + x86_branch_size);
            const std::uint32_t value = x86_read_operand(data, operand);
            const std::uint32_t converted = Pass == X86Pass::decode ? value - next : value + next;
            if constexpr (Pass != X86Pass::count)
            {
                x86_write_operand(data, operand, converted);
            }
            if (converted < data.size())
            {
                ++targets;
            }
        }
        index += x86_branch_size;
    }
    return targets;
}

} // namespace

std::size_t x86_branch_targets(const std::span<const Byte> data) noexcept
{
    return x86_filter<X86Pass::count>(data);
}

std::size_t x86_filter_encode(const std::span<Byte> data) noexcept
{
    return x86_filter<X86Pass::encode>(data);
}

void x86_filter_decode(const std::span<Byte> data) noexcept
{
    static_cast<void>(x86_filter<X86Pass::decode>(data));
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
