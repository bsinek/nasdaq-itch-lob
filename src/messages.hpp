#pragma once
#include <array>
#include <cstdint>
#include <cstring>

namespace itch {

// Compile with -DITCH_UNCHECKED to strip hot-path safety checks (frame
// validation, level-deficit tracking, zero-share guards) for a trusted-input
// benchmark build. The default build keeps them; both numbers are reported in
// the README. Never use the unchecked build for validation or export.
#ifdef ITCH_UNCHECKED
inline constexpr bool kChecked = false;
#else
inline constexpr bool kChecked = true;
#endif

// Message lengths (including the 1-byte type), transcribed from the official
// Nasdaq TotalView-ITCH 5.0 specification (fetched 2026-08-06; docs/spec/ is
// gitignored — scripts/get_spec.sh re-downloads it). Historical-file framing:
// 2-byte big-endian length prefix per message whose value equals these
// lengths (verified empirically; see docs/DECISIONS.md).
constexpr std::array<uint8_t, 256> make_msg_len() {
  std::array<uint8_t, 256> t{};
  t['S'] = 12;  t['R'] = 39;  t['H'] = 25;  t['Y'] = 20;  t['L'] = 26;
  t['V'] = 35;  t['W'] = 12;  t['K'] = 28;  t['J'] = 35;  t['h'] = 21;
  t['A'] = 36;  t['F'] = 40;  t['E'] = 31;  t['C'] = 36;  t['X'] = 23;
  t['D'] = 19;  t['U'] = 35;  t['P'] = 44;  t['Q'] = 40;  t['B'] = 19;
  t['I'] = 50;  t['N'] = 20;  t['O'] = 48;
  return t;
}
inline constexpr std::array<uint8_t, 256> kMsgLen = make_msg_len();

inline uint16_t be16(const uint8_t* p) { uint16_t v; std::memcpy(&v, p, 2); return __builtin_bswap16(v); }
inline uint32_t be32(const uint8_t* p) { uint32_t v; std::memcpy(&v, p, 4); return __builtin_bswap32(v); }
inline uint64_t be64(const uint8_t* p) { uint64_t v; std::memcpy(&v, p, 8); return __builtin_bswap64(v); }
inline uint64_t be48(const uint8_t* p) { return (uint64_t(be16(p)) << 32) | be32(p + 2); }

// Fields shared by every stock-related message (offset 0 = type byte).
inline uint16_t f_locate(const uint8_t* m) { return be16(m + 1); }
inline uint64_t f_ts(const uint8_t* m) { return be48(m + 5); }  // ns since midnight

}  // namespace itch
