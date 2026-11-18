/// DE-9 serial transport: POSIX SerialPort, Session hunt/exchange, De9Link.
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <g90/de9.hpp>
#include <limits>
#include <poll.h>
#include <termios.h>
#include <unistd.h>

namespace g90::de9 {
namespace {

[[nodiscard]] std::chrono::milliseconds
remaining_until(std::chrono::steady_clock::time_point deadline) {
  const auto now = std::chrono::steady_clock::now();
  if (now >= deadline) {
    return std::chrono::milliseconds{0};
  }
  return std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
}

[[nodiscard]] int poll_wait(int fd, short events, std::chrono::milliseconds timeout) {
  pollfd pfd{};
  pfd.fd = fd;
  pfd.events = events;
  pfd.revents = 0;
  const int ms =
      timeout.count() > static_cast<std::chrono::milliseconds::rep>(std::numeric_limits<int>::max())
          ? std::numeric_limits<int>::max()
          : static_cast<int>(timeout.count());
  return ::poll(&pfd, 1, ms);
}

} // namespace

SerialPort::~SerialPort() {
  close();
}

void SerialPort::close() noexcept {
  if (fd_ >= 0) {
    ::close(fd_);
    fd_ = -1;
  }
  open_ = false;
}

Result<void> SerialPort::open(const char* path) {
  if (path == nullptr || path[0] == '\0') {
    return Result<void>::Err(Error::IoError);
  }
  close();

  const int fd = ::open(path, O_RDWR | O_NOCTTY | O_NONBLOCK);
  if (fd < 0) {
    return Result<void>::Err(Error::IoError);
  }

  termios tty{};
  if (tcgetattr(fd, &tty) != 0) {
    ::close(fd);
    return Result<void>::Err(Error::IoError);
  }

  cfmakeraw(&tty);
  if (cfsetispeed(&tty, B115200) != 0 || cfsetospeed(&tty, B115200) != 0) {
    ::close(fd);
    return Result<void>::Err(Error::IoError);
  }

  tty.c_cflag |= static_cast<tcflag_t>(CLOCAL | CREAD);
  tty.c_cflag &= ~static_cast<tcflag_t>(CSIZE);
  tty.c_cflag |= CS8;
  tty.c_cflag &= ~static_cast<tcflag_t>(PARENB | CSTOPB);
#ifdef CRTSCTS
  tty.c_cflag &= ~static_cast<tcflag_t>(CRTSCTS);
#endif
  tty.c_iflag &= ~static_cast<tcflag_t>(IXON | IXOFF | IXANY);
  tty.c_cc[VMIN] = 0;
  tty.c_cc[VTIME] = 0;

  if (tcsetattr(fd, TCSANOW, &tty) != 0) {
    ::close(fd);
    return Result<void>::Err(Error::IoError);
  }

  fd_ = fd;
  open_ = true;
  // Clear stale RX/TX on open.
  reset_buffers();
  return Result<void>::Ok();
}

void SerialPort::reset_buffers() {
  if (!open_ || fd_ < 0) {
    return;
  }
  (void)tcflush(fd_, TCIOFLUSH);
}

Result<void> SerialPort::write_exact(std::span<const uint8_t> data,
                                     std::chrono::milliseconds timeout) {
  if (!open_ || fd_ < 0) {
    return Result<void>::Err(Error::IoError);
  }
  if (data.empty()) {
    return Result<void>::Ok();
  }

  const auto deadline = std::chrono::steady_clock::now() + timeout;
  std::size_t written = 0;
  while (written < data.size()) {
    const auto rem = remaining_until(deadline);
    if (rem.count() <= 0) {
      return Result<void>::Err(Error::Timeout);
    }

    const ssize_t n = ::write(fd_, data.data() + written, data.size() - written);
    if (n < 0) {
      if (errno == EINTR) {
        continue;
      }
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        const int pr = poll_wait(fd_, POLLOUT, rem);
        if (pr < 0) {
          if (errno == EINTR) {
            continue;
          }
          return Result<void>::Err(Error::IoError);
        }
        if (pr == 0) {
          return Result<void>::Err(Error::Timeout);
        }
        continue;
      }
      return Result<void>::Err(Error::IoError);
    }
    if (n == 0) {
      return Result<void>::Err(Error::IoError);
    }
    written += static_cast<std::size_t>(n);
  }

  // Drain only; retry EINTR. Do not treat other errors as "retry whole frame".
  for (;;) {
    if (tcdrain(fd_) == 0) {
      break;
    }
    if (errno == EINTR) {
      continue;
    }
    return Result<void>::Err(Error::IoError);
  }
  return Result<void>::Ok();
}

