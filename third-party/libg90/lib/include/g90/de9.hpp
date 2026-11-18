#pragma once

#include <chrono>
#include <cstdint>
#include <deque>
#include <g90/body.hpp>
#include <g90/error.hpp>
#include <g90/head.hpp>
#include <g90/types.hpp>
#include <memory>
#include <span>
#include <stdexcept>
#include <vector>

namespace g90::de9 {

/// @brief Injectable byte stream abstraction (serial port or in-memory fake).
///
/// The codec layer does not depend on this type; it exists for serial I/O
/// and unit tests.
class ByteStream {
public:
  virtual ~ByteStream() = default;

  /// @brief Write all bytes to the transport within `timeout`.
  /// @param data Bytes to send.
  /// @param timeout Maximum wait time (never hang unboundedly).
  /// @return `Ok` on success; `Timeout` / `IoError` otherwise.
  [[nodiscard]] virtual Result<void> write_exact(std::span<const uint8_t> data,
                                                 std::chrono::milliseconds timeout) = 0;

  /// @brief Read exactly `buf.size()` bytes or fail.
  /// @param buf Destination buffer.
  /// @param timeout Maximum wait time.
  /// @return `Ok` when the buffer is full; `Timeout` / `IoError` otherwise.
  /// @note Never returns a partial buffer as success.
  [[nodiscard]] virtual Result<void> read_exact(std::span<uint8_t> buf,
                                                std::chrono::milliseconds timeout) = 0;

  /// @brief Flush transport RX (and TX where applicable). No-op by default.
  virtual void reset_buffers() {}
};

/// @brief POSIX termios serial port wrapper (115200 8N1, raw, no SW flow control).
class SerialPort final : public ByteStream {
public:
  SerialPort() = default;
  ~SerialPort() override;

  SerialPort(const SerialPort&) = delete;
  SerialPort& operator=(const SerialPort&) = delete;
  SerialPort(SerialPort&&) = delete;
  SerialPort& operator=(SerialPort&&) = delete;

  /// @brief Open a serial device path at 115200 8N1 raw mode.
  /// @param path Device node, e.g. `"/dev/ttyUSB0"` or `"/dev/cu.usbserial-…"`.
  /// @return `Ok` on success; `IoError` if the path cannot be opened or configured.
  [[nodiscard]] Result<void> open(const char* path);

  /// @brief Close the port if open (also invoked from the destructor).
  void close() noexcept;

  /// @return @c true after a successful `open`.
  [[nodiscard]] bool is_open() const noexcept { return open_; }

  void reset_buffers() override;

  [[nodiscard]] Result<void> write_exact(std::span<const uint8_t> data,
                                         std::chrono::milliseconds timeout) override;
  [[nodiscard]] Result<void> read_exact(std::span<uint8_t> buf,
                                        std::chrono::milliseconds timeout) override;

private:
  bool open_ = false;
  int fd_ = -1;
};

/// @brief In-memory `ByteStream` for tests: writes accumulate, reads dequeue injected RX.
class FakeStream final : public ByteStream {
public:
  FakeStream() = default;
  ~FakeStream() override = default;

  /// @brief Enqueue bytes for subsequent `read_exact` calls.
  /// @param data RX bytes to append to the internal queue.
  void inject_rx(std::span<const uint8_t> data);

  /// @brief Bytes to inject into RX after each successful `write_exact` (survives flush).
  /// @param data Reply payload; empty disables auto-reply.
  void set_reply_after_write(std::span<const uint8_t> data);

  /// @brief Clear accumulated TX and RX queues (does not clear auto-reply).
  void clear();

  /// @return Bytes written via `write_exact` since construction or last `clear`.
  [[nodiscard]] const std::vector<uint8_t>& tx_bytes() const noexcept { return tx_; }

  /// @brief Flush pending RX before TX so a stale body cannot be mistaken for the reply.
  void reset_buffers() override;

