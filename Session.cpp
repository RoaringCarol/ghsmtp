#include <algorithm>
#include <iomanip>
#include <iostream>
#include <string>
#include <unordered_map>
#include <vector>

#include "DNS.hpp"
#include "Domain.hpp"
#include "IP.hpp"
#include "IP4.hpp"
#include "IP6.hpp"
#include "Message.hpp"
#include "SPF.hpp"
#include "Session.hpp"
#include "esc.hpp"
#include "iequal.hpp"
#include "osutil.hpp"

#include <boost/algorithm/string/case_conv.hpp>
#include <boost/algorithm/string/classification.hpp>
#include <boost/algorithm/string/split.hpp>

#include <syslog.h>

using namespace std::string_literals;

namespace Config {
constexpr char const* const rbls[]{
    "zen.spamhaus.org",
    "b.barracudacentral.org",
};

constexpr char const* const uribls[]{
    "dbl.spamhaus.org",
    "black.uribl.com",
    "multi.surbl.org",
};

constexpr auto greeting_wait = std::chrono::seconds{3};
constexpr auto max_recipients_per_message{100};
constexpr auto max_unrecognized_cmds{20};

// Read timeout value gleaned from RFC-1123 section 5.3.2 and RFC-5321
// section 4.5.3.2.7.
constexpr auto read_timeout = std::chrono::minutes{5};
constexpr auto write_timeout = std::chrono::seconds{30};
} // namespace Config

#include <gflags/gflags.h>

DEFINE_uint64(max_read, 0, "max data to read");
DEFINE_uint64(max_write, 0, "max data to write");

Session::Session(std::function<void(void)> read_hook, int fd_in, int fd_out)
  : sock_(fd_in, fd_out, read_hook, Config::read_timeout, Config::write_timeout)
{
  auto const our_id{[&] {
    auto const id_from_env{getenv("GHSMTP_SERVER_ID")};
    if (id_from_env)
      return std::string{id_from_env};

    auto const hostname{osutil::get_hostname()};
    if (hostname.find('.') != std::string::npos)
      return hostname;

    if (sock_.has_peername() && !IP::is_private(sock_.us_c_str()))
      return IP::to_address_literal(sock_.us_c_str());

    LOG(FATAL) << "can't determine my server ID";
  }()};

  server_identity_.set(our_id);

  max_msg_size(Config::max_msg_size_initial);
}

void Session::max_msg_size(size_t max)
{
  max_msg_size_ = max; // number to advertise via RFC 1870

  if (FLAGS_max_read) {
    sock_.set_max_read(FLAGS_max_read);
  }
  else {
    auto const overhead = std::max(max / 10, size_t(2048));
    sock_.set_max_read(max + overhead);
  }
}

void Session::reset_()
{
  // RSET does not force another EHLO/HELO, one piece of transaction
  // data saved is client_identity_:

  // client_identity_.clear();

  reverse_path_.clear();
  forward_path_.clear();
  received_spf_.clear();

  binarymime_ = false;
  smtputf8_ = false;

  if (msg_) {
    msg_.reset();
  }

  max_msg_size(max_msg_size());

  state_ = xact_step::mail;
}

// Return codes from connection establishment are 220 or 554, according
// to RFC 5321.  That's it.

void Session::greeting()
{
  CHECK(state_ == xact_step::helo);

  if (sock_.has_peername()) {
    auto error_msg{std::string{}};
    if (!verify_ip_address_(error_msg)) {
      // no glog message at this point
      syslog(LOG_MAIL | LOG_WARNING, "bad host [%s] %s", sock_.them_c_str(),
             error_msg.c_str());
      std::exit(EXIT_SUCCESS);
    }

    // Wait a bit of time for pre-greeting traffic.
    if (!(ip_whitelisted_ || fcrdns_whitelisted_)
        && sock_.input_ready(Config::greeting_wait)) {
      out_() << "554 5.3.2 not accepting network messages\r\n" << std::flush;
      // no glog message at this point
      syslog(LOG_MAIL | LOG_WARNING, "bad host [%s] input before greeting",
             sock_.them_c_str());
      std::exit(EXIT_SUCCESS);
    }
  }

  out_() << "220 " << server_id_() << " ESMTP - ghsmtp\r\n" << std::flush;
}

void Session::log_lo_(char const* verb, std::string_view client_identity) const
{
  if (sock_.has_peername()) {
    if (client_fcrdns_ == client_identity_) {
      LOG(INFO) << verb << " " << client_identity << " from "
                << sock_.them_address_literal();
    }
    else {
      LOG(INFO) << verb << " " << client_identity << " from " << client_;
    }
  }
  else {
    LOG(INFO) << verb << " " << client_identity;
  }
}

void Session::flush() { out_() << std::flush; }

