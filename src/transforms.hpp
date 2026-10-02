#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace mzip::detail
{

using Byte = std::uint8_t;
using Bytes = std::vector<Byte>;
using Symbol = std::uint16_t;

// RUNA/RUNB spell zero-run lengths in bijective base 2, RUNC/RUND repeats of the preceding
// literal, and byte v maps to symbol v + run_literal_offset.
inline constexpr Symbol run_a = 0;
inline constexpr Symbol run_b = 1;
inline constexpr Symbol run_c = 2;
inline constexpr Symbol run_d = 3;
inline constexpr Symbol run_literal_offset = 3;
inline constexpr std::size_t run_alphabet_size = 259;

struct BwtResult
{
    Bytes data;
    std::uint32_t primary_index = 0;
};

[[nodiscard]] BwtResult bwt_encode(std::span<const Byte> input);
[[nodiscard]] Bytes bwt_decode(std::span<const Byte> input, std::uint32_t primary_index);

void mtf_encode(Bytes& data);
void mtf_decode(Bytes& data);

// Run coding fused with the range coder; min_extension_run is the shortest RUNC/RUND repeat.
struct EncodedStream
{
    Bytes payload;
    std::size_t symbol_count = 0;
};

// Returns nothing once the payload reaches size_limit.
[[nodiscard]] std::optional<EncodedStream>
rc_encode(std::span<const Byte> input, std::size_t min_extension_run, std::size_t size_limit);
[[nodiscard]] Bytes rc_decode(std::span<const Byte> payload, std::size_t symbol_count,
                              std::size_t expected_size);

// Context-mixing coder over raw BWT output; returns nothing once the payload reaches size_limit.
[[nodiscard]] std::optional<Bytes> cm_encode(std::span<const Byte> input, std::size_t size_limit);
[[nodiscard]] Bytes cm_decode(std::span<const Byte> payload, std::size_t expected_size);
// Decoder for the version 2 model, kept so existing archives stay readable.
[[nodiscard]] Bytes cm_decode_v2(std::span<const Byte> payload, std::size_t expected_size);

// LZP: long repeats become marker+length tokens; the stream starts with the marker byte.
// Encode returns nothing unless the stream shrinks; hash_bits sizes the shared table.
[[nodiscard]] std::optional<Bytes> lzp_encode(std::span<const Byte> input,
                                              unsigned int hash_bits = 20U);
[[nodiscard]] Bytes lzp_decode(std::span<const Byte> input, std::size_t expected_size,
                               unsigned int hash_bits = 20U);

// x86 branch-target filter: the rel32 operand after every E8 (call) and E9 (jmp rel32) opcode
// becomes an absolute block offset, so repeated calls to one function look alike. Operands
// whose top byte is neither 0x00 nor 0xFF are left alone, and both walks skip the four
// operand bytes after every opcode, so the inverse visits exactly the same positions. The
// count, also returned by encode, is the number of converted operands whose target lands
// inside the block, a measure of how much machine code the block holds.
[[nodiscard]] std::size_t x86_branch_targets(std::span<const Byte> data) noexcept;
std::size_t x86_filter_encode(std::span<Byte> data) noexcept;
void x86_filter_decode(std::span<Byte> data) noexcept;

[[nodiscard]] std::uint32_t adler32(std::span<const Byte> input) noexcept;

} // namespace mzip::detail
