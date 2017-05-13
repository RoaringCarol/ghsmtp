#ifndef SOCKBUFFER_DOT_HPP
#define SOCKBUFFER_DOT_HPP

#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <streambuf>
#include <string>

#include "POSIX.hpp"
#include "TLS-OpenSSL.hpp"

#include <glog/logging.h>

#include <boost/iostreams/concepts.hpp>
#include <boost/iostreams/stream.hpp>

namespace Config {
// Read timeout value gleaned from RFC-1123 section 5.3.2 and RFC-5321
// section 4.5.3.2.7.
constexpr auto read_timeout = std::chrono::minutes(5);
constexpr auto write_timeout = std::chrono::seconds(10);
constexpr auto starttls_timeout = std::chrono::seconds(10);
}

class SockBuffer
    : public boost::iostreams::device<boost::iostreams::bidirectional> {
public:
  SockBuffer& operator=(const SockBuffer&) = delete;
  SockBuffer(SockBuffer const& that)
    : fd_in_(that.fd_in_)
    , fd_out_(that.fd_out_)
  {
    CHECK(!that.timed_out_);
    CHECK(!that.tls_active_);
  }

  SockBuffer(int fd_in, int fd_out)
    : fd_in_(fd_in)
    , fd_out_(fd_out)
  {
    POSIX::set_nonblocking(fd_in_);
    POSIX::set_nonblocking(fd_out_);
  }
  bool input_ready(std::chrono::milliseconds wait) const
  {
    return POSIX::input_ready(fd_in_, wait);
  }
  bool output_ready(std::chrono::milliseconds wait) const
  {
    return POSIX::output_ready(fd_out_, wait);
  }
  bool timed_out() const { return timed_out_; }
  std::streamsize read(char* s, std::streamsize n)
  {
    return tls_active_
               ? tls_.read(s, n, Config::read_timeout, timed_out_)
               : POSIX::read(fd_in_, s, n, Config::read_timeout, timed_out_);
  }
  std::streamsize write(const char* s, std::streamsize n)
  {
    return tls_active_
               ? tls_.write(s, n, Config::write_timeout, timed_out_)
               : POSIX::write(fd_out_, s, n, Config::write_timeout, timed_out_);
  }
  void starttls()
  {
    tls_.starttls(fd_in_, fd_out_, Config::starttls_timeout);
    tls_active_ = true;
  }
  std::string tls_info()
  {
    if (tls_active_) {
      return tls_.info();
    }
    return "";
  }
  bool tls() { return tls_active_; }

private:
  int fd_in_;
  int fd_out_;

  bool timed_out_{false};

  bool tls_active_{false};
  TLS tls_;
};

#endif // SOCKBUFFER_DOT_HPP