void Session::last_in_group_(std::string_view verb)
{
  if (sock_.input_ready(std::chrono::seconds(0))) {
    LOG(WARNING) << "pipelining error; input ready processing " << verb;
  }
}

void Session::ehlo(std::string_view client_identity)
{
  auto constexpr verb{"EHLO"};

  last_in_group_(verb);
  reset_();
  extensions_ = true;

  if (client_identity_ != client_identity) {
    client_identity_.set(client_identity);
    verify_client_();
  }

  out_() << "250-" << server_id_();
  if (sock_.has_peername()) {
    out_() << " at your service, " << client_;
  }
  out_() << "\r\n";

  // RFC 1870
  out_() << "250-SIZE " << max_msg_size() << "\r\n";
  // RFC 6152
  out_() << "250-8BITMIME\r\n";

  if (sock_.tls()) {
    // Check sasl sources for auth types.
    // out_() << "250-AUTH PLAIN\r\n";
  }
  else {
    // If we're not already TLS, offer TLS, à la RFC 3207
    out_() << "250-STARTTLS\r\n";
  }

  // RFC 2034
  out_() << "250-ENHANCEDSTATUSCODES\r\n";
  // RFC 2920
  out_() << "250-PIPELINING\r\n";
  // RFC 3030
  out_() << "250-BINARYMIME\r\n"
            "250-CHUNKING\r\n";
  // RFC 6531
  out_() << "250 SMTPUTF8\r\n" << std::flush;

  log_lo_(verb, client_identity);
}

void Session::helo(std::string_view client_identity)
{
  auto constexpr verb{"HELO"};

  last_in_group_(verb);
  reset_();
  extensions_ = false;

  if (client_identity_ != client_identity) {
    client_identity_.set(client_identity);
    verify_client_();
  }

  out_() << "250 " << server_id_() << "\r\n" << std::flush;

  log_lo_(verb, client_identity);
}

void Session::mail_from(Mailbox&& reverse_path, parameters_t const& parameters)
{
  switch (state_) {
  case xact_step::helo:
    out_() << "503 5.5.1 must send HELO/EHLO first\r\n" << std::flush;
    LOG(WARNING) << "'MAIL FROM' before HELO/EHLO"
                 << (sock_.has_peername() ? " from " : "") << client_;
    return;
  case xact_step::mail:
    break;
  case xact_step::rcpt:
  case xact_step::data:
  case xact_step::bdat:
    out_() << "503 5.5.1 nested MAIL command\r\n" << std::flush;
    LOG(WARNING) << "nested MAIL command"
                 << (sock_.has_peername() ? " from " : "") << client_;
    return;
  }

  if (!verify_from_params_(parameters)) {
    return;
  }

  auto params{std::ostringstream{}};
  for (auto const& [name, value] : parameters) {
    params << " " << name << (value.empty() ? "" : "=") << value;
  }

  auto error_msg{std::string{}};
  if (!verify_sender_(reverse_path, error_msg)) {
    LOG(WARNING) << "verify sender " << reverse_path << params.str()
                 << " failed: " << error_msg;
    if (sock_.has_peername()) {
      syslog(LOG_MAIL | LOG_WARNING, "bad host [%s] %s", sock_.them_c_str(),
             error_msg.c_str());
    }
    std::exit(EXIT_SUCCESS);
  }

  reverse_path_ = std::move(reverse_path);
  forward_path_.clear();
  out_() << "250 2.1.0 MAIL FROM OK\r\n";
  // No flush RFC-2920 section 3.1, this could be part of a command group.
  LOG(INFO) << "MAIL FROM:<" << reverse_path_ << ">" << params.str();

  state_ = xact_step::rcpt;
}

void Session::rcpt_to(Mailbox&& forward_path, parameters_t const& parameters)
{
  switch (state_) {
  case xact_step::helo:
    out_() << "503 5.5.1 must send HELO/EHLO first\r\n" << std::flush;
    LOG(WARNING) << "'RCPT TO' before HELO/EHLO"
                 << (sock_.has_peername() ? " from " : "") << client_;
    return;
  case xact_step::mail:
    out_() << "503 5.5.1 must send MAIL FROM before RCPT TO\r\n" << std::flush;
    LOG(WARNING) << "'RCPT TO' before 'MAIL FROM'"
                 << (sock_.has_peername() ? " from " : "") << client_;
    return;
  case xact_step::rcpt:
  case xact_step::data:
    break;
  case xact_step::bdat:
    out_() << "503 5.5.1 sequence error, expecting BDAT" << std::flush;
    LOG(WARNING) << "'RCPT TO' during BDAT transfer"
                 << (sock_.has_peername() ? " from " : "") << client_;
    return;
  }

  // Take a look at the optional parameters, we don't accept any:
  for (auto const& [name, value] : parameters) {
    LOG(WARNING) << "unrecognized 'RCPT TO' parameter " << name << "=" << value;
  }

  if (!verify_recipient_(forward_path))
    return;

  if (forward_path_.size() >= Config::max_recipients_per_message) {
    out_() << "452 4.5.3 too many recipients\r\n" << std::flush;
    LOG(WARNING) << "too many recipients <" << forward_path << ">";
    return;
  }

  // no check for dups, postfix doesn't
  forward_path_.push_back(std::move(forward_path));
  out_() << "250 2.1.5 RCPT TO OK\r\n";
  // No flush RFC-2920 section 3.1, this could be part of a command group.
  LOG(INFO) << "RCPT TO:<" << forward_path_.back() << ">";

  state_ = xact_step::data;
}

