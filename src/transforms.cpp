#include "transforms.hpp"

#include <mzip/mzip.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <cstring>
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

// SA-IS (Nong, Zhang, Chan 2009) over the text with a virtual sentinel after its end, which
// sorts below every symbol and is not stored: the result is the standard suffix array, where
// a suffix that is a prefix of another sorts first. A suffix is S-type when it is smaller than
// the suffix after it and L-type otherwise; an LMS position is an S-type one after an L-type
// one. Everything happens inside the suffix array itself plus one bucket array per level: the
// reduced string of the recursion lives in the top of the array and its suffix array in the
// bottom, so the sort takes 4 bytes per symbol and the bucket array on top.
//
// Every entry carries a flag in its top bit that tells whether the suffix before it is S-type,
// so the inducing loops need no type array: the L pass induces from unflagged entries, the S
// pass from flagged ones, and the flag of a newly placed suffix follows from two text symbols.
// Empty slots hold 0, which suffix 0 may share, as it has no suffix before it to induce.
constexpr std::uint32_t induce_flag = 1U << 31U;
// How far ahead the loops warm the text (and other arrays) they are about to read at random.
constexpr std::uint32_t sort_prefetch_distance = 64;

template <typename Char> class SuffixSorter
{
public:
    // Sorts the suffixes of `text` into `suffix_array` (same size, below 2^31 entries); every
    // symbol must be below `alphabet`.
    static void sort(const std::span<const Char> text, const std::span<std::uint32_t> suffix_array,
                     const std::uint32_t alphabet)
    {
        const auto size = static_cast<std::uint32_t>(text.size());
        if (size <= 1U)
        {
            if (size == 1U)
            {
                suffix_array[0] = 0U;
            }
            return;
        }
        SuffixSorter sorter(text.data(), suffix_array.data(), size, alphabet);
        sorter.run();
    }

private:
    SuffixSorter(const Char* text, std::uint32_t* suffix_array, const std::uint32_t size,
                 const std::uint32_t alphabet)
        : text_(text), sa_(suffix_array), size_(size), alphabet_(alphabet), buckets_(alphabet + 1U)
    {
    }

    // buckets_[c] becomes the first slot of bucket c (`tails` false) or one past its last.
    void find_buckets(const bool tails)
    {
        std::fill(buckets_.begin(), buckets_.end(), 0U);
        const std::uint32_t shift = tails ? 0U : 1U;
        for (std::uint32_t index = 0; index < size_; ++index)
        {
            ++buckets_[static_cast<std::size_t>(text_[index]) + shift];
        }
        std::uint32_t sum = 0;
        for (std::uint32_t& bucket : buckets_)
        {
            sum += bucket;
            bucket = sum;
        }
    }

    // Calls visit(p) for every LMS position p in 1..size-1, from right to left. The last
    // position is L-type, being above the sentinel.
    template <typename Visit> void for_each_lms(Visit&& visit) const
    {
        bool next_is_s = false;
        for (std::uint32_t index = size_ - 1U; index-- > 0U;)
        {
            const bool is_s = text_[index] < text_[index + 1U] ||
                              (text_[index] == text_[index + 1U] && next_is_s);
            if (!is_s && next_is_s)
            {
                visit(index + 1U);
            }
            next_is_s = is_s;
        }
    }

    // `suffix` and its flag, for a suffix of the given type.
    [[nodiscard]] std::uint32_t flagged_l(const std::uint32_t suffix) const noexcept
    {
        return suffix > 0U && text_[suffix - 1U] < text_[suffix] ? suffix | induce_flag : suffix;
    }

    [[nodiscard]] std::uint32_t flagged_s(const std::uint32_t suffix) const noexcept
    {
        return suffix > 0U && text_[suffix - 1U] <= text_[suffix] ? suffix | induce_flag : suffix;
    }

    void prefetch_text(const std::uint32_t entry) const noexcept
    {
        const std::uint32_t suffix = entry & ~induce_flag;
        prefetch(text_ + (suffix >= 2U ? suffix - 2U : 0U));
    }

    // L-suffixes left to right from the bucket heads, starting with the one the sentinel
    // induces. With `Prune` (stage 1) every entry that has done its work is emptied, so that
    // after both passes only the LMS suffixes are left.
    template <bool Prune> void induce_l()
    {
        find_buckets(false);
        std::uint32_t* const sa = sa_;
        sa[buckets_[text_[size_ - 1U]]++] = flagged_l(size_ - 1U);
        for (std::uint32_t index = 0; index < size_; ++index)
        {
            if (index + sort_prefetch_distance < size_)
            {
                prefetch_text(sa[index + sort_prefetch_distance]);
            }
            const std::uint32_t entry = sa[index];
            if (entry == 0U || (entry & induce_flag) != 0U)
            {
                continue;
            }
            const std::uint32_t suffix = entry - 1U;
            sa[buckets_[text_[suffix]]++] = flagged_l(suffix);
            if constexpr (Prune)
            {
                sa[index] = 0U;
            }
        }
    }

    // S-suffixes right to left from the bucket tails; clears every flag on the way.
    template <bool Prune> void induce_s()
    {
        find_buckets(true);
        std::uint32_t* const sa = sa_;
        for (std::uint32_t index = size_; index-- > 0U;)
        {
            if (index >= sort_prefetch_distance)
            {
                prefetch_text(sa[index - sort_prefetch_distance]);
            }
            const std::uint32_t entry = sa[index];
            if ((entry & induce_flag) == 0U)
            {
                continue;
            }
            const std::uint32_t suffix = (entry & ~induce_flag) - 1U;
            sa[--buckets_[text_[suffix]]] = flagged_s(suffix);
            sa[index] = Prune ? 0U : entry & ~induce_flag;
        }
    }

    // Gives every sorted LMS substring in sa[0, lms_count) a name, equal names for equal
    // substrings, and leaves the names in text order in the top lms_count slots. Returns the
    // number of distinct names.
    [[nodiscard]] std::uint32_t name_lms_substrings(const std::uint32_t lms_count)
    {
        std::uint32_t* const sa = sa_;
        // LMS positions are at least two apart, so slot lms_count + p/2 is free for each one;
        // it first holds the length of p's substring (through the next LMS symbol), then its
        // name plus one. The substring reaching the sentinel is unlike any other.
        std::fill(sa + lms_count, sa + size_, 0U);
        std::uint32_t next = size_;
        std::uint32_t last = 0;
        for_each_lms(
            [&](const std::uint32_t position)
            {
                sa[lms_count + position / 2U] = next - position + 1U;
                last = next == size_ ? position : last;
                next = position;
            });

        std::uint32_t names = 0;
        std::uint32_t previous = 0;
        std::uint32_t previous_length = 0;
        for (std::uint32_t rank = 0; rank < lms_count; ++rank)
        {
            if (rank + sort_prefetch_distance < lms_count)
            {
                const std::uint32_t ahead = sa[rank + sort_prefetch_distance];
                prefetch(sa + lms_count + ahead / 2U);
                prefetch(text_ + ahead);
            }
            const std::uint32_t position = sa[rank];
            const std::uint32_t length = sa[lms_count + position / 2U];
            // Equal symbols over an equal length imply equal types, as both end on an LMS one.
            const bool same =
                rank > 0U && length == previous_length && position != last && previous != last &&
                std::equal(text_ + position, text_ + position + length, text_ + previous);
            names += same ? 0U : 1U;
            sa[lms_count + position / 2U] = names;
            previous = position;
            previous_length = length;
        }

        std::uint32_t target = size_;
        for (std::uint32_t index = size_; index-- > lms_count;)
        {
            if (sa[index] != 0U)
            {
                sa[--target] = sa[index] - 1U;
            }
        }
        return names;
    }

    void run()
    {
        std::uint32_t* const sa = sa_;

        // Stage 1: one round of induced sorting from the LMS positions sorts the LMS substrings.
        std::fill(sa, sa + size_, 0U);
        find_buckets(true);
        std::uint32_t lms_count = 0;
        for_each_lms(
            [&](const std::uint32_t position)
            {
                sa[--buckets_[text_[position]]] = position;
                ++lms_count;
            });
        induce_l<true>();
        induce_s<true>();
        std::uint32_t gathered = 0;
        for (std::uint32_t index = 0; index < size_ && gathered < lms_count; ++index)
        {
            if (sa[index] != 0U)
            {
                sa[gathered++] = sa[index];
            }
        }

        // Stage 2: order the LMS suffixes by sorting the string of their names, recursing
        // while names repeat. The bucket array is released for the duration.
        const std::uint32_t names = lms_count > 0U ? name_lms_substrings(lms_count) : 0U;
        std::uint32_t* const reduced = sa + size_ - lms_count;
        if (names < lms_count)
        {
            buckets_ = std::vector<std::uint32_t>();
            SuffixSorter<std::uint32_t>::sort(std::span<const std::uint32_t>(reduced, lms_count),
                                              std::span<std::uint32_t>(sa, lms_count), names);
            buckets_ = std::vector<std::uint32_t>(alphabet_ + 1U);
        }
        else
        {
            for (std::uint32_t index = 0; index < lms_count; ++index)
            {
                sa[reduced[index]] = index;
            }
        }

        // Stage 3: turn the ranks back into positions, put the sorted LMS suffixes at their
        // bucket tails, and induce the complete order from them.
        std::uint32_t target = lms_count;
        for_each_lms([&](const std::uint32_t position) { reduced[--target] = position; });
        for (std::uint32_t index = 0; index < lms_count; ++index)
        {
            if (index + sort_prefetch_distance < lms_count)
            {
                prefetch(reduced + sa[index + sort_prefetch_distance]);
            }
            sa[index] = reduced[sa[index]];
        }
        std::fill(sa + lms_count, sa + size_, 0U);
        find_buckets(true);
        for (std::uint32_t index = lms_count; index-- > 0U;)
        {
            if (index >= sort_prefetch_distance)
            {
                prefetch(text_ + sa[index - sort_prefetch_distance]);
            }
            const std::uint32_t position = sa[index];
            sa[index] = 0U;
            sa[--buckets_[text_[position]]] = position;
        }
        induce_l<false>();
        induce_s<false>();
    }

    const Char* text_;
    std::uint32_t* sa_;
    std::uint32_t size_;
    std::uint32_t alphabet_;
    std::vector<std::uint32_t> buckets_;
};

} // namespace

