#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <g90/error.hpp>
#include <g90/types.hpp>
#include <optional>
#include <string_view>
#include <utility>

namespace g90 {

/// @brief Firmware codec profile for head-frame mode/filter/FFT rules.
///
/// Product labels `1.78` / `1.79` / `1.81` map via `parse_profile`; firmware
/// `1.81` uses the V179 codec (eight modes, 50 Hz filter steps, FFT scale 1).
enum class Profile {
  /// G90 firmware 1.78 — six modes, 25 Hz filter steps, manual FFT scale 1–10.
  V178,
  /// G90 firmware 1.79+ (including product label 1.81) — eight modes, 50 Hz
  /// filter steps, FFT scale fixed at 1.
  V179
};

/// @brief Operating mode carried in a VFO block of a head frame.
enum class Mode {
  Lsb, ///< Lower sideband voice.
  Ld,  ///< Lower sideband digital (V179 only).
  Usb, ///< Upper sideband voice.
  Ud,  ///< Upper sideband digital (V179 only).
  Cw,  ///< CW.
  Cwr, ///< CW reverse.
  Am,  ///< AM.
  Nfm  ///< Narrow FM.
};

/// @brief Wire length of a head→body command frame (bytes 0–95).
constexpr std::size_t kHeadFrameLen = 96;
/// @brief Wire length of a body→head reply frame (bytes 0–371).
constexpr std::size_t kBodyFrameLen = 372;

/// @brief Fixed-size buffer for a 96-byte head command frame.
using HeadFrame = std::array<uint8_t, kHeadFrameLen>;
/// @brief Fixed-size buffer for a 372-byte body reply frame.
using BodyFrame = std::array<uint8_t, kBodyFrameLen>;

// --- Head control-region bit/byte constants (community wire map) ---

/// @brief Byte offset of the PTT / operate control byte in a head frame.
constexpr std::size_t kHeadPttByte = 36;
/// @brief Bit mask: PTT armed when set in byte @ref kHeadPttByte.
constexpr uint8_t kHeadPttBit = 0x80;
/// @brief Bit in byte 37: ATU tune request.
constexpr uint8_t kHeadCtrl2TuningBit = 0x20;
/// @brief Bit in byte 37: shutdown request.
constexpr uint8_t kHeadCtrl2ShutdownBit = 0x80;
/// @brief Bit in byte 38: operate / healthy-stream enable.
constexpr uint8_t kHeadCtrl3Operate = 0x02;
/// @brief Bit in byte 38: VOX enable.
constexpr uint8_t kHeadCtrl3VoxEn = 0x08;
/// @brief Bit in byte 38: CW QSK enable (stock padding sets this).
constexpr uint8_t kHeadCtrl3CwQsk = 0x01;
/// @brief Bit in byte 38: audio input line enable.
constexpr uint8_t kHeadCtrl3AudioLine = 0x04;
/// @brief Bit in byte 38: CW display enable.
constexpr uint8_t kHeadCtrl3CwDisp = 0x10;
/// @brief Bit in byte 36: memory channel mode enable.
constexpr uint8_t kHeadCtrl1MemEn = 0x40;
/// @brief Bit in byte 36: ATU / tuner enable.
constexpr uint8_t kHeadCtrl1TunerEn = 0x20;
/// @brief Bit in byte 36: noise blanker enable.
constexpr uint8_t kHeadCtrl1NbEn = 0x10;
/// @brief Bit in byte 36: mic compression enable.
constexpr uint8_t kHeadCtrl1MicComp = 0x08;
/// @brief Bit in byte 36: `output_headphones` (stock: `getDemodAudioPath`, set in every mode but
/// CW/CWR).
///
/// Set: the body mutes its own speaker (MainUnit V1.81 forces its AF gain to 0 and drives PA12) and
/// puts the demodulated AF on DE-9 phones1/2 for the head to play. The head volume no longer
/// applies. Ears-on verified on a GSOC, 2026-11-07.
constexpr uint8_t kHeadCtrl1Headphones = 0x04;
/// @brief Bit in byte 36: split enable.
constexpr uint8_t kHeadCtrl1Split = 0x02;
/// @brief Bit in byte 36: front-panel lock.
constexpr uint8_t kHeadCtrl1PanelLock = 0x01;
/// @brief Bit in byte 54: active VFO is B when set.
constexpr uint8_t kHeadCtrl6VfoB = 0x01;

/// @brief Parse a profile label from user input.
/// @param s Profile string, e.g. `"1.78"`, `"178"`, `"1.79"`, `"1.81"`, `"v179"`.
/// @return `Profile::V178` or `Profile::V179` on success.
/// @note Product label `1.81` aliases to `Profile::V179`.
[[nodiscard]] Result<Profile> parse_profile(std::string_view s);

/// @brief Profile for the MainUnit firmware a body frame reports.
/// @param major Body byte 7 (`BodyStatus::info7`), e.g. `0x01`.
/// @param minor Body byte 6 (`BodyStatus::info6`), e.g. `0x81`.
/// @return `Profile::V178` below `1.79`, else `Profile::V179`.
/// @note The bytes are hex digits as the stock head prints them (`%X.%02X`):
/// `0x01`/`0x81` is `1.81`. The MainUnit applies the head frame's mode and
/// filter bytes, so its version, not the head's, picks the profile.
[[nodiscard]] Profile profile_for_body_version(uint8_t major, uint8_t minor) noexcept;

/// @brief Parse a mode name from CLI or config text.
/// @param s Mode name (`"USB"`, `"L-D"`, `"CW-R"`, etc.).
/// @return Matching `Mode`, or `Error::InvalidMode`.
[[nodiscard]] Result<Mode> mode_from_string(std::string_view s);

/// @brief Canonical uppercase mode label for display.
/// @param m Mode value.
/// @return Stable string such as `"USB"` or `"L-D"`.
[[nodiscard]] std::string_view mode_to_string(Mode m) noexcept;

/// @brief Encode a logical mode to the profile-specific wire byte.
/// @param profile Active codec profile.
/// @param mode Logical mode; L-D/U-D are invalid on V178.
/// @return Raw mode byte, or `Error::InvalidMode` for unsupported combinations.
[[nodiscard]] Result<uint8_t> mode_to_raw(Profile profile, Mode mode);

/// @brief Decode a wire mode byte for the given profile.
/// @param profile Active codec profile.
/// @param raw Mode byte from the VFO block.
/// @return Logical `Mode`, or `Error::InvalidMode`.
[[nodiscard]] Result<Mode> mode_from_raw(Profile profile, uint8_t raw);

/// @brief Raw filter edge bytes as stored in a VFO block (offsets +8/+9).
struct FilterRaw {
  uint8_t high = 0; ///< High-edge raw byte.
  uint8_t low = 0;  ///< Low-edge raw byte.
};

/// @brief Filter edges decoded to hertz.
struct FilterHz {
  uint16_t high = 0; ///< High cutoff in Hz.
  uint16_t low = 0;  ///< Low cutoff in Hz.
};

/// @brief Encode filter edges to wire raw bytes.
/// @param profile V178 uses 25 Hz steps from 125/100 Hz; V179 uses 50 Hz from 50 Hz.
/// @param high_hz High-edge frequency in hertz.
/// @param low_hz Low-edge frequency in hertz.
/// @return Clamped raw pair (never fails).
[[nodiscard]] FilterRaw encode_filter(Profile profile, uint16_t high_hz, uint16_t low_hz);

/// @brief Decode wire raw filter bytes to hertz.
/// @param profile Active codec profile.
/// @param high_raw High-edge raw byte from the frame.
/// @param low_raw Low-edge raw byte from the frame.
/// @return Decoded Hz pair.
[[nodiscard]] FilterHz decode_filter(Profile profile, uint8_t high_raw, uint8_t low_raw);

/// @brief Validate and normalize an FFT scale nibble.
/// @param profile V178 allows 1–10; V179 allows only 1.
/// @param value Requested scale (low 4 bits used).
/// @return Normalized scale, or `Error::InvalidFftScale`.
[[nodiscard]] Result<uint8_t> validate_fft_scale(Profile profile, uint8_t value);

/// @brief Pack RCLK offset into wire bytes (byte 38 high bits + byte 39).
/// @param hz Offset in hertz (−1000…+1023 domain).
/// @return `(low3, high_byte)` for overlay into the head frame.
[[nodiscard]] std::pair<uint8_t, uint8_t> pack_rclk_hz(int hz);

/// @brief Unpack RCLK offset from wire bytes.
/// @param low3 Low 3 bits from byte 38.
/// @param high Byte 39 value.
/// @return RCLK offset in hertz.
[[nodiscard]] int unpack_rclk_hz(uint8_t low3, uint8_t high);

/// @brief Pack RIT offset into wire bytes (bytes 84–85).
/// @param hz RIT offset in hertz (−512…+511).
/// @return `(low_byte, high2_bits)` for the frame.
[[nodiscard]] std::pair<uint8_t, uint8_t> pack_rit_hz(int hz);

/// @brief Unpack RIT offset from wire bytes.
/// @param low Byte 84.
/// @param high2 Low 2 bits of byte 85.
/// @return RIT offset in hertz.
[[nodiscard]] int unpack_rit_hz(uint8_t low, uint8_t high2);

/// @brief Pack AF gain, beep, and band-stack flags (bytes 42–43).
/// @param gain AF gain 0…127.
/// @param beep_en Beep enable (default @c true).
/// @param band_stack_full Band-stack-full flag (default @c false).
/// @return `(byte42, byte43)` wire pair.
[[nodiscard]] std::pair<uint8_t, uint8_t>
pack_af_gain_beep(int gain, bool beep_en = true, bool band_stack_full = false);

/// @brief Pack CW ratio / mode-B / MLR into byte 61.
/// @param cw_ratio_raw Ratio nibble (default 0).
/// @param cw_mode_b CW mode B flag (default @c false).
/// @param cw_mlr MLR field 0…3 (default 1).
/// @return Packed control byte.
[[nodiscard]] uint8_t
pack_ctrl7(uint8_t cw_ratio_raw = 0, bool cw_mode_b = false, uint8_t cw_mlr = 1);

/// @brief Pack SWR threshold and aux volume nibbles (bytes 82–83).
/// @param swr_threshold_raw SWR threshold nibble (default 1).
/// @param aux_out_vol Aux out volume nibble (default 9).
/// @param aux_in_vol Aux in volume nibble (default 8).
/// @return `(byte82, byte83)` wire pair.
[[nodiscard]] std::pair<uint8_t, uint8_t>
pack_ctrl8(uint8_t swr_threshold_raw = 1, uint8_t aux_out_vol = 9, uint8_t aux_in_vol = 8);

/// @brief One VFO block inside a head encode request (freq/mode/filter/FFT).
struct HeadVfo {
  uint32_t freq_hz = 14'225'000; ///< VFO frequency in hertz.
  Mode mode = Mode::Usb;         ///< Operating mode.