std::string Session::added_headers_(Message const& msg)
{
  // The headers Return-Path, Received-SPF, and Received are returned
  // as a string.
  auto const protocol{[&]() {
    if (smtputf8_)
      return sock_.tls() ? "UTF8SMTPS" : "UTF8SMTP";
    else if (extensions_)
      return sock_.tls() ? "ESMTPS" : "ESMTP";
    else
      return sock_.tls() ? "SMTPS" : "SMTP";
  }()};

  auto headers{std::ostringstream{}};
  headers << "Return-Path: <" << reverse_path_ << ">\r\n";
  // Received-SPF:
  if (!received_spf_.empty()) {
    headers << received_spf_ << "\r\n";
  }

  // STD 3 section 5.2.8

  headers << "Received: from " << client_identity_.utf8();
  if (sock_.has_peername()) {
    headers << " (" << client_ << ')';
  }
  headers << "\r\n        by " << server_identity_.utf8() << " with "
          << protocol << " id " << msg.id();

  if (forward_path_.size()) {
    auto len{12};
    headers << "\r\n        for ";
    for (size_t i = 0; i < forward_path_.size(); ++i) {
      auto fwd{std::string{forward_path_[i]}};
      if (i) {
        headers << ',';
        ++len;
      }
      if ((len + fwd.length() + 2) > 80) {
        headers << "\r\n        ";
        len = 8;
      }
      headers << '<' << fwd << '>';
      len += fwd.length() + 2;
    }
  }

  const std::string tls_info{sock_.tls_info()};
  if (tls_info.length()) {
    headers << "\r\n        (" << tls_info << ')';
  }
  headers << ";\r\n        " << msg.when() << "\r\n";

  return headers.str();
}

bool lookup_domain(CDB& cdb, Domain const& domain)
{
  if (!domain.empty()) {
    if (cdb.lookup(domain.lc())) {
      return true;
    }
    if (domain.is_unicode() && cdb.lookup(domain.utf8())) {
      return true;
    }
  }
  return false;
}

bool Session::msg_new()
{
  CHECK((state_ == xact_step::data) || (state_ == xact_step::bdat));

  auto const status{[&] {
    // Anything enciphered tastes a lot like ham.
    if (sock_.tls())
      return Message::SpamStatus::ham;

    // I will allow this as sort of the gold standard for naming.
    if (client_identity_ == client_fcrdns_)
      return Message::SpamStatus::ham;

    if (fcrdns_whitelisted_)
      return Message::SpamStatus::ham;

    if (lookup_domain(white_, client_identity_))
      return Message::SpamStatus::ham;

    auto tld_id{tld_db_.get_registered_domain(client_identity_.lc())};
    if (tld_id && white_.lookup(tld_id))
      return Message::SpamStatus::ham;

    auto rp_dom = reverse_path_.domain();
    if (lookup_domain(white_, rp_dom))
      return Message::SpamStatus::ham;

    auto tld_rp{tld_db_.get_registered_domain(rp_dom.lc())};
    if (tld_rp && white_.lookup(tld_rp))
      return Message::SpamStatus::ham;

    return Message::SpamStatus::spam;
  }()};

  // All sources of ham get a fresh 5 minute timeout per message.
  if (status == Message::SpamStatus::ham) {
    alarm(5 * 60);
  }

  msg_ = std::make_unique<Message>();

  if (!FLAGS_max_write)
    FLAGS_max_write = max_msg_size();

  msg_->open(server_id_(), FLAGS_max_write, status);
  auto const hdrs{added_headers_(*(msg_.get()))};
  msg_->write(hdrs);

  return true;
}

bool Session::msg_data(char const* s, std::streamsize count)
{
  if ((state_ != xact_step::data) && (state_ != xact_step::bdat))
    return false;

  if (!msg_)
    return false;

  if (msg_->write(s, count))
    return !msg_->size_error();

  return false;
}