BwtResult bwt_encode(const std::span<const Byte> input)
{
    const std::size_t size = input.size();
    if (size == 0U)
    {
        return {};
    }
    if (size >= induce_flag)
    {
        throw std::invalid_argument("BWT block is too large");
    }

    std::vector<std::uint32_t> suffix_array(size);
    SuffixSorter<Byte>::sort(input, suffix_array, alphabet_size);

    // Row 0 is the sentinel's own suffix, preceded by the last byte; the row of suffix 0,
    // preceded by the sentinel, is omitted and kept as the primary index.
    BwtResult result;
    result.data.resize(size);
    result.data[0] = input[size - 1U];
    std::size_t output_index = 1;
    for (std::size_t rank = 0; rank < size; ++rank)
    {
        if (rank + sort_prefetch_distance < size)
        {
            const std::uint32_t ahead = suffix_array[rank + sort_prefetch_distance];
            prefetch(input.data() + (ahead > 0U ? ahead - 1U : 0U));
        }
        const std::uint32_t suffix = suffix_array[rank];
        if (suffix == 0U)
        {
            result.primary_index = static_cast<std::uint32_t>(rank + 1U);
            continue;
        }
        result.data[output_index++] = input[suffix - 1U];
    }
    return result;
}

Bytes bwt_decode(Bytes block, const std::uint32_t primary_index)
{
    const std::size_t size = block.size();
    if (size == 0U)
    {
        if (primary_index != 0U)
        {
            throw FormatError("invalid BWT index for an empty block");
        }
        return block;
    }
    if (primary_index == 0U || static_cast<std::size_t>(primary_index) > size)
    {
        throw FormatError("BWT primary index is outside the block");
    }
    if (size >= std::numeric_limits<std::uint32_t>::max())
    {
        throw FormatError("BWT block is too large");
    }

    // Row r of the sorted rotations is the sentinel's suffix for r = 0 and otherwise sits at
    // slot r - 1 of the first column, whose symbols run in order; the stored last column
    // leaves out row primary_index, the sentinel itself. For each stored row, `backward`
    // holds the slot of the row of the suffix one position earlier (the LF mapping), whose
    // first symbol is this row's last one; `forward` is the inverse map, from a slot to the
    // row of the suffix one position later, as a slot. The text is then rebuilt from both
    // ends at once, back from the sentinel and forth from suffix 0: two independent chains
    // of reads that the processor overlaps, where one chain would wait on every cache miss.
    std::array<std::uint32_t, alphabet_size + 1U> starts{};
    for (const Byte value : block)
    {
        ++starts[value + 1U];
    }
    for (std::size_t symbol = 0; symbol < alphabet_size; ++symbol)
    {
        starts[symbol + 1U] += starts[symbol];
    }
    std::array<std::uint32_t, alphabet_size> next_slot{};
    std::copy_n(starts.begin(), alphabet_size, next_slot.begin());
    std::vector<std::uint32_t> forward(size);
    std::vector<std::uint32_t> backward(size);
    for (std::uint32_t stored = 0; stored < size; ++stored)
    {
        const std::uint32_t row = stored < primary_index ? stored : stored + 1U;
        const std::uint32_t slot = next_slot[block[stored]]++;
        // Row 0 has no slot; it only ends the forward chain, which never follows it.
        forward[slot] = row - (row != 0U ? 1U : 0U);
        backward[stored] = slot;
    }

    // The symbol of a slot is the bucket it falls in: a coarse table gets close, a short
    // scan finishes.
    const auto width = static_cast<unsigned int>(std::bit_width(size));
    const unsigned int shift = width > 16U ? width - 16U : 0U;
    std::vector<Byte> coarse((size >> shift) + 1U);
    for (std::size_t index = 0, symbol = 0; index < coarse.size(); ++index)
    {
        while (symbol + 1U < alphabet_size && starts[symbol + 1U] <= (index << shift))
        {
            ++symbol;
        }
        coarse[index] = static_cast<Byte>(symbol);
    }
    const auto symbol_of = [&](const std::uint32_t slot)
    {
        std::size_t symbol = coarse[slot >> shift];
        while (starts[symbol + 1U] <= slot)
        {
            ++symbol;
        }
        return static_cast<Byte>(symbol);
    };

    Bytes& output = block;
    std::uint32_t ahead = primary_index - 1U; // slot of suffix 0's row
    std::uint32_t behind = 0;                 // stored row of the sentinel's suffix
    const std::size_t half = size / 2U;
    for (std::size_t index = 0; index < half; ++index)
    {
        const std::uint32_t slot = backward[behind];
        output[index] = symbol_of(ahead);
        output[size - 1U - index] = symbol_of(slot);
        ahead = forward[ahead];
        behind = slot + 1U < primary_index ? slot + 1U : slot;
    }
    if (size % 2U != 0U)
    {
        output[half] = symbol_of(ahead);
    }
    return block;
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

// The repeat decision keeps longer histories, of up to eleven decisions, in 16 bits.
constexpr unsigned int repeat_history_bits = 12;

[[nodiscard]] constexpr std::uint16_t repeat_history_push(const std::uint16_t history,
                                                          const unsigned int bit) noexcept
{
    constexpr unsigned int top = 1U << (repeat_history_bits - 1U);
    const unsigned int shifted = (static_cast<unsigned int>(history) << 1U) | bit;
    return static_cast<std::uint16_t>(shifted < 2U * top ? shifted : (shifted & (top - 1U)) | top);
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
constexpr unsigned int order0_shift = 3;
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
constexpr std::size_t mixer_inputs = 7;
// The repeat decision's own counters; the recency input follows up to eight earlier symbols.
constexpr std::uint32_t repeat_limit = 30;
constexpr std::uint32_t repeat_length_limit = 80;
constexpr std::uint32_t recency_limit = 60;
constexpr std::size_t recency_ranks = 8;
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
// symbol d1 that preceded the current run (inside a run the byte before c1 is c1 again, while
// d1 keeps a real second symbol of context), and the run length. Every byte starts with a
// binary repeat decision (same as c1 or not); only a byte that differs walks the byte tree,
// and there the branch that would spell c1 again is skipped.
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
          by_run_(run_classes * 256U, initial_weights()), by_class_(8U * 256U, initial_weights()),
          apm_order1_(256U * 256U), apm_run_(run_buckets * 256U),
          repeat_map_(std::size_t{1} << repeat_history_bits, adaptive_initial),
          repeat_recent_(256U, adaptive_initial),
          repeat_lengths_(run_buckets * run_buckets, adaptive_initial),
          repeat_ranks_(run_buckets * 16U * 16U, adaptive_initial),
          repeat_by_length_(run_buckets * run_buckets, initial_weights()),
          repeat_by_symbol_(256U, initial_weights()),
          recency_(run_buckets * recency_ranks * 8U, adaptive_initial)
    {
        repeat_histories_.fill(history_empty);
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
        row_sparse_ = sparse_.context(d1_);
        row_run_ = runs_.context((run_bucket << 8U) | c1_);
        weights_run_ = by_run_.weights(run_class_of(run_) << 8U);
        weights_class_ = by_class_.weights((c1_ >> 6U) << 8U);
        apm_order1_row_ = apm_order1_.context(c1_ << 8U);
        apm_run_row_ = apm_run_.context(run_bucket << 8U);
        const std::size_t lengths = run_bucket * run_buckets + last_run_[c1_];
        repeat_length_cell_ = &repeat_lengths_[lengths];
        repeat_recent_cell_ = &repeat_recent_[recent_ & 255U];
        repeat_rank_cell_ =
            &repeat_ranks_[((run_bucket << 4U) | run_rank_) * 16U + (recent_ & 15U)];
        repeat_weights_length_ = repeat_by_length_.weights(lengths);
        repeat_weights_symbol_ = repeat_by_symbol_.weights(c1_);
        recency_row_ = &recency_[run_bucket * recency_ranks * 8U];
    }

    // The repeat decision comes first in every byte: 1 if the byte repeats the previous one,
    // in which case no further bit is coded. Estimates it without touching the model.
    [[nodiscard]] MZIP_CM_INLINE Slots predict_repeat() noexcept
    {
        return repeat_step<false>(0U);
    }

    MZIP_CM_INLINE void update_repeat(const Slots& slots, const unsigned int bit) noexcept
    {
        counter_update(*repeat_recent_cell_, bit, repeat_limit);
        counter_update(*slots.history_cell, bit, repeat_limit);
        repeat_histories_[c1_] = repeat_history_push(repeat_histories_[c1_], bit);
        counter_update(row2_[0], bit, order2_limit);
        counter_update(*repeat_length_cell_, bit, repeat_length_limit);
        counter_update(row_run_[0], bit, run_limit);
        counter_update(row1_slow_[0], bit, slow_limit);
        counter_update(*repeat_rank_cell_, bit, repeat_length_limit);
        learn(slots, bit);
        if (bit != 0U)
        {
            learn_repeat();
        }
    }

    // Both steps at once for an encoder. Returns the 16-bit probability of a repeat.
    [[nodiscard]] MZIP_CM_INLINE int predict_and_update_repeat(const unsigned int bit) noexcept
    {
        const Slots slots = repeat_step<true>(bit);
        learn(slots, bit);
        if (bit != 0U)
        {
            learn_repeat();
        }
        return slots.probability;
    }

    // The byte a repeat decision of 1 stands for.
    [[nodiscard]] unsigned int previous() const noexcept
    {
        return c1_;
    }

    // After a repeat decision of 0, the node whose last bit is forced: following it would
    // lead back to the previous byte, so the coders skip it.
    [[nodiscard]] unsigned int excluded_node() const noexcept
    {
        return (c1_ >> 1U) | 128U;
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
        prefetch_row(sparse_.context(continues ? d1_ : c1_), high_block);
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
        if (recency_cell_ != nullptr)
        {
            counter_update(*recency_cell_, bit == recency_expected_ ? 1U : 0U, recency_limit);
        }
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
        const unsigned int repeat = byte == c1_ ? 1U : 0U;
        recent_ = (recent_ << 1U) | repeat;
        if (repeat != 0U)
        {
            ++run_;
        }
        else
        {
            last_run_[c1_] = static_cast<std::uint8_t>(run_bucket_of(run_));
            run_ = 1U;
            d1_ = c1_;
            // Move the new run's symbol to the front of the recent symbols, noting its rank.
            unsigned int position = 0;
            while (position < recent_symbols_.size() - 1U && recent_symbols_[position] != byte)
            {
                ++position;
            }
            run_rank_ = position;
            for (; position > 0U; --position)
            {
                recent_symbols_[position] = recent_symbols_[position - 1U];
            }
            recent_symbols_[0] = byte;
        }
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

    // A repeat leaves the byte tree untouched, so its two most general tables still learn
    // that the previous byte occurred again.
    void learn_repeat() noexcept
    {
        unsigned int node = 1;
        for (unsigned int shift = 8U; shift-- > 0U;)
        {
            const unsigned int bit = (c1_ >> shift) & 1U;
            counter_update(row0_[node_slots[node]], bit, order0_shift);
            counter_update(row1_slow_[node_slots[node]], bit, slow_limit);
            node = node * 2U + bit;
        }
    }

    // The repeat decision has contexts of its own: the last eight decisions, the decision
    // history under c1, the run length against the length of c1's previous run, and the
    // recency rank of the run's symbol; and it borrows the spare node-0 cell of the order-2,
    // run and slow order-1 rows (nodes start at 1) and the node-0 refinement contexts.
    template <bool Update>
    [[nodiscard]] MZIP_CM_INLINE Slots repeat_step(const unsigned int bit) noexcept
    {
        Slots slots{};
        std::uint16_t& history = repeat_histories_[c1_];
        slots.history_cell = &repeat_map_[history];
        slots.inputs[0] = sample<Update>(*repeat_recent_cell_, bit, repeat_limit);
        slots.inputs[1] = sample<Update>(*slots.history_cell, bit, repeat_limit);
        slots.inputs[2] = sample<Update>(row2_[0], bit, order2_limit);
        slots.inputs[3] = sample<Update>(*repeat_length_cell_, bit, repeat_length_limit);
        slots.inputs[4] = sample<Update>(row_run_[0], bit, run_limit);
        slots.inputs[5] = sample<Update>(row1_slow_[0], bit, slow_limit);
        slots.inputs[6] = sample<Update>(*repeat_rank_cell_, bit, repeat_length_limit);
        if constexpr (Update)
        {
            history = repeat_history_push(history, bit);
        }
        slots.weights_run = repeat_weights_length_;
        slots.weights_class = repeat_weights_symbol_;
        blend(slots, apm_order1_row_, apm_run_row_);
        return slots;
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
        slots.inputs[6] = recency<Update>(node, bit);
        if constexpr (Update)
        {
            history = history_push(history, bit);
        }

        // While the partial byte still spells the previous byte, the excluded symbol is one
        // of the candidates; the class mixer keeps separate weights for that case.
        const auto depth = static_cast<unsigned int>(std::bit_width(node)) - 1U;
        const std::size_t on_path = ((c1_ | 256U) >> (8U - depth)) == node ? 4U << 8U : 0U;
        slots.weights_run = weights_run_ + node * mixer_inputs;
        slots.weights_class = weights_class_ + (on_path + node) * mixer_inputs;
        blend(slots, apm_order1_row_ + node * Apm::cells_per_context,
              apm_run_row_ + node * Apm::cells_per_context);
        return slots;
    }

    // Recency input: when the partial byte `node` still matches one of the symbols that
    // started the last few runs (most recent first, the previous byte aside), a counter per
    // (run bucket, rank, depth) says how often such a symbol's next bit is followed; the
    // input leans toward that bit. Zero when no candidate matches.
    template <bool Update>
    [[nodiscard]] MZIP_CM_INLINE int recency(const unsigned int node,
                                             [[maybe_unused]] const unsigned int bit) noexcept
    {
        const auto depth = static_cast<unsigned int>(std::bit_width(node)) - 1U;
        const unsigned int shift = 8U - depth;
        recency_cell_ = nullptr;
        for (std::size_t rank = 0; rank < recency_ranks; ++rank)
        {
            const unsigned int symbol = recent_symbols_[rank + 1U];
            if (((symbol | 256U) >> shift) == node)
            {
                std::uint32_t& cell = recency_row_[rank * 8U + depth];
                const unsigned int expected = (symbol >> (shift - 1U)) & 1U;
                const int input = stretch(counter_probability(cell));
                recency_cell_ = &cell;
                recency_expected_ = expected;
                if constexpr (Update)
                {
                    counter_update(cell, bit == expected ? 1U : 0U, recency_limit);
                }
                return expected != 0U ? input : -input;
            }
        }
        return 0;
    }

    // Blends the inputs and refines the result with the given refinement contexts.
    MZIP_CM_INLINE void blend(Slots& slots, std::uint16_t* refine1, std::uint16_t* refine2) noexcept
    {
        int domain = Mixer<mixer_inputs>::mix(slots.weights_run, slots.inputs);
        slots.mixed_run = squash_in_domain(domain);
        const int by_class = Mixer<mixer_inputs>::mix(slots.weights_class, slots.inputs);
        slots.mixed_class = squash_in_domain(by_class);
        domain = (domain + by_class + 1) >> 1U;
        const int mixed = squash_in_domain(domain);

        const int position = domain + 2048;
        const int fraction = position & 127;
        slots.pair1 = Apm::pair(refine1, position);
        slots.pair2 = Apm::pair(refine2, position);
        const int refined1 = Apm::refine(slots.pair1, fraction);
        const int refined2 = Apm::refine(slots.pair2, fraction);
        const int probability = ((mixed << 4U) + refined1 + 2 * refined2 + 2) >> 2U;
        slots.probability = std::clamp(probability, 1, 65535);
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
    unsigned int d1_ = 0;
    unsigned int run_ = 0;

    // The repeat decision's own contexts.
    std::array<std::uint16_t, 256> repeat_histories_;
    std::vector<std::uint32_t> repeat_map_;
    std::vector<std::uint32_t> repeat_recent_;
    std::vector<std::uint32_t> repeat_lengths_;
    std::vector<std::uint32_t> repeat_ranks_;
    Mixer<mixer_inputs> repeat_by_length_;
    Mixer<mixer_inputs> repeat_by_symbol_;
    std::uint32_t* repeat_recent_cell_ = nullptr;
    std::uint32_t* repeat_length_cell_ = nullptr;
    std::uint32_t* repeat_rank_cell_ = nullptr;
    std::int32_t* repeat_weights_length_ = nullptr;
    std::int32_t* repeat_weights_symbol_ = nullptr;
    std::array<std::uint8_t, 256> last_run_{}; // each symbol's last run bucket
    unsigned int recent_ = 0;                  // the latest repeat decisions, newest lowest
    unsigned int run_rank_ = 0;                // the current run symbol's recency rank

    // The symbols that started the latest runs, most recent first (the previous byte's own).
    std::array<unsigned int, recency_ranks + 2U> recent_symbols_{};
    std::vector<std::uint32_t> recency_;
    std::uint32_t* recency_row_ = nullptr;
    std::uint32_t* recency_cell_ = nullptr;
    unsigned int recency_expected_ = 0;
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

constexpr std::size_t lzp_context_size = 8;

[[nodiscard]] std::uint32_t lzp_slot(const std::uint64_t context,
                                     const unsigned int hash_bits) noexcept
{
    return static_cast<std::uint32_t>((context * 0x9E3779B97F4A7C15ULL) >> (64U - hash_bits));
}

[[nodiscard]] std::size_t lzp_needed(const LzpRule& rule, const std::size_t distance) noexcept
{
    return distance >= rule.far_distance ? rule.far_match : rule.min_match;
}

// The context before `position`: the eight bytes ending there, the last in the low byte.
[[nodiscard]] std::uint64_t lzp_context_at(const Byte* bytes, const std::size_t position) noexcept
{
    std::uint64_t context = 0;
    for (std::size_t index = position - lzp_context_size; index < position; ++index)
    {
        context = (context << 8U) | bytes[index];
    }
    return context;
}

// How far ahead the LZP loops warm the table slot of a coming position.
constexpr std::size_t lzp_prefetch_distance = 16;

// Length of the common prefix of input[source..] and input[index..], source < index, compared
// a word at a time where the byte order allows.
[[nodiscard]] std::size_t lzp_match_length(const std::span<const Byte> input,
                                           const std::size_t source, const std::size_t index)
{
    const std::size_t limit = input.size() - index;
    std::size_t match = 0;
    if constexpr (std::endian::native == std::endian::little ||
                  std::endian::native == std::endian::big)
    {
        while (match + sizeof(std::uint64_t) <= limit)
        {
            std::uint64_t left = 0;
            std::uint64_t right = 0;
            std::memcpy(&left, input.data() + source + match, sizeof(left));
            std::memcpy(&right, input.data() + index + match, sizeof(right));
            if (left != right)
            {
                const std::uint64_t difference = left ^ right;
                const int equal_bits = std::endian::native == std::endian::little
                                           ? std::countr_zero(difference)
                                           : std::countl_zero(difference);
                return match + static_cast<std::size_t>(equal_bits) / 8U;
            }
            match += sizeof(std::uint64_t);
        }
    }
    while (match < limit && input[source + match] == input[index + match])
    {
        ++match;
    }
    return match;
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
    const auto encode = [&](const unsigned int bit, const int probability)
    {
        const std::uint32_t mid =
            low + static_cast<std::uint32_t>((static_cast<std::uint64_t>(high - low) *
                                              static_cast<std::uint32_t>(probability)) >>
                                             16U);
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
    };

    for (std::size_t index = 0; index < input.size(); ++index)
    {
        const Byte value = input[index];
        model.begin_byte();
        if (index + 1U < input.size())
        {
            model.prefetch_next(value, input[index + 1U]);
        }
        const unsigned int repeat = value == model.previous() ? 1U : 0U;
        encode(repeat, model.predict_and_update_repeat(repeat));
        if (repeat == 0U)
        {
            unsigned int node = 1;
            for (unsigned int shift = 8U; shift-- > 0U;)
            {
                const unsigned int bit = (value >> shift) & 1U;
                if (node != model.excluded_node())
                {
                    encode(bit, model.predict_and_update(node, bit));
                }
                node = node * 2U + bit;
            }
        }
        model.end_byte(value);
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
    const auto decode = [&](const int probability16) -> unsigned int
    {
        const auto probability = static_cast<std::uint32_t>(probability16);
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
        return bit;
    };

    for (int iteration = 0; iteration < 4; ++iteration)
    {
        code = (code << 8U) | next_byte();
    }
    for (std::size_t index = 0; index < expected_size; ++index)
    {
        model.begin_byte();
        unsigned int byte = model.previous();
        const MixedModel::Slots repeat_slots = model.predict_repeat();
        const unsigned int repeat = decode(repeat_slots.probability);
        model.update_repeat(repeat_slots, repeat);
        if (repeat == 0U)
        {
            unsigned int node = 1;
            for (unsigned int shift = 8U; shift-- > 0U;)
            {
                unsigned int bit = (byte & 1U) ^ 1U;
                if (node != model.excluded_node())
                {
                    const MixedModel::Slots slots = model.predict(node);
                    bit = decode(slots.probability);
                    model.update(slots, bit);
                }
                node = node * 2U + bit;
            }
            byte = node & 255U;
        }
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

std::optional<Bytes> lzp_encode(const std::span<const Byte> input, const unsigned int hash_bits,
                                const LzpRule& rule)
{
    if (input.size() <= std::min(rule.min_match, rule.far_match) + lzp_context_size)
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
    // Every position reads or writes a slot at random, so the loops warm the slot of the
    // position a little ahead, whose context is already known.
    const auto prefetch_slot = [&](const std::size_t position)
    {
        if (position >= lzp_context_size && position < total)
        {
            prefetch(&table[lzp_slot(lzp_context_at(input.data(), position), hash_bits)]);
        }
    };
    while (index < total)
    {
        prefetch_slot(index + lzp_prefetch_distance);
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
            const std::size_t match = lzp_match_length(input, source, index);
            const std::size_t needed = lzp_needed(rule, index - source);
            if (match >= needed)
            {
                output.push_back(marker);
                lzp_put_length(output, match - needed + 1U);
                for (std::size_t step = index; step < index + match; ++step)
                {
                    prefetch_slot(step + lzp_prefetch_distance);
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
                 const unsigned int hash_bits, const LzpRule& rule)
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
        if (predicted == 0U)
        {
            throw FormatError("LZP match escapes the block");
        }
        const std::size_t source = predicted - 1U;
        const std::size_t match =
            static_cast<std::size_t>(coded) - 1U + lzp_needed(rule, index - source);
        if (match > expected_size - index)
        {
            throw FormatError("LZP match escapes the block");
        }
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

// ---- Record filter --------------------------------------------------------------------------

namespace
{

// Record lengths from 3 bytes up to this; 1 and 2 are neighbouring bytes and samples, which the
// sort already sees as context.
constexpr std::size_t record_min_stride = 3;
constexpr std::size_t record_max_stride = 2048;
// The search needs at least this many records in the block.
constexpr std::size_t record_min_records = 64;
// Equal bytes are counted in this many windows of up to this many bytes, spread over the block;
// a window is at most 1/256 of the block, which keeps the search cheap for small blocks.
constexpr std::size_t record_sample_windows = 8;
constexpr std::size_t record_sample_window = 8192;
constexpr std::size_t record_window_divisor = 256;
// A stride must stand out of its neighbours by 1/256 of the sampled bytes.
constexpr std::uint64_t record_prominence_divisor = 256;
// The plan is fitted on about this many bytes of whole records from the middle of the block and
// must cut the estimated cost by at least 1/32 to be worth a candidate. Before that, byte units
// over the first 64 KiB of those records must promise at least 1/64.
constexpr std::size_t record_plan_sample = std::size_t{1} << 20U;
constexpr std::uint64_t record_gain_divisor = 32;
constexpr std::size_t record_screen_sample = std::size_t{1} << 16U;
constexpr std::uint64_t record_screen_divisor = 64;
constexpr std::array<std::uint32_t, 4> record_units{1U, 2U, 4U, 8U};

// The record length is the stride at which bytes repeat most clearly: the one whose count of
// equal bytes stands out most above the strides next to it. Comparing with the neighbours, not
// with short strides, ignores the slowly falling counts of smooth data and 16-bit samples, so
// the row length of an image is found even where neighbouring samples are as alike as rows.
[[nodiscard]] std::size_t record_stride(const std::span<const Byte> data)
{
    const std::size_t max_stride = std::min(record_max_stride, data.size() / record_min_records);
    if (max_stride < record_min_stride)
    {
        return 0;
    }
    const std::size_t lookback = max_stride + 2U;
    const std::size_t window = std::min(record_sample_window, data.size() / record_window_divisor);
    const std::size_t spread = data.size() - lookback - window;
    std::vector<std::uint32_t> equal(lookback + 1U, 0U);
    for (std::size_t index = 0; index < record_sample_windows; ++index)
    {
        const std::size_t start =
            lookback +
            static_cast<std::size_t>(std::uint64_t{spread} * index / (record_sample_windows - 1U));
        const Byte* const here = data.data() + start;
        for (std::size_t stride = 1; stride <= lookback; ++stride)
        {
            // Counted in 8-bit lanes flushed every 255 bytes, which vectorises well.
            const Byte* const back = here - stride;
            for (std::size_t offset = 0; offset < window;)
            {
                const std::size_t end = std::min(window, offset + 255U);
                Byte lane = 0;
                for (; offset < end; ++offset)
                {
                    lane = static_cast<Byte>(lane + (here[offset] == back[offset] ? 1U : 0U));
                }
                equal[stride] += lane;
            }
        }
    }

    std::size_t best = 0;
    std::uint32_t best_prominence = 0;
    for (std::size_t stride = record_min_stride; stride <= max_stride; ++stride)
    {
        const std::uint32_t neighbours = std::max(
            {equal[stride - 2U], equal[stride - 1U], equal[stride + 1U], equal[stride + 2U]});
        if (equal[stride] > neighbours && equal[stride] - neighbours > best_prominence)
        {
            best = stride;
            best_prominence = equal[stride] - neighbours;
        }
    }
    const std::uint64_t samples = std::uint64_t{window} * record_sample_windows;
    return best_prominence * record_prominence_divisor >= samples ? best : 0;
}

// log2(1 + k/256) in 1/256 bits, by repeated squaring in 30-bit fixed point.
[[nodiscard]] constexpr std::array<std::uint16_t, 256> make_log2_fractions() noexcept
{
    constexpr unsigned int point = 30;
    std::array<std::uint16_t, 256> table{};
    for (std::uint64_t index = 0; index < table.size(); ++index)
    {
        std::uint64_t value = (256U + index) << (point - 8U);
        unsigned int fraction = 0;
        for (unsigned int bit = 0; bit < 8U; ++bit)
        {
            value = (value * value) >> point;
            fraction <<= 1U;
            if (value >= (std::uint64_t{2} << point))
            {
                fraction |= 1U;
                value >>= 1U;
            }
        }
        table[index] = static_cast<std::uint16_t>(fraction);
    }
    return table;
}

constexpr std::array<std::uint16_t, 256> log2_fractions = make_log2_fractions();

// log2(value) in 1/256 bits for value >= 1.
[[nodiscard]] std::uint64_t log2_fixed(const std::uint32_t value) noexcept
{
    const auto exponent = static_cast<unsigned int>(std::bit_width(value)) - 1U;
    const std::uint32_t mantissa =
        exponent >= 8U ? value >> (exponent - 8U) : value << (8U - exponent);
    return std::uint64_t{exponent} * 256U + log2_fractions[mantissa - 256U];
}

// What an adaptive order-1 model (counts starting at one half) pays for a column of bytes, each
// under the byte before it, in 1/256 bits. It stands in for the mixer when choosing which units
// to delta-code: cheap, and it agrees with the mixer on which columns get easier. The sequential
// cost of a context seen t times with symbol counts c is the sum of log2(2i + 256) over i < t
// minus the sums of log2(2j + 1) over j < c, so the column is counted first and then priced
// from running sums of both, up to `longest` bytes per column.
class ColumnCost
{
public:
    explicit ColumnCost(const std::size_t longest)
        : counts_(alphabet_size * alphabet_size, 0U), totals_(alphabet_size, 0U),
          symbol_sums_(longest + 1U, 0U), context_sums_(longest + 1U, 0U)
    {
        for (std::uint32_t index = 0; index < longest; ++index)
        {
            symbol_sums_[index + 1U] = symbol_sums_[index] + log2_fixed(2U * index + 1U);
            context_sums_[index + 1U] = context_sums_[index] + log2_fixed(2U * index + 256U);
        }
    }

    [[nodiscard]] std::uint64_t operator()(const std::span<const Byte> values,
                                           const std::span<const Byte> contexts)
    {
        for (std::size_t index = 0; index < values.size(); ++index)
        {
            ++counts_[std::size_t{contexts[index]} * alphabet_size + values[index]];
            ++totals_[contexts[index]];
        }
        std::uint64_t context_cost = 0;
        std::uint64_t symbol_credit = 0;
        for (std::size_t index = 0; index < values.size(); ++index)
        {
            std::uint32_t& count =
                counts_[std::size_t{contexts[index]} * alphabet_size + values[index]];
            symbol_credit += symbol_sums_[count];
            count = 0U;
            std::uint32_t& total = totals_[contexts[index]];
            context_cost += context_sums_[total];
            total = 0U;
        }
        return context_cost - symbol_credit;
    }

private:
    std::vector<std::uint32_t> counts_;
    std::vector<std::uint32_t> totals_;
    std::vector<std::uint64_t> symbol_sums_;
    std::vector<std::uint64_t> context_sums_;
};

// Gathers one column of `region` (whole records, the first only a reference): every byte from
// the second record on and the byte before it.
void gather_column(const std::span<const Byte> region, const std::size_t stride,
                   const std::size_t column, Bytes& values, Bytes& contexts)
{
    for (std::size_t record = 1; record <= values.size(); ++record)
    {
        const std::size_t at = record * stride + column;
        values[record - 1U] = region[at];
        contexts[record - 1U] = region[at - 1U];
    }
}

struct PlanCost
{
    std::uint64_t unfiltered = 0;
    std::uint64_t filtered = 0;
    Bytes mask;
};

// A record is cut into units of `unit` bytes; the last one is shorter when the unit does not
// divide the stride.
[[nodiscard]] std::size_t record_unit_count(const std::size_t stride,
                                            const std::size_t unit) noexcept
{
    return (stride + unit - 1U) / unit;
}

[[nodiscard]] std::size_t record_unit_width(const std::size_t stride, const std::size_t unit,
                                            const std::size_t column) noexcept
{
    return std::min(unit, stride - column * unit);
}

// Decides unit by unit, left to right, whether delta coding makes a unit cheaper than the
// costs in `kept` (per column, the bytes left as they are). The first byte of a coded unit is
// costed under the byte before it as it will be coded.
[[nodiscard]] PlanCost record_plan_cost(const std::span<const Byte> region,
                                        const std::size_t stride, const std::size_t unit,
                                        const std::span<const std::uint64_t> kept,
                                        ColumnCost& column_cost)
{
    const std::size_t records = region.size() / stride;
    const std::size_t units = record_unit_count(stride, unit);
    Bytes coded(region.begin(), region.end());
    Bytes differences(records * unit);
    Bytes borrows(records);
    Bytes values(records - 1U);
    Bytes contexts(records - 1U);
    PlanCost result;
    result.mask.assign((units + 7U) / 8U, Byte{0U});
    for (std::size_t column = 0; column < units; ++column)
    {
        const std::size_t width = record_unit_width(stride, unit, column);
        std::uint64_t unfiltered = 0;
        std::uint64_t delta = 0;
        for (std::size_t byte = 0; byte < width; ++byte)
        {
            const std::size_t offset = column * unit + byte;
            unfiltered += kept[offset];
            for (std::size_t record = 1; record < records; ++record)
            {
                const std::size_t at = record * stride + offset;
                const unsigned int difference = unsigned{region[at]} - region[at - stride] -
                                                (byte == 0U ? 0U : borrows[record]);
                borrows[record] = static_cast<Byte>((difference >> 8U) & 1U);
                differences[record * unit + byte] = static_cast<Byte>(difference);
                values[record - 1U] = static_cast<Byte>(difference);
                contexts[record - 1U] =
                    byte == 0U ? coded[at - 1U] : differences[record * unit + byte - 1U];
            }
            delta += column_cost(values, contexts);
        }
        result.unfiltered += unfiltered;
        if (delta >= unfiltered)
        {
            result.filtered += unfiltered;
            continue;
        }
        result.filtered += delta;
        result.mask[column / 8U] =
            static_cast<Byte>(result.mask[column / 8U] | (1U << (column % 8U)));
        for (std::size_t record = 1; record < records; ++record)
        {
            for (std::size_t byte = 0; byte < width; ++byte)
            {
                coded[record * stride + column * unit + byte] = differences[record * unit + byte];
            }
        }
    }
    return result;
}

// The cheapest plan over `region` (whole records) among the unit widths in `units`, if it cuts
// the estimated cost by at least 1/`gain_divisor`.
[[nodiscard]] std::optional<RecordPlan> fit_record_plan(const std::span<const Byte> region,
                                                        const std::size_t stride,
                                                        const std::span<const std::uint32_t> units,
                                                        const std::uint64_t gain_divisor)
{
    const std::size_t records = region.size() / stride;
    ColumnCost column_cost(records);
    std::vector<std::uint64_t> kept(stride);
    Bytes values(records - 1U);
    Bytes contexts(records - 1U);
    for (std::size_t column = 0; column < stride; ++column)
    {
        gather_column(region, stride, column, values, contexts);
        kept[column] = column_cost(values, contexts);
    }

    std::optional<RecordPlan> best;
    std::uint64_t best_filtered = 0;
    for (const std::uint32_t unit : units)
    {
        if (unit > stride)
        {
            continue;
        }
        PlanCost cost = record_plan_cost(region, stride, unit, kept, column_cost);
        if (cost.filtered * gain_divisor <= cost.unfiltered * (gain_divisor - 1U) &&
            (!best || cost.filtered < best_filtered))
        {
            best = RecordPlan{static_cast<std::uint32_t>(stride), unit, std::move(cost.mask)};
            best_filtered = cost.filtered;
        }
    }
    return best;
}

[[nodiscard]] bool record_unit_selected(const RecordPlan& plan, const std::size_t column) noexcept
{
    return ((unsigned{plan.mask[column / 8U]} >> (column % 8U)) & 1U) != 0U;
}

} // namespace

std::optional<RecordPlan> record_plan(const std::span<const Byte> data)
{
    const std::size_t stride = record_stride(data);
    if (stride == 0U)
    {
        return std::nullopt;
    }
    const std::size_t records = data.size() / stride;
    const std::size_t sampled = std::min(records, record_plan_sample / stride + 1U);
    const std::span<const Byte> region =
        data.subspan((records - sampled) / 2U * stride, sampled * stride);
    // Most blocks with a record length still gain nothing from differences (code, text, packed
    // structures); byte units over the first records of the sample turn them away cheaply.
    const std::size_t screened = std::min(sampled, record_screen_sample / stride + 1U);
    if (!fit_record_plan(region.first(screened * stride), stride, std::span(record_units).first(1U),
                         record_screen_divisor))
    {
        return std::nullopt;
    }
    return fit_record_plan(region, stride, record_units, record_gain_divisor);
}

// Backwards, so that every difference is taken against the unfiltered record before it.
void record_filter_encode(const std::span<Byte> data, const RecordPlan& plan) noexcept
{
    const std::size_t stride = plan.stride;
    const std::size_t unit = plan.unit;
    for (std::size_t record = (data.size() + stride - 1U) / stride; record-- > 1U;)
    {
        for (std::size_t column = 0; column < record_unit_count(stride, unit); ++column)
        {
            const std::size_t start = record * stride + column * unit;
            const std::size_t end = start + record_unit_width(stride, unit, column);
            if (end > data.size())
            {
                break;
            }
            if (!record_unit_selected(plan, column))
            {
                continue;
            }
            unsigned int borrow = 0;
            for (std::size_t at = start; at < end; ++at)
            {
                const unsigned int difference = unsigned{data[at]} - data[at - stride] - borrow;
                data[at] = static_cast<Byte>(difference);
                borrow = (difference >> 8U) & 1U;
            }
        }
    }
}

// Forwards, so that every sum is taken against the restored record before it.
void record_filter_decode(const std::span<Byte> data, const RecordPlan& plan) noexcept
{
    const std::size_t stride = plan.stride;
    const std::size_t unit = plan.unit;
    for (std::size_t base = stride; base < data.size(); base += stride)
    {
        for (std::size_t column = 0; column < record_unit_count(stride, unit); ++column)
        {
            const std::size_t start = base + column * unit;
            const std::size_t end = start + record_unit_width(stride, unit, column);
            if (end > data.size())
            {
                break;
            }
            if (!record_unit_selected(plan, column))
            {
                continue;
            }
            unsigned int carry = 0;
            for (std::size_t at = start; at < end; ++at)
            {
                const unsigned int sum = unsigned{data[at]} + data[at - stride] + carry;
                data[at] = static_cast<Byte>(sum);
                carry = sum >> 8U;
            }
        }
    }
}

Bytes record_plan_write(const RecordPlan& plan)
{
    Bytes bytes{static_cast<Byte>(plan.unit), static_cast<Byte>(plan.stride & 0xFFU),
                static_cast<Byte>((plan.stride >> 8U) & 0xFFU)};
    bytes.insert(bytes.end(), plan.mask.begin(), plan.mask.end());
    return bytes;
}

RecordPlan record_plan_read(const std::span<const Byte> payload, const std::size_t block_size,
                            std::size_t& consumed)
{
    if (payload.size() < 3U)
    {
        throw FormatError("truncated record filter plan");
    }
    RecordPlan plan;
    plan.unit = payload[0];
    plan.stride = std::uint32_t{payload[1]} | (std::uint32_t{payload[2]} << 8U);
    if (std::find(record_units.begin(), record_units.end(), plan.unit) == record_units.end() ||
        plan.stride < plan.unit || plan.stride >= block_size)
    {
        throw FormatError("invalid record filter stride");
    }
    const std::size_t units = record_unit_count(plan.stride, plan.unit);
    const std::size_t mask_size = (units + 7U) / 8U;
    if (payload.size() - 3U < mask_size)
    {
        throw FormatError("truncated record filter plan");
    }
    plan.mask.assign(payload.begin() + 3,
                     payload.begin() + 3 + static_cast<std::ptrdiff_t>(mask_size));
    // Bits past the last unit must be clear, and at least one unit must be filtered.
    const unsigned int spare_bits = static_cast<unsigned int>(mask_size * 8U - units);
    if ((unsigned{plan.mask.back()} >> (8U - spare_bits)) != 0U ||
        std::all_of(plan.mask.begin(), plan.mask.end(),
                    [](const Byte value) { return value == 0U; }))
    {
        throw FormatError("invalid record filter mask");
    }
    consumed = 3U + mask_size;
    return plan;
}

// ---- Content boundaries ---------------------------------------------------------------------

namespace
{

// A chunk is cut in two where its halves are estimated to code best apart, then each half
// again, until no cut pays. The estimate is the order-1 entropy: n_c log n_c - sum n_cs log n_cs
// over the contexts c (the previous byte) and bytes s that follow them. It only depends on
// the counts, so it moves by four table lookups as the cut sweeps across the chunk. A cut
// must save a 32nd of the whole and a little more, which leaves uniform data in one piece.
constexpr std::size_t segment_min_size = std::size_t{128} * 1024U;
constexpr unsigned int segment_max_depth = 8;
constexpr unsigned int segment_gain_shift = 5;
constexpr std::int64_t segment_gain_floor = std::int64_t{12} * 1024 * 65536; // 12 Kibit
// Order-1 statistics miss what the sort gains most from: long repeats. When the bytes after a
// cut keep finding their last occurrence before it, as in a tar of similar small files, the
// pieces lose more than their statistics gain, so such a cut is not made. Repeats are found
// like LZP: the 8 bytes before a position predict where they last occurred, and a match of at
// least 32 bytes counts; a 16th of the bytes after the cut is too many. A match is followed
// for at most 64 KiB: a long run of one value, such as zero padding, would otherwise count in
// full although it codes almost for free on either side.
constexpr std::size_t repeat_min_match = 32;
constexpr std::size_t repeat_max_match = std::size_t{64} * 1024U;
constexpr std::size_t repeat_context_size = 8;
constexpr unsigned int repeat_hash_bits = 20;
constexpr unsigned int repeat_share_shift = 4;

// log2(1 + i / 4096) in 1/65536 bits, by repeated squaring.
[[nodiscard]] constexpr std::array<std::int32_t, 4096> make_entropy_log2_fractions() noexcept
{
    std::array<std::int32_t, 4096> table{};
    for (std::size_t index = 0; index < table.size(); ++index)
    {
        std::uint64_t value = (std::uint64_t{4096U} + index) << 18U; // [1, 2), 30 fraction bits
        std::int32_t fraction = 0;
        for (int bit = 15; bit >= 0; --bit)
        {
            value = (value * value) >> 30U;
            if (value >= (std::uint64_t{2} << 30U))
            {
                value >>= 1U;
                fraction |= std::int32_t{1} << bit;
            }
        }
        table[index] = fraction;
    }
    return table;
}

constexpr std::array<std::int32_t, 4096> entropy_log2_fractions = make_entropy_log2_fractions();

// log2(value) in 1/65536 bits, value >= 1, from its top 13 significant bits.
[[nodiscard]] constexpr std::int64_t entropy_log2(const std::uint64_t value) noexcept
{
    const auto width = static_cast<unsigned int>(std::bit_width(value));
    const std::uint64_t top = width > 13U ? value >> (width - 13U) : value << (13U - width);
    return std::int64_t{width - 1U} * 65536 +
           entropy_log2_fractions[static_cast<std::size_t>(top - 4096U)];
}

// (n + 1) log2(n + 1) - n log2(n) in 1/65536 bits: what one more count adds to n log2 n. Exact
// below 4096; above, log2(n) + log2(e) is within a thousandth of a bit.
constexpr std::size_t entropy_step_exact = 4096;
constexpr std::int64_t log2_e = 94548;

[[nodiscard]] constexpr std::array<std::int32_t, entropy_step_exact> make_entropy_steps() noexcept
{
    std::array<std::int32_t, entropy_step_exact> table{};
    for (std::size_t count = 1; count < table.size(); ++count)
    {
        table[count] = static_cast<std::int32_t>(
            static_cast<std::int64_t>(count + 1U) * entropy_log2(count + 1U) -
            static_cast<std::int64_t>(count) * entropy_log2(count));
    }
    return table;
}

constexpr std::array<std::int32_t, entropy_step_exact> entropy_steps = make_entropy_steps();

[[nodiscard]] std::int64_t entropy_step(const std::uint32_t count) noexcept
{
    return count < entropy_step_exact ? entropy_steps[count] : entropy_log2(count) + log2_e;
}

// Order-1 counts for one side of a cut.
class SegmentCounts
{
public:
    SegmentCounts() : pairs_(alphabet_size * alphabet_size), contexts_(alphabet_size) {}

    void count(const Byte context, const Byte symbol) noexcept
    {
        ++pairs_[std::size_t{context} * alphabet_size + symbol];
        ++contexts_[context];
    }

    // The order-1 entropy of what has been counted, in 1/65536 bits.
    [[nodiscard]] std::int64_t entropy() const noexcept
    {
        const auto n_log_n = [](const std::uint32_t count)
        { return count == 0U ? std::int64_t{0} : std::int64_t{count} * entropy_log2(count); };
        std::int64_t total = 0;
        for (const std::uint32_t count : contexts_)
        {
            total += n_log_n(count);
        }
        for (const std::uint32_t count : pairs_)
        {
            total -= n_log_n(count);
        }
        return total;
    }

    // Entropy change, in 1/65536 bits, from counting `symbol` once more after `context`.
    std::int64_t add(const Byte context, const Byte symbol) noexcept
    {
        std::uint32_t& pair = pairs_[std::size_t{context} * alphabet_size + symbol];
        return entropy_step(contexts_[context]++) - entropy_step(pair++);
    }

    std::int64_t remove(const Byte context, const Byte symbol) noexcept
    {
        std::uint32_t& pair = pairs_[std::size_t{context} * alphabet_size + symbol];
        return entropy_step(--pair) - entropy_step(--contexts_[context]);
    }

    void clear() noexcept
    {
        std::fill(pairs_.begin(), pairs_.end(), 0U);
        std::fill(contexts_.begin(), contexts_.end(), 0U);
    }

private:
    std::vector<std::uint32_t> pairs_;
    std::vector<std::uint32_t> contexts_;
};

[[nodiscard]] Byte segment_context(const std::span<const Byte> data,
                                   const std::size_t index) noexcept
{
    return index > 0U ? data[index - 1U] : Byte{0U};
}

// Whether too many bytes after `cut` repeat something that last occurred before it.
[[nodiscard]] bool separates_repeats(const std::span<const Byte> data, const std::size_t begin,
                                     const std::size_t cut, const std::size_t end,
                                     std::vector<std::uint32_t>& last_seen)
{
    // Positions are stored plus one, so zero means "not seen"; chunks stay below 4 GiB.
    last_seen.assign(std::size_t{1} << repeat_hash_bits, 0U);
    std::uint64_t context = 0;
    std::size_t separated = 0;
    std::size_t next_match = cut;
    for (std::size_t index = begin; index < end; ++index)
    {
        if (index - begin >= repeat_context_size)
        {
            std::uint32_t& slot = last_seen[lzp_slot(context, repeat_hash_bits)];
            const std::size_t previous = slot;
            slot = static_cast<std::uint32_t>(index + 1U);
            if (index >= next_match && previous != 0U)
            {
                const std::size_t length = lzp_match_length(
                    data.first(std::min(end, index + repeat_max_match)), previous - 1U, index);
                const std::size_t source = previous - 1U;
                if (length >= repeat_min_match)
                {
                    separated += source < cut ? length : 0U;
                    next_match = index + length;
                }
            }
        }
        context = (context << 8U) | data[index];
    }
    return separated > (end - cut) >> repeat_share_shift;
}

void find_segments(const std::span<const Byte> data, const std::size_t begin, const std::size_t end,
                   const unsigned int depth, SegmentCounts& left, SegmentCounts& right,
                   std::vector<std::uint32_t>& last_seen, std::vector<std::size_t>& cuts)
{
    if (end - begin < 2U * segment_min_size || depth == segment_max_depth)
    {
        return;
    }
    left.clear();
    right.clear();
    for (std::size_t index = begin; index < end; ++index)
    {
        right.count(segment_context(data, index), data[index]);
    }
    // Entropy of the two sides minus that of the whole, for a cut before `index`.
    std::int64_t change = 0;
    std::int64_t best = -(right.entropy() >> segment_gain_shift) - segment_gain_floor;
    std::size_t cut = 0;
    for (std::size_t index = begin; index + segment_min_size < end; ++index)
    {
        if (change < best && index - begin >= segment_min_size)
        {
            best = change;
            cut = index;
        }
        const Byte context = segment_context(data, index);
        change += right.remove(context, data[index]) + left.add(context, data[index]);
    }
    if (cut == 0U || separates_repeats(data, begin, cut, end, last_seen))
    {
        return;
    }
    find_segments(data, begin, cut, depth + 1U, left, right, last_seen, cuts);
    cuts.push_back(cut);
    find_segments(data, cut, end, depth + 1U, left, right, last_seen, cuts);
}

} // namespace

std::vector<std::size_t> content_boundaries(const std::span<const Byte> data)
{
    std::vector<std::size_t> cuts;
    if (data.size() < 2U * segment_min_size)
    {
        return cuts;
    }
    SegmentCounts left;
    SegmentCounts right;
    std::vector<std::uint32_t> last_seen;
    find_segments(data, 0U, data.size(), 0U, left, right, last_seen, cuts);
    return cuts;
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