  /// Unset → stock default (V178: 2725/200 Hz, V179: 2700/200 Hz). Explicit
  /// 2700 Hz on V178 is preserved (not rewritten to 2725).
  std::optional<uint16_t> filter_high_hz;
  /// Low-edge Hz; paired with `filter_high_hz` when set.
  std::optional<uint16_t> filter_low_hz;

  /// Unset → `1` for VFO-A; for VFO-B, inherits A's resolved scale after encode.
  std::optional<uint8_t> fft_scale;

  /// AGC mode byte (VFO block +6): 0=OFF, 1=SLOW, 2=FAST, 3=AUTO.
  /// Unset → `0` (preserves existing golden encode bit-identity).
  std::optional<uint8_t> agc;

  /// Raw filter bytes override Hz fields; both must be set together or
  /// `encode_head` returns `Error::InvalidFilter`.
  std::optional<uint8_t> filter_high_raw;
  std::optional<uint8_t> filter_low_raw;
};

/// @brief Optional control overlays applied after stock padding (bytes 36–91).
///
/// Each field is applied only when the `std::optional` is set. See
/// `docs/ENCODE-PADDING.md` for overlay order.
struct HeadControls {
  std::optional<bool> nb_en;
  std::optional<uint8_t> nb_level; ///< High nibble of byte 46.
  std::optional<uint8_t> nb_width; ///< Low nibble of byte 46.
  std::optional<bool> split_en;
  std::optional<bool> vfo_b_en; ///< Select VFO-B (byte 54).
  std::optional<bool> headphones;
  std::optional<bool> audio_in_line_en;
  std::optional<bool> mic_compression;
  std::optional<bool> tuner_en;
  std::optional<bool> panel_lock;
  std::optional<bool> mem_en;
  std::optional<uint8_t> mem_ch;    ///< Memory channel (byte 53).
  std::optional<bool> shutdown_req; ///< Sets byte 37 shutdown bit; caller owns policy.
  std::optional<bool> cw_disp_en;
  std::optional<bool> cw_qsk;
  std::optional<uint8_t> cw_wpm; ///< Byte 60.
  std::optional<uint8_t> cw_qsk_time_raw;
  std::optional<uint8_t> cw_ratio_raw;
  std::optional<bool> cw_mode_b;
  std::optional<uint8_t> cw_mlr;
  std::optional<uint8_t> swr_threshold_raw;
  std::optional<uint8_t> aux_out_vol;
  std::optional<uint8_t> aux_in_vol;
  std::optional<bool> band_stack_full;
  std::optional<int> rit_hz;
  std::optional<int> rclk_hz;
  std::optional<uint8_t> sql_level; ///< Byte 45.
  std::optional<uint8_t> mic_gain;  ///< Byte 49.
  std::optional<uint8_t> rf_power;  ///< Byte 44.
  std::optional<uint8_t> volume;    ///< Byte 48 (also set from request default).
  std::optional<uint8_t> af_gain;
  std::optional<bool> beep_en;
};

/// @brief Laboratory / TX-arm knobs layered after `HeadControls`.
///
/// Arm flags (`arm_ptt`, `arm_atu_tune`, `arm_vox`) are ignored unless `allow_tx`
/// is true.
struct LabExtras {
  bool allow_tx = false;           ///< Master TX enable for arm_* flags.
  bool arm_ptt = false;            ///< Arm PTT (byte 36 bit 7).
  bool arm_atu_tune = false;       ///< Arm ATU tune (byte 37 tuning bit).
  bool arm_vox = false;            ///< Arm VOX (byte 38 VOX bit).
  bool clear_operate = false;      ///< When set, clear operate/PTT/tune/VOX before CRC.
  std::optional<uint8_t> pad2b;    ///< Override VFO-B pad byte (offset 27).
  std::optional<uint8_t> unknown2; ///< Override stock padding byte 52.
};

/// @brief Returns whether any TX arm flag is set (ignores `allow_tx`).
/// @param lab Lab extras block to inspect.
/// @return @c true if `arm_ptt`, `arm_atu_tune`, or `arm_vox` is set.
/// @note Combine with `allow_tx` to audit encode intent before calling `encode_head`.
[[nodiscard]] constexpr bool lab_arms_tx(const LabExtras& lab) noexcept {
  return lab.arm_ptt || lab.arm_atu_tune || lab.arm_vox;
}

/// @brief Full input to `encode_head`: profile, VFOs, volume, optional overlays.
struct HeadEncodeRequest {
  Profile profile = Profile::V179;
  uint8_t volume = 1; ///< Main volume written to byte 48 before control overlays.
  HeadVfo vfo_a{};
  HeadVfo vfo_b{.freq_hz = 7'175'000,
                .mode = Mode::Lsb,
                .filter_high_hz = {},
                .filter_low_hz = {},
                .fft_scale = {},
                .agc = {},
                .filter_high_raw = {},
                .filter_low_raw = {}};
  std::optional<HeadControls> controls;
  std::optional<LabExtras> lab;
};

/// @brief Subset of head-frame fields returned by `decode_head`.
struct DecodedHead {
  /// @brief STM32 CRC over bytes 0–91 matches bytes 92–95.
  /// @note Soft CRC: decode returns `Ok` even when @c false; callers may still
  ///       treat a mismatch as a hard error (e.g. `g90ctl decode head`).
  bool crc_ok = false;
  Mode mode_a{};
  uint32_t freq_a_hz = 0;
  uint16_t filter_high_hz = 0;
  uint16_t filter_low_hz = 0;
  uint8_t filter_high_raw = 0;
  uint8_t filter_low_raw = 0;
  uint8_t volume = 0;
  uint8_t fft_scale = 0;
  Mode mode_b{};
  uint32_t freq_b_hz = 0;
  uint8_t filter_high_raw_b = 0;
  uint8_t filter_low_raw_b = 0;
  uint8_t fft_scale_b = 0;
};

/// @brief Encode a 96-byte head→body command frame from a layered request.
/// @param req VFO-A/B, volume, optional `HeadControls` and `LabExtras`.
/// @return Head frame on success.
/// @return `Error::InvalidMode`, `InvalidFilter`, `InvalidFftScale`, etc. on bad input.
/// @note Encode is **raw**: does not auto-clear PTT/tune/VOX.
/// @see docs/ENCODE-PADDING.md for the stock padding template.
[[nodiscard]] Result<HeadFrame> encode_head(const HeadEncodeRequest& req);

/// @brief Decode a head command frame into logical VFO and volume fields.
/// @param profile Codec profile used for mode/filter/FFT interpretation.
/// @param frame 96-byte wire buffer (sync `0x55 0xAA`).
/// @return Decoded fields; `crc_ok` reflects CRC match but does not fail the call.
/// @return `Error::BadSync` when sync bytes are wrong; `InvalidMode` on bad mode byte.
/// @note Soft CRC: `Ok` with `crc_ok=false` is a valid decode result.
[[nodiscard]] Result<DecodedHead> decode_head(Profile profile, const HeadFrame& frame);

/// @brief Detect whether a head frame would request TX (PTT, ATU tune, or VOX).
/// @param frame Candidate head frame (must be CRC-valid for a definitive answer).
/// @return `Ok(true)` when PTT, tune, or VOX is armed; `Ok(false)` when none are.
/// @return `Err(BadSync)` or `Err(BadCrc)` when the frame cannot be trusted.
/// @note Used by `g90ctl live` fail-closed TX policy; see `docs/DESIGN.md`.
///       `Err` or `true` blocks live TX unless `--allow-tx`.
[[nodiscard]] Result<bool> head_frame_requests_tx(const HeadFrame& frame);

} // namespace g90