bool Session::data_start()
{
  last_in_group_("DATA");

  switch (state_) {
  case xact_step::helo:
    out_() << "503 5.5.1 must send HELO/EHLO first\r\n" << std::flush;
    LOG(WARNING) << "'DATA' before HELO/EHLO"
                 << (sock_.has_peername() ? " from " : "") << client_;
    return false;
  case xact_step::mail:
    out_() << "503 5.5.1 must send 'MAIL FROM' before DATA\r\n" << std::flush;
    LOG(WARNING) << "'DATA' before 'MAIL FROM'"
                 << (sock_.has_peername() ? " from " : "") << client_;
    return false;
  case xact_step::rcpt:
    out_() << "554 5.5.1 no valid recipients\r\n" << std::flush;
    LOG(WARNING) << "no valid recipients"
                 << (sock_.has_peername() ? " from " : "") << client_;
    return false;
  case xact_step::data:
    break;
  case xact_step::bdat:
    out_() << "503 5.5.1 sequence error, expecting BDAT" << std::flush;
    LOG(WARNING) << "'DATA' during BDAT transfer"
                 << (sock_.has_peername() ? " from " : "") << client_;
    return false;
  }

  if (binarymime_) {
    out_() << "503 5.5.1 DATA does not support BINARYMIME\r\n" << std::flush;
    LOG(WARNING) << "DATA does not support BINARYMIME";
    return false;
  }
  // for bounce messages
  // CHECK(!reverse_path_.empty());
  CHECK(!forward_path_.empty());

  if (!msg_new()) {
    out_() << "451 4.3.1 mail system full\r\n" << std::flush;
    LOG(ERROR) << "msg_new() failed";
    return false;
  }

  out_() << "354 go, end with <CR><LF>.<CR><LF>\r\n" << std::flush;
  LOG(INFO) << "DATA";
  return true;
}

void Session::data_done()
{
  CHECK((state_ == xact_step::data));

  CHECK(msg_);
  msg_->save();

  out_() << "250 2.0.0 DATA OK\r\n" << std::flush;
  LOG(INFO) << "message delivered, " << msg_->size() << " octets, with id "
            << msg_->id();

  reset_();
}

void Session::data_size_error()
{
  out_().clear(); // clear possible eof from input side
  out_() << "552 5.3.4 message size limit exceeded\r\n" << std::flush;
  if (msg_) {
    msg_->trash();
  }
  LOG(WARNING) << "DATA size error";
  reset_();
}

void Session::data_error()
{
  out_().clear(); // clear possible eof from input side
  out_() << "552 5.3.4 message error of some kind\r\n" << std::flush;
  if (msg_) {
    msg_->trash();
  }
  LOG(WARNING) << "DATA error";
  reset_();
}

bool Session::bdat_start(size_t n)
{
  switch (state_) {
  case xact_step::helo:
    out_() << "503 5.5.1 must send HELO/EHLO first\r\n" << std::flush;
    LOG(WARNING) << "'BDAT' before HELO/EHLO"
                 << (sock_.has_peername() ? " from " : "") << client_;
    return false;
  case xact_step::mail:
    out_() << "503 5.5.1 must send 'MAIL FROM' before BDAT\r\n" << std::flush;
    LOG(WARNING) << "'BDAT' before 'MAIL FROM'"
                 << (sock_.has_peername() ? " from " : "") << client_;
    return false;
  case xact_step::rcpt:
    out_() << "554 5.5.1 no valid recipients\r\n" << std::flush;
    LOG(WARNING) << "no valid recipients"
                 << (sock_.has_peername() ? " from " : "") << client_;
    return false;
  case xact_step::data: // first bdat
    break;
  case xact_step::bdat:
    return true;
  }

  CHECK(!forward_path_.empty());

  state_ = xact_step::bdat;

  return msg_new();
}

void Session::bdat_done(size_t n, bool last)
{
  if (state_ != xact_step::bdat)
    return;

  if (msg_ && msg_->size_error()) {
    bdat_size_error();
    return;
  }

  if (!last) {
    out_() << "250 2.0.0 BDAT " << n << " OK\r\n" << std::flush;
    LOG(INFO) << "BDAT " << n;
    return;
  }

  CHECK(msg_);
  msg_->save();

  out_() << "250 2.0.0 BDAT " << n << " LAST OK\r\n" << std::flush;

  LOG(INFO) << "BDAT " << n << " LAST";
  LOG(INFO) << "message delivered, " << msg_->size() << " octets, with id "
            << msg_->id();
  reset_();
}

void Session::bdat_size_error()
{
  out_().clear(); // clear possible eof from input side
  out_() << "552 5.3.4 message size limit exceeded\r\n" << std::flush;
  LOG(WARNING) << "BDAT size error";
  if (msg_) {
    msg_->trash();
  }
  reset_();
}