Result<void> SerialPort::read_exact(std::span<uint8_t> buf, std::chrono::milliseconds timeout) {
  if (!open_ || fd_ < 0) {
    return Result<void>::Err(Error::IoError);
  }
  if (buf.empty()) {
    return Result<void>::Ok();
  }

  const auto deadline = std::chrono::steady_clock::now() + timeout;
  std::size_t got = 0;
  while (got < buf.size()) {
    const auto rem = remaining_until(deadline);
    if (rem.count() <= 0) {
      return Result<void>::Err(Error::Timeout);
    }

    const int pr = poll_wait(fd_, POLLIN, rem);
    if (pr < 0) {
      if (errno == EINTR) {
        continue;
      }
      return Result<void>::Err(Error::IoError);
    }
    if (pr == 0) {
      return Result<void>::Err(Error::Timeout);
    }

    const ssize_t n = ::read(fd_, buf.data() + got, buf.size() - got);
    if (n < 0) {
      if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
        continue;
      }
      return Result<void>::Err(Error::IoError);
    }
    if (n == 0) {
      // Peer hang-up / no data after poll — treat as I/O failure, not a short OK.
      return Result<void>::Err(Error::IoError);
    }
    got += static_cast<std::size_t>(n);
  }
  return Result<void>::Ok();
}

void FakeStream::inject_rx(std::span<const uint8_t> data) {
  rx_.insert(rx_.end(), data.begin(), data.end());
}

void FakeStream::set_reply_after_write(std::span<const uint8_t> data) {
  reply_after_write_.assign(data.begin(), data.end());
}

void FakeStream::clear() {
  tx_.clear();
  rx_.clear();
}

void FakeStream::reset_buffers() {
  rx_.clear();
}

Result<void> FakeStream::write_exact(std::span<const uint8_t> data,
                                     std::chrono::milliseconds timeout) {
  (void)timeout;
  tx_.insert(tx_.end(), data.begin(), data.end());
  if (!reply_after_write_.empty()) {
    inject_rx(reply_after_write_);
  }
  return Result<void>::Ok();
}

Result<void> FakeStream::read_exact(std::span<uint8_t> buf, std::chrono::milliseconds timeout) {
  (void)timeout;
  // Unit-test transport: insufficient RX is an immediate Timeout. Never return a
  // partial buffer as success.
  if (rx_.size() < buf.size()) {
    return Result<void>::Err(Error::Timeout);
  }
  for (std::size_t i = 0; i < buf.size(); ++i) {
    buf[i] = rx_.front();
    rx_.pop_front();
  }
  return Result<void>::Ok();
}

Session::Session(ByteStream& io) : io_(io) {}

void Session::reset_buffers() {
  io_.reset_buffers();
}

Result<BodyFrame> Session::hunt_body(std::chrono::milliseconds timeout) {
  using clock = std::chrono::steady_clock;
  have_last_body_ = false;
  const auto deadline = clock::now() + timeout;

  BodyFrame frame{};
  {
    const auto rem = remaining_until(deadline);
    const auto rr = io_.read_exact(frame, rem);
    if (!rr.ok()) {
      return Result<BodyFrame>::Err(rr.err());
    }
  }

  // Validate the frame already in hand before gating on the deadline (a read that
  // completed at/after the deadline still yields a usable window).
  for (;;) {
    const auto dec = decode_body(frame);
    if (!dec.ok()) {
      return Result<BodyFrame>::Err(dec.err());
    }
    if (dec.value->crc_ok) {
      last_body_ = frame;
      have_last_body_ = true;
      return Result<BodyFrame>::Ok(frame);
    }

    const auto rem = remaining_until(deadline);
    if (rem.count() <= 0) {
      return Result<BodyFrame>::Err(Error::Timeout);
    }
    uint8_t nxt = 0;
    const auto nr = io_.read_exact(std::span<uint8_t>(&nxt, 1), rem);
    if (!nr.ok()) {
      return Result<BodyFrame>::Err(nr.err());
    }
    std::move(frame.begin() + 1, frame.end(), frame.begin());
    frame.back() = nxt;
  }
}

Result<DecodedBody> Session::exchange(const HeadFrame& tx, std::chrono::milliseconds timeout) {
  using clock = std::chrono::steady_clock;
  const auto deadline = clock::now() + timeout;

  // Drop stale RX so a buffered body cannot masquerade as this exchange's reply.
  io_.reset_buffers();

  {
    const auto rem = remaining_until(deadline);
    const auto wr = io_.write_exact(tx, rem);
    if (!wr.ok()) {
      return Result<DecodedBody>::Err(wr.err());
    }
  }

  const auto body = hunt_body(remaining_until(deadline));
  if (!body.ok()) {
    return Result<DecodedBody>::Err(body.err());
  }
  return decode_body(*body.value);
}

} // namespace g90::de9

namespace g90 {

De9Link::De9Link(std::unique_ptr<de9::ByteStream> port, Profile profile)
    : port_(std::move(port)), profile_(profile), session_(nullptr) {
  if (!port_) {
    throw std::invalid_argument("De9Link: port must not be null");
  }
  session_ = std::make_unique<de9::Session>(*port_);
}

Result<DecodedBody> De9Link::exchange(const HeadEncodeRequest& req,
                                      std::chrono::milliseconds timeout) {
  if (!session_) {
    return Result<DecodedBody>::Err(Error::IoError);
  }
  have_last_tx_ = false;
  HeadEncodeRequest encoded = req;
  encoded.profile = profile_;
  const auto frame = encode_head(encoded);
  if (!frame.ok()) {
    return Result<DecodedBody>::Err(frame.err());
  }
  last_tx_ = *frame.value;
  have_last_tx_ = true;
  return session_->exchange(*frame.value, timeout);
}

} // namespace g90
