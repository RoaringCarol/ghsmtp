/*
    This file is part of ghsmtp - Gene's simple SMTP server.
    Copyright (C) 2013  Gene Hightower <gene@digilicious.com>

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU Affero General Public License as
    published by the Free Software Foundation, either version 3 of the
    License, or (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU Affero General Public License for more details.

    You should have received a copy of the GNU Affero General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#ifndef SOCKBUFFER_DOT_HPP
#define SOCKBUFFER_DOT_HPP

#include <cerrno>
#include <chrono>
#include <cstring> // std::strerror
#include <sstream>
#include <stdexcept>
#include <streambuf>
#include <string>

#include <fcntl.h>
#include <sys/select.h>

#include "Logging.hpp"

#include <boost/iostreams/concepts.hpp>
#include <boost/iostreams/stream.hpp>

#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/ssl.h>

namespace Config {
// Timeout value gleaned from RFC-1123 section 5.3.2 and RFC-5321
// section 4.5.3.2.7.
constexpr auto read_timeout = std::chrono::minutes(5);
constexpr auto write_timeout = std::chrono::seconds(5);
constexpr auto starttls_timeout = std::chrono::seconds(5);
}

class io_error : public std::runtime_error {
public:
  explicit io_error(int e) : std::runtime_error(errno_to_str(e))
  {
  }

private:
  static std::string errno_to_str(int e)
  {
    std::stringstream ss;
    ss << "read() error errno==" << e << ": " << std::strerror(e);
    return ss.str();
  }
};

class SockBuffer
    : public boost::iostreams::device<boost::iostreams::bidirectional> {
public:
  SockBuffer(int fd_in, int fd_out)
    : fd_in_(fd_in)
    , fd_out_(fd_out)
    , timed_out_(false)
    , tls_(false)
  {
    int flags;
    PCHECK((flags = fcntl(fd_in_, F_GETFL, 0)) != -1);
    if (0 == (flags&O_NONBLOCK)) {
      PCHECK(fcntl(fd_in_, F_SETFL, flags | O_NONBLOCK) != -1);
    }
    PCHECK((flags = fcntl(fd_out_, F_GETFL, 0)) != -1);
    if (0 == (flags&O_NONBLOCK)) {
      PCHECK(fcntl(fd_out_, F_SETFL, flags | O_NONBLOCK) != -1);
    }

    // TLS

    SSL_load_error_strings();
    SSL_library_init();
    CHECK(RAND_status()); // Be sure the PRNG has been seeded with enough data.
    OpenSSL_add_all_algorithms();

    const SSL_METHOD* method = CHECK_NOTNULL(SSLv23_server_method());
    ctx_ = CHECK_NOTNULL(SSL_CTX_new(method));

    if (SSL_CTX_use_certificate_file(ctx_, "/z/home/gene/src/smtpd/cert.pem",
                                     SSL_FILETYPE_PEM) <= 0) {
      ssl_error();
    }
    if (SSL_CTX_use_PrivateKey_file(ctx_, "/z/home/gene/src/smtpd/cert.pem",
                                    SSL_FILETYPE_PEM) <= 0) {
      ssl_error();
    }
    CHECK(SSL_CTX_check_private_key(ctx_))
        << "Private key does not match the public certificate";

    constexpr char dh_ike_23_pem[] =
        "-----BEGIN DH PARAMETERS-----\n"
        "MIICCgKCAQEArRB+HpEjqdDWYPqnlVnFH6INZOVoO5/RtUsVl7YdCnXm+hQd+VpW\n"
        "26+aPEB7od8V6z1oijCcGA4d5rhaEnSgpm0/gVKtasISkDfJ7e/aTfjZHo/vVbc5\n"
        "S3rVt9C2wSIHyfmNEe002/bGugssi7wnvmoA4KC5xJcIs7+KMXCRiDaBKGEwvImF\n"
        "2xYC5xRBXZMwJ4Jzx94x79xzEPcSH9WgdBWYfZrcCkhtzfk6zEQyg4cxXXXhmMZB\n"
        "pIDNhqG55YfovmDmnMkosrnFIXLkEwQumyPxCw4W55djybU9z0uoCinj+3PBa451\n"
        "uX7zY+L/ox9xz53lOE5xuBwKxN/+DBDmTwKCAQEArEAy708tmuOd8wtcj/2sUGze\n"
        "vnuJmYyvdIZqCM/k/+OmgkpOELmm8N2SHwGnDEr6q3OddwDCn1LFfbF8YgqGUr5e\n"
        "kAGo1mrXwXZpEBmZAkr00CcnWsE0i7inYtBSG8mK4kcVBCLqHtQJk51U2nRgzbX2\n"
        "xrJQcXy+8YDrNBGOmNEZUppF1vg0Vm4wJeMWozDvu3eobwwasVsFGuPUKMj4rLcK\n"
        "gTcVC47rEOGD7dGZY93Z4mPkdwWJ72qiHn9fL/OBtTnM40CdE81Wavu0jWwBkYHh\n"
        "vP6UswJp7f5y/ptqpL17Wg8ccc//TBnEGOH27AF5gbwIfypwZbOEuJDTGR8r+g==\n"
        "-----END DH PARAMETERS-----\n";

    BIO* bio =
        CHECK_NOTNULL(BIO_new_mem_buf(const_cast<char*>(dh_ike_23_pem), -1));
    DH* dh =
        CHECK_NOTNULL(PEM_read_bio_DHparams(bio, nullptr, nullptr, nullptr));

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wold-style-cast"

    SSL_CTX_set_tmp_dh(ctx_, dh);

#pragma GCC diagnostic pop

    DH_free(dh);
    BIO_free(bio);

    SSL_CTX_set_tmp_rsa_callback(ctx_, rsa_callback);

    SSL_CTX_set_mode(ctx_, SSL_MODE_AUTO_RETRY);

    CHECK_EQ(1, SSL_CTX_set_cipher_list(ctx_, "!SSLv2:SSLv3:TLSv1"));
  }
  ~SockBuffer()
  {
    EVP_cleanup();
    // ??
    // SSL_CTX_free(ctx_);
  }
  bool input_ready(std::chrono::milliseconds wait) const
  {
    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(fd_in_, &rfds);

    struct timeval tv;
    tv.tv_sec = std::chrono::duration_cast<std::chrono::seconds>(wait).count();
    tv.tv_usec = (wait.count() % 1000) * 1000;

    int inputs;
    PCHECK((inputs = select(fd_in_ + 1, &rfds, nullptr, nullptr, &tv)) != -1);

    return 0 != inputs;
  }
  bool output_ready(std::chrono::milliseconds wait) const
  {
    fd_set wfds;
    FD_ZERO(&wfds);
    FD_SET(fd_out_, &wfds);

    struct timeval tv;
    tv.tv_sec = std::chrono::duration_cast<std::chrono::seconds>(wait).count();
    tv.tv_usec = (wait.count() % 1000) * 1000;

    int inputs;
    PCHECK((inputs = select(fd_out_ + 1, nullptr, &wfds, nullptr, &tv)) != -1);

    return 0 != inputs;
  }
  bool timed_out() const
  {
    return timed_out_;
  }
  std::streamsize read(char* s, std::streamsize n)
  {
    using namespace std::chrono;
    time_point<system_clock> start = system_clock::now();

    if (tls_) {
      int ssl_n_read;
      while ((ssl_n_read = SSL_read(ssl_, static_cast<void*>(s),
                                    static_cast<int>(n))) < 0) {
        time_point<system_clock> now = system_clock::now();
        if (now > (start + Config::read_timeout)) {
          LOG(ERROR) << "SSL_read timed out";
          timed_out_ = true;
          return static_cast<std::streamsize>(-1);
        }

        milliseconds t_o =
            duration_cast<milliseconds>((start + Config::read_timeout) - now);

        switch (SSL_get_error(ssl_, ssl_n_read)) {
        case SSL_ERROR_WANT_READ:
          if (input_ready(t_o))
            continue;
          timed_out_ = true;
          return static_cast<std::streamsize>(-1);

        case SSL_ERROR_WANT_WRITE:
          if (output_ready(t_o))
            continue;
          timed_out_ = true;
          return static_cast<std::streamsize>(-1);

        case SSL_ERROR_SYSCALL:
        case SSL_ERROR_SSL:
        default:
          ssl_error();
        }
      }

      if (ssl_n_read == 0) {
        switch (SSL_get_error(ssl_, ssl_n_read)) {
        case SSL_ERROR_NONE:
          LOG(INFO) << "SSL_read returned SSL_ERROR_NONE";
          break;

        case SSL_ERROR_ZERO_RETURN:
          // This is a close
          LOG(INFO) << "SSL_read returned SSL_ERROR_SSL";
          tls_ = false;
          break;

        case SSL_ERROR_SSL:
          LOG(INFO) << "SSL_read returned SSL_ERROR_SSL";
          ssl_error();
          break;

        default:
          LOG(INFO) << "SSL_read other error";
          ssl_error();
          break;
        }
      }

      return static_cast<std::streamsize>(ssl_n_read);
    }

    ssize_t n_read;
    while ((n_read = ::read(fd_in_, static_cast<void*>(s),
                            static_cast<size_t>(n))) < 0) {

      if ((errno == EWOULDBLOCK) || (errno == EAGAIN)) {

        time_point<system_clock> now = system_clock::now();
        if (now < (start + Config::read_timeout))
          if (input_ready(duration_cast<milliseconds>(
                  (start + Config::read_timeout) - now)))
            continue;

        timed_out_ = true;
        return static_cast<std::streamsize>(-1);
      }

      if (errno == EINTR)
        continue;

      throw io_error(errno);
    }

    if (0 == n_read)
      return static_cast<std::streamsize>(-1);

    return static_cast<std::streamsize>(n_read);
  }
  std::streamsize write(const char* s, std::streamsize n)
  {
    using namespace std::chrono;
    time_point<system_clock> start = system_clock::now();

    if (tls_) {
      int ssl_n_write;
      while ((ssl_n_write = SSL_write(ssl_, static_cast<const void*>(s),
                                      static_cast<size_t>(n))) < 0) {
        time_point<system_clock> now = system_clock::now();
        if (now > (start + Config::write_timeout)) {
          LOG(ERROR) << "SSL_write timed out";
          timed_out_ = true;
          return static_cast<std::streamsize>(-1);
        }

        milliseconds t_o =
            duration_cast<milliseconds>((start + Config::write_timeout) - now);

        switch (SSL_get_error(ssl_, ssl_n_write)) {
        case SSL_ERROR_WANT_READ:
          if (input_ready(t_o))
            continue;
          timed_out_ = true;
          return static_cast<std::streamsize>(-1);

        case SSL_ERROR_WANT_WRITE:
          if (output_ready(t_o))
            continue;
          timed_out_ = true;
          return static_cast<std::streamsize>(-1);

        case SSL_ERROR_SYSCALL:
        case SSL_ERROR_SSL:
        default:
          ssl_error();
        }
      }

      if (0 == ssl_n_write) {
        switch (SSL_get_error(ssl_, ssl_n_write)) {
        case SSL_ERROR_NONE:
          LOG(INFO) << "SSL_write returned SSL_ERROR_NONE";
          break;

        case SSL_ERROR_ZERO_RETURN:
          // This is a close
          tls_ = false;
          break;

        case SSL_ERROR_SSL:
          LOG(INFO) << "SSL_write returned SSL_ERROR_SSL";
          ssl_error();
          break;

        default:
          LOG(INFO) << "SSL_write other error";
          ssl_error();
          break;
        }
      }

      return static_cast<std::streamsize>(ssl_n_write);
    }

    ssize_t n_write;
    while ((n_write = ::write(fd_out_, static_cast<const void*>(s),
                              static_cast<size_t>(n))) < 0) {
      if ((errno == EWOULDBLOCK) || (errno == EAGAIN)) {

        time_point<system_clock> now = system_clock::now();
        if (now < (start + Config::write_timeout))
          if (output_ready(duration_cast<milliseconds>(
                  (start + Config::write_timeout) - now)))
            continue;

        timed_out_ = true;
        return static_cast<std::streamsize>(-1);
      }

      if (errno == EINTR)
        continue;

      throw io_error(errno);
    }

    if (0 == n_write)
      return static_cast<std::streamsize>(-1);

    return static_cast<std::streamsize>(n_write);
  }
  void starttls()
  {
    ssl_ = SSL_new(ctx_);
    if (!ssl_) {
      ssl_error();
    }
    if (!SSL_set_rfd(ssl_, fd_in_)) {
      ssl_error();
    }
    if (!SSL_set_wfd(ssl_, fd_out_)) {
      ssl_error();
    }

    using namespace std::chrono;
    time_point<system_clock> start = system_clock::now();

    int rc;
    while ((rc = SSL_accept(ssl_)) < 0) {

      time_point<system_clock> now = system_clock::now();
      if (now > (start + Config::starttls_timeout)) {
        LOG(ERROR) << "starttls timed out";
        return;
      }

      milliseconds t_o =
          duration_cast<milliseconds>((start + Config::starttls_timeout) - now);

      switch (SSL_get_error(ssl_, rc)) {
      case SSL_ERROR_WANT_READ:
        if (input_ready(t_o))
          continue;
        LOG(ERROR) << "starttls timed out on input_ready";
        return;

      case SSL_ERROR_WANT_WRITE:
        if (output_ready(t_o))
          continue;
        LOG(ERROR) << "starttls timed out on output_ready";
        continue;

      default:
        ssl_error();
      }
    }

    tls_ = true;
  }
  std::string tls_info()
  {
    std::ostringstream info;
    if (tls_) {
      SSL_CIPHER const* const c = SSL_get_current_cipher(ssl_);
      if (c) {
        info << "version=" << SSL_CIPHER_get_version(c);
        info << " cipher=" << SSL_CIPHER_get_name(c);
        int alg_bits;
        int bits = SSL_CIPHER_get_bits(c, &alg_bits);
        info << " bits=" << bits << "/" << alg_bits;
      }
    }
    return info.str();
  }
  bool tls()
  {
    return tls_;
  }

private:
  static void ssl_error()
  {
    unsigned long er;
    LOG(ERROR) << "SSL error";
    while (0 != (er = ERR_get_error()))
      LOG(ERROR) << ERR_error_string(er, nullptr);
    abort();
  }
  static RSA* rsa_callback(SSL* s, int ex, int keylength)
  {
    LOG(INFO) << "generating " << keylength << " bit RSA key";

    RSA* rsa_key = RSA_generate_key(keylength, RSA_F4, nullptr, nullptr);
    if (rsa_key == NULL) {
      static char ssl_errstring[256];
      ERR_error_string(ERR_get_error(), ssl_errstring);
      LOG(ERROR) << "TLS error (RSA_generate_key): " << ssl_errstring;
      return NULL;
    }
    return rsa_key;
  }

private:
  int fd_in_;
  int fd_out_;

  bool timed_out_;

  bool tls_;
  SSL_CTX* ctx_;
  SSL* ssl_;
};

#endif // SOCKBUFFER_DOT_HPP