void Session::bdat_error()
{
  out_().clear(); // clear possible eof from input side
  out_() << "503 5.5.1 BDAT sequence error\r\n" << std::flush;
  LOG(WARNING) << "BDAT sequence error";
  if (msg_) {
    msg_->trash();
  }
  reset_();
}

void Session::rset()
{
  out_() << "250 2.1.5 RSET OK\r\n";
  // No flush RFC-2920 section 3.1, this could be part of a command group.
  LOG(INFO) << "RSET";
  reset_();
}

void Session::noop(std::string_view str)
{
  last_in_group_("NOOP");
  out_() << "250 2.0.0 NOOP OK\r\n" << std::flush;
  LOG(INFO) << "NOOP" << (str.length() ? " " : "") << str;
}

void Session::vrfy(std::string_view str)
{
  last_in_group_("VRFY");
  out_() << "252 2.1.5 try it\r\n" << std::flush;
  LOG(INFO) << "VRFY" << (str.length() ? " " : "") << str;
}

void Session::help(std::string_view str)
{
  out_() << "214 2.0.0 see https://digilicious.com/smtp.html\r\n" << std::flush;
  LOG(INFO) << "HELP" << (str.length() ? " " : "") << str;
}

void Session::quit()
{
  last_in_group_("QUIT");
  out_() << "221 2.0.0 closing connection\r\n" << std::flush;
  LOG(INFO) << "QUIT";
  exit_();
}

void Session::auth()
{
  if (sock_.has_peername()) {
    syslog(LOG_MAIL | LOG_WARNING, "bad host [%s] auth", sock_.them_c_str());
  }
  out_() << "454 4.7.0 authentication failure\r\n" << std::flush;
  std::exit(EXIT_SUCCESS);
}

void Session::error(std::string_view log_msg)
{
  out_() << "421 4.3.5 system error\r\n" << std::flush;
  LOG(WARNING) << log_msg;
}

void Session::cmd_unrecognized(std::string_view cmd)
{
  auto const escaped{esc(cmd)};
  LOG(WARNING) << "command unrecognized: \"" << escaped << "\"";

  if (++n_unrecognized_cmds_ >= Config::max_unrecognized_cmds) {
    out_() << "500 5.5.1 command unrecognized: \"" << escaped
           << "\" exceeds limit\r\n"
           << std::flush;
    LOG(WARNING) << n_unrecognized_cmds_
                 << " unrecognized commands is too many";
    exit_();
  }

  out_() << "500 5.5.1 command unrecognized: \"" << escaped << "\"\r\n"
         << std::flush;
}

void Session::bare_lf()
{
  // Error code used by Office 365.
  out_() << "554 5.6.11 bare LF\r\n" << std::flush;
  LOG(WARNING) << "bare LF";
  exit_();
}

void Session::max_out()
{
  out_() << "552 5.3.4 message size exceeds maximium size\r\n" << std::flush;
  LOG(WARNING) << "message size maxed out";
  exit_();
}

void Session::time_out()
{
  out_() << "421 4.4.2 time-out\r\n" << std::flush;
  LOG(WARNING) << "time-out" << (sock_.has_peername() ? " from " : "")
               << client_;
  exit_();
}

void Session::starttls()
{
  last_in_group_("STARTTLS");
  if (sock_.tls()) {
    out_() << "554 5.5.1 TLS already active\r\n" << std::flush;
    LOG(WARNING) << "STARTTLS issued with TLS already active";
  }
  else {
    out_() << "220 2.0.0 STARTTLS OK\r\n" << std::flush;
    if (sock_.starttls_server()) {
      reset_();
      // max_msg_size(Config::max_msg_size_bro);
      LOG(INFO) << "STARTTLS " << sock_.tls_info();
    }
  }
}

void Session::exit_()
{
  sock_.log_totals();

  auto time_used{timespec{}};
  clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &time_used);

  LOG(INFO) << "CPU time " << time_used.tv_sec << "." << std::setw(9)
            << std::setfill('0') << time_used.tv_nsec << " seconds";

  std::exit(EXIT_SUCCESS);
}

/////////////////////////////////////////////////////////////////////////////

// All of the verify_* functions send their own error messages back to
// the client on failure, and return false.