  [[nodiscard]] Result<void> write_exact(std::span<const uint8_t> data,
                                         std::chrono::milliseconds timeout) override;
  [[nodiscard]] Result<void> read_exact(std::span<uint8_t> buf,
                                        std::chrono::milliseconds timeout) override;

private:
  std::vector<uint8_t> tx_;
  std::deque<uint8_t> rx_;
  std::vector<uint8_t> reply_after_write_;
};

/// @brief DE-9 session state over a `ByteStream` (hunt body, exchange).
class Session {
public:
  /// @param io Transport to use for all reads and writes.
  explicit Session(ByteStream& io);

  /// @brief Flush RX/TX on the underlying stream when supported.
  void reset_buffers();

  /// @brief Scan the byte stream for a CRC-valid 372-byte body frame (1-byte slip).
  /// @param timeout Maximum search time (initial read + slips).
  /// @return Body frame on success; `Timeout` if none found before the deadline.
  /// @note A frame already read is validated even if the deadline has elapsed.
  [[nodiscard]] Result<BodyFrame> hunt_body(std::chrono::milliseconds timeout);

  /// @brief Flush RX, send a head frame, read/decode the body reply.
  /// @param tx 96-byte head command.
  /// @param timeout Shared budget for write + body hunt.
  /// @return Decoded body reply.
  [[nodiscard]] Result<DecodedBody> exchange(const HeadFrame& tx,
                                             std::chrono::milliseconds timeout);

  /// @brief Last CRC-valid body frame from a successful `hunt_body` / `exchange`.
  [[nodiscard]] const BodyFrame* last_body() const noexcept {
    return have_last_body_ ? &last_body_ : nullptr;
  }

private:
  ByteStream& io_;
  BodyFrame last_body_{};
  bool have_last_body_ = false;
};

} // namespace g90::de9

namespace g90 {

/// @brief High-level DE-9 link: owns transport, profile, and session.
///
/// Encodes a `HeadEncodeRequest`, performs wire exchange, and returns
/// `DecodedBody`. Move is deleted — moved-from would leave a null session.
class De9Link {
public:
  /// @brief Construct a link over the given transport.
  /// @param port Owned byte stream (must not be null).
  /// @param profile Codec profile for encode (default V179 / 1.81).
  /// @throws std::invalid_argument if `port` is null (fail closed).
  explicit De9Link(std::unique_ptr<de9::ByteStream> port, Profile profile = Profile::V179);
  ~De9Link() = default;

  De9Link(const De9Link&) = delete;
  De9Link& operator=(const De9Link&) = delete;
  De9Link(De9Link&&) = delete;
  De9Link& operator=(De9Link&&) = delete;

  /// @brief Encode `req` with the link profile, send head frame, receive and decode body.
  /// @param req Head encode request (same fields as `encode_head`; profile overridden).
  /// @param timeout Wire timeout budget for write + body hunt.
  /// @return Decoded body on success.
  [[nodiscard]] Result<DecodedBody> exchange(const HeadEncodeRequest& req,
                                             std::chrono::milliseconds timeout);

  /// @brief Codec profile used to encode head frames from the next `exchange` on.
  /// @param profile New profile (e.g. from `profile_for_body_version`).
  void set_profile(Profile profile) noexcept { profile_ = profile; }

  /// @brief Codec profile `exchange` encodes with.
  [[nodiscard]] Profile profile() const noexcept { return profile_; }

  /// @brief Last head frame transmitted by a successful or attempted `exchange`.
  /// @return Pointer valid until the next `exchange`; nullptr if none yet.
  [[nodiscard]] const HeadFrame* last_tx() const noexcept {
    return have_last_tx_ ? &last_tx_ : nullptr;
  }

  /// @brief Last body frame received by a successful `exchange`.
  /// @return Pointer valid until the next `exchange`; nullptr if none yet.
  [[nodiscard]] const BodyFrame* last_rx() const noexcept {
    return session_ ? session_->last_body() : nullptr;
  }

private:
  std::unique_ptr<de9::ByteStream> port_;
  Profile profile_;
  std::unique_ptr<de9::Session> session_;
  HeadFrame last_tx_{};
  bool have_last_tx_ = false;
};

} // namespace g90