bool Session::verify_ip_address_(std::string& error_msg)
{
  CDB ip_black{"ip-black"};
  if (ip_black.lookup(sock_.them_c_str())) {
    error_msg = "IP on static blacklist";
    out_() << "554 5.7.1 blacklisted\r\n" << std::flush;
    return false;
  }

  client_fcrdns_ = IP::fcrdns(sock_.them_c_str());
  if (!client_fcrdns_.empty()) {
    client_ = client_fcrdns_.ascii() + " " + sock_.them_address_literal();

    if (black_.lookup(client_fcrdns_.lc())) {
      error_msg = "FCrDNS "s + client_fcrdns_.lc() + " on static blacklist"s;
      out_() << "554 5.7.1 blacklisted\r\n" << std::flush;
      return false;
    }
    if (white_.lookup(client_fcrdns_.lc())) {
      LOG(INFO) << "FCrDNS domain " << client_fcrdns_.lc() << " whitelisted";
      fcrdns_whitelisted_ = true;
    }

    auto const tld{tld_db_.get_registered_domain(client_fcrdns_.lc())};
    if (tld) {
      if (black_.lookup(tld)) {
        error_msg = "FCrDNS domain "s + tld + " on static blacklist"s;
        out_() << "554 5.7.1 blacklisted\r\n" << std::flush;
        return false;
      }
      if (white_.lookup(tld)) {
        LOG(INFO) << "FCrDNS TLD domain " << tld << " whitelisted";
        fcrdns_whitelisted_ = true;
      }
    }
  }
  else {
    client_ = "unknown "s + sock_.them_address_literal();
  }

  if ((sock_.them_address_literal() == IP4::loopback_literal)
      || (sock_.them_address_literal() == IP6::loopback_literal)) {
    ip_whitelisted_ = true;
  }
  else {
    CDB ip_white{"ip-white"};
    if (ip_white.lookup(sock_.them_c_str())) {
      LOG(INFO) << "IP address " << sock_.them_c_str() << " whitelisted";
      ip_whitelisted_ = true;
    }
    else if (IP4::is_address(sock_.them_c_str())) {
      using namespace DNS;

      // Check with black hole lists. <https://en.wikipedia.org/wiki/DNSBL>
      auto const reversed{IP4::reverse(sock_.them_c_str())};
      auto const res{DNS::Resolver{}};
      for (auto rbl : Config::rbls) {
        if (has_record<RR_type::A>(res, reversed + rbl)) {
          error_msg = "blocked by "s + rbl;
          out_() << "554 5.7.1 blocked on advice from " << rbl << "\r\n"
                 << std::flush;
          return false;
        }
      }
      // LOG(INFO) << "IP address " << sock_.them_c_str() << " not
      // blacklisted";
    }
  }

  return true;
}

// check the identity from HELO/EHLO
void Session::verify_client_()
{
  auto error_msg{std::string{}};
  if (!verify_client_(client_identity_, error_msg)) {
    LOG(WARNING) << "verify client failed: " << error_msg;
    if (sock_.has_peername()) {
      syslog(LOG_MAIL | LOG_WARNING, "bad host [%s] EHLO failed: %s",
             sock_.them_c_str(), error_msg.c_str());
    }
    std::exit(EXIT_SUCCESS);
  }
}

bool Session::verify_client_(Domain const& client_identity,
                             std::string& error_msg)
{
  if (!sock_.has_peername() || ip_whitelisted_ || fcrdns_whitelisted_
      || client_identity.is_address_literal()) {
    return true;
  }

  // Bogus clients claim to be us or some local host.
  if ((client_identity == server_identity_) || (client_identity == "localhost")
      || (client_identity == "localhost.localdomain")) {

    if (server_identity_ != client_fcrdns_) {
      error_msg = "liar, claimed to be us";
      out_() << "550 5.7.1 liar\r\n" << std::flush;
      return false;
    }
  }

  auto labels{std::vector<std::string>{}};
  boost::algorithm::split(labels, client_identity.lc(),
                          boost::algorithm::is_any_of("."));
  if (labels.size() < 2) {
    // Sometimes we may want to look at mail from misconfigured
    // sending systems.
    LOG(WARNING) << "invalid sender" << (sock_.has_peername() ? " " : "")
                 << client_ << " claiming " << client_identity;
    return true;
  }

  if (lookup_domain(black_, client_identity)) {
    error_msg = "claimed blacklisted identity";
    out_() << "550 4.7.1 blacklisted identity\r\n" << std::flush;
    return false;
  }

  auto const tld{tld_db_.get_registered_domain(client_identity.lc())};
  if (!tld) {
    // Sometimes we may want to look at mail from misconfigured
    // sending systems.
    LOG(WARNING) << "claimed identity has to TLD";
    return true;
  }
  if (black_.lookup(tld)) {
    error_msg = "claimed identity has blacklisted TLD "s + tld;
    out_() << "550 4.7.1 blacklisted TLD\r\n" << std::flush;
    return false;
  }

  return true;
}

// check recipient from RCPT TO
bool Session::verify_recipient_(Mailbox const& recipient)
{
  if ((recipient.local_part() == "Postmaster") && (recipient.domain() == "")) {
    LOG(INFO) << "magic Postmaster address";
    return true;
  }

  auto const accepted_domain{[&] {
    if (recipient.domain().is_address_literal()) {
      if (recipient.domain() == sock_.us_address_literal()) {
        return true;
      }
    }
    else {
      // Domains we accept mail for.
      CDB accept_domains{"accept_domains"};
      if (accept_domains.is_open()) {
        if (accept_domains.lookup(recipient.domain().ascii())
            || accept_domains.lookup(recipient.domain().utf8())) {
          return true;
        }
      }
      else {
        // If we have no list of domains to accept, take our own.
        if (recipient.domain() == server_identity_) {
          return true;
        }
      }
    }
    return false;
  }()};

  if (!accepted_domain) {
    out_() << "554 5.7.1 relay access denied\r\n" << std::flush;
    LOG(WARNING) << "relay access denied for domain " << recipient.domain();
    return false;
  }

  // Check for local addresses we reject.
  CDB bad_recipients{"bad_recipients"};
  if (bad_recipients.lookup(recipient.local_part())) {
    out_() << "550 5.1.1 bad recipient " << recipient << "\r\n" << std::flush;
    LOG(WARNING) << "bad recipient " << recipient;
    return false;
  }

  return true;
}

// the sender is the RFC5321 MAIL FROM:
bool Session::verify_sender_(Mailbox const& sender, std::string& error_msg)
{
  auto const sender_str{std::string{sender}};
  CDB bad_senders{"bad_senders"}; // Addresses we don't accept mail from.
  if (bad_senders.lookup(sender_str)) {
    out_() << "550 5.1.8 bad sender\r\n" << std::flush;
    error_msg = "bad sender";
    return false;
  }

  if (sender.domain().is_address_literal()) {
    if (sender.domain() != sock_.them_address_literal()) {
      LOG(WARNING) << "sender domain " << sender.domain() << " does not match "
                   << sock_.them_address_literal();
    }
    return true;
  }

  // If the reverse path domain matches the Forward-confirmed reverse
  // DNS of the sending IP address, we skip the uribl check.
  if (sender.domain() == client_fcrdns_) {
    LOG(INFO) << "MAIL FROM: domain matches sender's FCrDNS";
  }
  else if (!verify_sender_domain_(sender.domain(), error_msg)) {
    return false;
  }

  if (!verify_sender_spf_(sender)) {
    error_msg = "failed SPF check";
    return false;
  }

  return true;
}

// this sender is the RFC5321 MAIL FROM: domain part
bool Session::verify_sender_domain_(Domain const& sender,
                                    std::string& error_msg)
{
  if (sender.empty()) {
    // MAIL FROM:<>
    // is used to send bounce messages.
    return true;
  }

  if (white_.lookup(sender.lc())) {
    LOG(INFO) << "sender \"" << sender.lc() << "\" whitelisted";
    return true;
  }

  // Break sender domain into labels:

  auto labels{std::vector<std::string>{}};
  boost::algorithm::split(labels, sender.lc(),
                          boost::algorithm::is_any_of("."));

  if (labels.size() < 2) { // This is not a valid domain.
    out_() << "550 5.7.1 invalid sender domain " << sender << "\r\n"
           << std::flush;
    error_msg = "invalid syntax";
    return false;
  }

  auto reg_dom{tld_db_.get_registered_domain(sender.lc())};
  if (!reg_dom) {
    error_msg = "no registered domain";
    return false;
  }
  if (white_.lookup(reg_dom)) {
    LOG(INFO) << "sender registered domain \"" << reg_dom << "\" whitelisted";
    return true;
  }

  // Based on <http://www.surbl.org/guidelines>

  auto two_level{labels[labels.size() - 2] + "." + labels[labels.size() - 1]};

  if (labels.size() > 2) {
    auto three_level{labels[labels.size() - 3] + "." + two_level};

    CDB three_tld{"three-level-tlds"};
    if (three_tld.lookup(reg_dom)) {
      LOG(INFO) << reg_dom << " found on the three level list";
      if (labels.size() > 3) {
        auto look_up = labels[labels.size() - 4] + "." + three_level;
        LOG(INFO) << "looking up " << look_up;
        return verify_sender_domain_uribl_(look_up, error_msg);
      }
      else {
        out_() << "554 5.7.1 bad sender domain\r\n" << std::flush;
        error_msg = "blocked by exact match on three-level-tlds list";
        return false;
      }
    }
  }

  CDB two_tld{"two-level-tlds"};
  if (two_tld.lookup(reg_dom)) {
    LOG(INFO) << reg_dom << " found on the two level list";
    if (labels.size() > 2) {
      auto look_up = labels[labels.size() - 3] + "." + two_level;
      LOG(INFO) << "looking up " << look_up;
      return verify_sender_domain_uribl_(look_up, error_msg);
    }
    else {
      out_() << "554 5.7.1 bad sender domain\r\n" << std::flush;
      error_msg = "blocked by exact match on two-level-tlds list";
      return false;
    }
  }

  if (two_level.compare(reg_dom)) {
    LOG(INFO) << "two level '" << two_level << "' != registered domain '"
              << reg_dom << "'";
  }

  LOG(INFO) << "looking up " << two_level;
  return verify_sender_domain_uribl_(two_level, error_msg);
}

// check sender domain on dynamic URI black lists
bool Session::verify_sender_domain_uribl_(std::string const& sender,
                                          std::string& error_msg)
{
  if (!sock_.has_peername()) // short circuit
    return true;

  auto res{DNS::Resolver{}};
  for (auto uribl : Config::uribls) {
    if (DNS::has_record<DNS::RR_type::A>(res, (sender + ".") + uribl)) {
      out_() << "554 5.7.1 sender blocked on advice of " << uribl << "\r\n"
             << std::flush;
      error_msg = "blocked by "s + uribl;
      return false;
    }
  }

  LOG(INFO) << sender << " cleared by URIBLs";
  return true;
}

bool Session::verify_sender_spf_(Mailbox const& sender)
{
  auto const srvr_id{server_id_()};

  if (!sock_.has_peername() || ip_whitelisted_) {
    auto ip_addr = sock_.them_c_str();
    if (!sock_.has_peername()) {
      ip_addr = "127.0.0.1"; // use localhost for local socket
    }
    auto received_spf{std::ostringstream{}};
    received_spf << "Received-SPF: pass (" << srvr_id << ": " << ip_addr
                 << " is whitelisted.) client-ip=" << ip_addr
                 << "; envelope-from=" << sender
                 << "; helo=" << client_identity_ << ";";
    received_spf_ = received_spf.str();
    return true;
  }

  auto const sid{std::string(srvr_id.data(), srvr_id.length())};
  auto const spf_srv{SPF::Server{sid.c_str()}};
  auto spf_req{SPF::Request{spf_srv}};

  if (IP4::is_address(sock_.them_c_str())) {
    spf_req.set_ipv4_str(sock_.them_c_str());
  }
  else if (IP6::is_address(sock_.them_c_str())) {
    spf_req.set_ipv6_str(sock_.them_c_str());
  }
  else {
    LOG(FATAL) << "bogus address " << sock_.them_address_literal() << ", "
               << sock_.them_c_str();
  }

  spf_req.set_helo_dom(client_identity_.ascii().c_str());

  auto const from{static_cast<std::string>(sender)};

  spf_req.set_env_from(from.c_str());

  auto const spf_res{SPF::Response{spf_req}};

  if (spf_res.result() == SPF::Result::FAIL) {
    // Error code from RFC 7372, section 3.2.  Also:
    // <https://www.iana.org/assignments/smtp-enhanced-status-codes/smtp-enhanced-status-codes.xhtml>
    out_() << "550 5.7.23 " << spf_res.smtp_comment() << "\r\n" << std::flush;
    LOG(WARNING) << spf_res.header_comment();
    return false;
  }

  LOG(INFO) << spf_res.header_comment();
  received_spf_ = spf_res.received_spf();
  return true;
}

bool Session::verify_from_params_(parameters_t const& parameters)
{
  // Take a look at the optional parameters:
  for (auto const& [name, val] : parameters) {
    if (iequal(name, "BODY")) {
      if (iequal(val, "8BITMIME")) {
        // everything is cool, this is our default...
      }
      else if (iequal(val, "7BIT")) {
        // nothing to see here, move along...
      }
      else if (iequal(val, "BINARYMIME")) {
        binarymime_ = true;
      }
      else {
        LOG(WARNING) << "unrecognized BODY type \"" << val << "\" requested";
      }
    }
    else if (iequal(name, "SMTPUTF8")) {
      if (!val.empty()) {
        LOG(WARNING) << "SMTPUTF8 parameter has a value: " << val;
      }
      smtputf8_ = true;
    }
    else if (iequal(name, "SIZE")) {
      if (val.empty()) {
        LOG(WARNING) << "SIZE parameter has no value.";
      }
      else {
        try {
          auto const sz = stoull(val);
          if (sz > max_msg_size()) {
            out_() << "552 5.3.4 message size exceeds maximium size\r\n"
                   << std::flush;
            LOG(WARNING) << "SIZE parameter too large: " << sz;
            return false;
          }
        }
        catch (std::invalid_argument const& e) {
          LOG(WARNING) << "SIZE parameter has invalid value: " << val;
        }
        catch (std::out_of_range const& e) {
          LOG(WARNING) << "SIZE parameter has out-of-range value: " << val;
        }
        // I guess we just ignore bad size parameters.
      }
    }
    else {
      LOG(WARNING) << "unrecognized MAIL FROM parameter " << name << "=" << val;
    }
  }

  return true;
}
