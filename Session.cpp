#include <algorithm>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

#include "DNS.hpp"
#include "Domain.hpp"
#include "IP.hpp"
#include "IP4.hpp"
#include "IP6.hpp"
#include "Message.hpp"
#include "Session.hpp"
#include "esc.hpp"
#include "iequal.hpp"
#include "osutil.hpp"

#include <fmt/format.h>
#include <fmt/ostream.h>

#include <boost/algorithm/string/classification.hpp>
#include <boost/algorithm/string/split.hpp>

#include <syslog.h>

namespace Config {
char const* rbls[]{
    "b.barracudacentral.org",
    "psbl.surriel.com",
    "zen.spamhaus.org",
};

/*** Last octet from A record returned by blacklists ***

From <http://uribl.com/about.shtml#implementation>

X   Binary    On List
---------------------------------------------------------
1   00000001  Query blocked, possibly due to high volume
2   00000010  black
4   00000100  grey
8   00001000  red
14  00001110  black,grey,red (for testpoints)

From <https://www.spamhaus.org/faq/section/Spamhaus%20DBL#291>

Return Codes 	Data Source
127.0.1.2 	spam domain
127.0.1.4 	phish domain
127.0.1.5 	malware domain
127.0.1.6 	botnet C&C domain
127.0.1.102 	abused legit spam
127.0.1.103 	abused spammed redirector domain
127.0.1.104 	abused legit phish
127.0.1.105 	abused legit malware
127.0.1.106 	abused legit botnet C&C
127.0.1.255 	IP queries prohibited!

From <http://www.surbl.org/lists#multi>

last octet indicates which lists it belongs to. The bit positions in
that last octet for membership in the different lists are:

  8 = listed on PH
 16 = listed on MW
 64 = listed on ABUSE
128 = listed on CR

*/

char const* uribls[]{
    "multi.uribl.com",
    "dbl.spamhaus.org",
    "multi.surbl.org",
};

constexpr auto greeting_wait              = std::chrono::seconds{2};
constexpr int  max_recipients_per_message = 100;
constexpr int  max_unrecognized_cmds      = 20;

// Read timeout value gleaned from RFC-1123 section 5.3.2 and RFC-5321
// section 4.5.3.2.7.
constexpr auto read_timeout  = std::chrono::minutes{5};
constexpr auto write_timeout = std::chrono::seconds{30};
} // namespace Config

#include <gflags/gflags.h>

DEFINE_uint64(max_read, 0, "max data to read");
DEFINE_uint64(max_write, 0, "max data to write");

DEFINE_bool(rrvs, false, "support RRVS à la RFC 7293");

Session::Session(fs::path                  config_path,
                 std::function<void(void)> read_hook,
                 int                       fd_in,
                 int                       fd_out)
  : config_path_(config_path)
  , res_(config_path_)
  , sock_(fd_in, fd_out, read_hook, Config::read_timeout, Config::write_timeout)
{
  auto accept_db_name = config_path_ / "accept_domains";
  auto black_db_name  = config_path_ / "black";
  auto white_db_name  = config_path_ / "white";

  accept_domains_.open(accept_db_name.c_str());
  black_.open(black_db_name.c_str());
  white_.open(white_db_name.c_str());

  if (sock_.has_peername() && !IP::is_private(sock_.us_c_str())) {
    auto fcrdns = DNS::fcrdns(res_, sock_.us_c_str());
    for (auto const& fcr : fcrdns) {
      server_fcrdns_.emplace_back(fcr);
    }
  }

  auto const our_id{[this] {
    auto const id_from_env{getenv("GHSMTP_SERVER_ID")};
    if (id_from_env)
      return std::string{id_from_env};

    auto const hostname{osutil::get_hostname()};
    if (hostname.find('.') != std::string::npos)
      return hostname;

    if (!server_fcrdns_.empty()) {
      // first result should be shortest
      return server_fcrdns_.front().ascii();
    }

    auto const us_c_str = sock_.us_c_str();
    if (us_c_str && !IP::is_private(us_c_str)) {
      return IP::to_address_literal(us_c_str);
    }

    LOG(FATAL) << "can't determine my server ID, set GHSMTP_SERVER_ID maybe";
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

void Session::bad_host_(char const* msg) const
{
  if (sock_.has_peername()) {
    // On my systems, this pattern triggers a fail2ban rule that
    // blocks connections from this IP address on port 25 for a few
    // days.  See <https://www.fail2ban.org/> for more info.
    syslog(LOG_MAIL | LOG_WARNING, "bad host [%s] %s", sock_.them_c_str(), msg);
  }
  std::exit(EXIT_SUCCESS);
}

void Session::reset_()
{
  // RSET does not force another EHLO/HELO, the one piece of per
  // transaction data saved is client_identity_:

  // client_identity_.clear(); <-- not cleared!

  reverse_path_.clear();
  forward_path_.clear();
  spf_received_.clear();

  binarymime_ = false;
  smtputf8_   = false;

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
    close(2); // if we're a networked program, never send to stderr

    std::string error_msg;
    if (!verify_ip_address_(error_msg)) {
      // no glog message at this point
      bad_host_(error_msg.c_str());
    }

    /******************************************************************
    <https://tools.ietf.org/html/rfc5321#section-4.3.1> says:

    4.3.  Sequencing of Commands and Replies

    4.3.1.  Sequencing Overview

    The communication between the sender and receiver is an alternating
    dialogue, controlled by the sender.  As such, the sender issues a
    command and the receiver responds with a reply.  Unless other
    arrangements are negotiated through service extensions, the sender
    MUST wait for this response before sending further commands.  One
    important reply is the connection greeting.  Normally, a receiver
    will send a 220 "Service ready" reply when the connection is
    completed.  The sender SHOULD wait for this greeting message before
    sending any commands.

    So which is it?

    “…the receiver responds with a reply.”
    “…the sender MUST wait for this response…”
    “One important reply is the connection greeting.”
    “The sender SHOULD wait for this greeting…”

    So is it MUST or SHOULD?  I enforce MUST.
    *******************************************************************/

    // Wait a bit of time for pre-greeting traffic.
    if (!(ip_whitelisted_ || fcrdns_whitelisted_)) {
      if (sock_.input_ready(Config::greeting_wait)) {
        out_() << "421 4.3.2 not accepting network messages\r\n" << std::flush;
        // no glog message at this point
        bad_host_("input before any greeting");
      }
      // Give a half greeting and wait again.
      out_() << "220-" << server_id_() << " ESMTP - ghsmtp\r\n" << std::flush;
      if (sock_.input_ready(Config::greeting_wait)) {
        out_() << "421 4.3.2 not accepting network messages\r\n" << std::flush;
        // LOG(INFO) << "half greeting got " << client_;
        bad_host_("input before full greeting");
      }
    }
    LOG(INFO) << "connect from " << client_;
  }

  out_() << "220 " << server_id_() << " ESMTP - ghsmtp\r\n" << std::flush;
}

void Session::flush() { out_() << std::flush; }

void Session::last_in_group_(std::string_view verb)
{
  if (sock_.input_ready(std::chrono::seconds(0))) {
    LOG(WARNING) << "pipelining error; input ready processing " << verb;
  }
}

void Session::lo_(char const* verb, std::string_view client_identity)
{
  last_in_group_(verb);
  reset_();
  extensions_ = true;

  if (client_identity_ != client_identity) {
    client_identity_ = client_identity;

    std::string error_msg;
    if (!verify_client_(client_identity_, error_msg)) {
      // no glog message at this point
      bad_host_(error_msg.c_str());
    }
  }

  if (*verb == 'H') {
    out_() << "250 " << server_id_() << "\r\n";
  }

  if (*verb == 'E') {
    out_() << "250-" << server_id_();
    if (sock_.has_peername()) {
      out_() << " at your service, " << client_;
    }
    out_() << "\r\n"
              "250-SIZE "
           << max_msg_size()
           << "\r\n"              // RFC 1870
              "250-8BITMIME\r\n"; // RFC 6152
    if (FLAGS_rrvs) {
      out_() << "250-RRVS\r\n"; // RFC 7293
    }
    if (sock_.tls()) {
      // Check sasl sources for auth types.
      // out_() << "250-AUTH PLAIN\r\n";
      out_() << "250-REQUIRETLS\r\n";
    }
    else {
      // If we're not already TLS, offer TLS, à la RFC 3207
      out_() << "250-STARTTLS\r\n";
    }
    out_() << "250-ENHANCEDSTATUSCODES\r\n" // RFC 2034
              "250-PIPELINING\r\n"          // RFC 2920
              "250-BINARYMIME\r\n"          // RFC 3030
              "250-CHUNKING\r\n"            // RFC 3030
              "250 SMTPUTF8\r\n";           // RFC 6531
  }

  out_() << std::flush;

  if (sock_.has_peername()) {
    if (std::find(begin(client_fcrdns_), end(client_fcrdns_), client_identity_)
        != end(client_fcrdns_)) {
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

void Session::mail_from(Mailbox&& reverse_path, parameters_t const& parameters)
{
  switch (state_) {
  case xact_step::helo:
    out_() << "503 5.5.1 must send HELO/EHLO first\r\n" << std::flush;
    LOG(WARNING) << "'MAIL FROM' before HELO/EHLO"
                 << (sock_.has_peername() ? " from " : "") << client_;
    return;
  case xact_step::mail: break;
  case xact_step::rcpt:
  case xact_step::data:
  case xact_step::bdat:
    out_() << "503 5.5.1 nested MAIL command\r\n" << std::flush;
    LOG(WARNING) << "nested MAIL command"
                 << (sock_.has_peername() ? " from " : "") << client_;
    return;
  case xact_step::rset:
    out_() << "503 5.5.1 sequence error, expecting RSET" << std::flush;
    LOG(WARNING) << "error state must be cleared with a RSET"
                 << (sock_.has_peername() ? " from " : "") << client_;
    return;
  }

  if (!verify_from_params_(parameters)) {
    return;
  }

  std::string error_msg;
  if (!verify_sender_(reverse_path, error_msg)) {
    LOG(WARNING) << "verify sender failed: " << error_msg;
    bad_host_(error_msg.c_str());
  }

  reverse_path_ = std::move(reverse_path);
  forward_path_.clear();
  out_() << "250 2.1.0 MAIL FROM OK\r\n";
  // No flush RFC-2920 section 3.1, this could be part of a command group.

  fmt::memory_buffer params;
  for (auto const& [name, value] : parameters) {
    fmt::format_to(params, " {}", name);
    if (!value.empty()) {
      fmt::format_to(params, "={}", value);
    }
  }
  LOG(INFO) << "MAIL FROM:<" << reverse_path_ << ">" << fmt::to_string(params);

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
  case xact_step::data: break;
  case xact_step::bdat:
    out_() << "503 5.5.1 sequence error, expecting BDAT" << std::flush;
    LOG(WARNING) << "'RCPT TO' during BDAT transfer"
                 << (sock_.has_peername() ? " from " : "") << client_;
    return;
  case xact_step::rset:
    out_() << "503 5.5.1 sequence error, expecting RSET" << std::flush;
    LOG(WARNING) << "error state must be cleared with a RSET"
                 << (sock_.has_peername() ? " from " : "") << client_;
    return;
  }

  if (!verify_rcpt_params_(parameters)) {
    return;
  }

  if (!verify_recipient_(forward_path))
    return;

  if (forward_path_.size() >= Config::max_recipients_per_message) {
    out_() << "452 4.5.3 too many recipients\r\n" << std::flush;
    LOG(WARNING) << "too many recipients <" << forward_path << ">";
    return;
  }

  // no check for dups, postfix doesn't
  forward_path_.emplace_back(std::move(forward_path));
  out_() << "250 2.1.5 RCPT TO OK\r\n";
  // No flush RFC-2920 section 3.1, this could be part of a command group.
  LOG(INFO) << "RCPT TO:<" << forward_path_.back() << ">";

  state_ = xact_step::data;
}

std::string Session::added_headers_(Message const& msg)
{
  // The headers Return-Path, Received-SPF, and Received are returned
  // as a string.
  auto const protocol{[this]() {
    if (smtputf8_)
      return sock_.tls() ? "UTF8SMTPS" : "UTF8SMTP";
    else if (extensions_)
      return sock_.tls() ? "ESMTPS" : "ESMTP";
    else
      return sock_.tls() ? "SMTPS" : "SMTP";
  }()};

  std::string const tls_info{sock_.tls_info()};

  std::ostringstream headers;
  headers.str().reserve(500);
  headers << "Return-Path: <" << reverse_path_ << ">\r\n";

  // STD 3 section 5.2.8
  auto constexpr indent    = "        ";
  auto constexpr indent_sz = sizeof(indent) - 1;
  auto constexpr break_col = 80;

  headers << "Received: from " << client_identity_.utf8();
  if (sock_.has_peername()) {
    headers << " (" << client_ << ')';
  }
  headers << "\r\n"
          << indent << "by " << server_identity_.utf8() << " with " << protocol
          << " id " << msg.id();

  if (forward_path_.size()) {
    auto constexpr phor    = "for ";
    auto constexpr phor_sz = sizeof(phor) - 1;
    headers << "\r\n" << indent << phor;
    int len = indent_sz + phor_sz;
    for (size_t i = 0; i < forward_path_.size(); ++i) {
      auto const fwd = static_cast<std::string>(forward_path_[i]);
      if (i) {
        headers << ',';
        ++len;
      }
      if ((len + fwd.length() + 2) > break_col) {
        headers << "\r\n" << indent;
        len = indent_sz;
      }
      headers << '<' << fwd << '>';
      len += fwd.length() + 2;
    }
  }

  if (tls_info.length()) {
    headers << "\r\n" << indent << '(' << tls_info << ')';
  }
  headers << ";\r\n" << indent << msg.when() << "\r\n";

  // Received-SPF:
  if (!spf_received_.empty()) {
    headers << spf_received_ << "\r\n";
  }

  return headers.str();
}

namespace {
bool lookup_domain(CDB& cdb, Domain const& domain)
{
  if (!domain.empty()) {
    if (cdb.lookup(domain.ascii())) {
      return true;
    }
    if (domain.is_unicode() && cdb.lookup(domain.utf8())) {
      return true;
    }
  }
  return false;
}
} // namespace

std::tuple<Session::SpamStatus, std::string> Session::spam_status_()
{
  if (spf_result_ == SPF::Result::FAIL && !ip_whitelisted_) {
    return {SpamStatus::spam, "SPF failed"};
  }

  auto               status{SpamStatus::spam};
  fmt::memory_buffer reason;

  // Anything enciphered tastes a lot like ham.
  if (sock_.tls()) {
    fmt::format_to(reason, "they used TLS");
    status = SpamStatus::ham;
  }

  if (spf_result_ == SPF::Result::PASS) {
    if (lookup_domain(white_, spf_sender_domain_)) {
      if (status == SpamStatus::ham) {
        fmt::format_to(reason, ", and ");
      }
      fmt::format_to(reason, "SPF sender domain ({}) is whitelisted",
                     spf_sender_domain_.utf8());
      status = SpamStatus::ham;
    }
    else {
      auto tld_dom{tld_db_.get_registered_domain(spf_sender_domain_.ascii())};
      if (tld_dom && white_.lookup(tld_dom)) {
        if (status == SpamStatus::ham) {
          fmt::format_to(reason, ", and ");
        }
        fmt::format_to(reason,
                       "SPF sender registered domain ({}) is whitelisted",
                       tld_dom);
        status = SpamStatus::ham;
      }
    }
  }

  if (fcrdns_whitelisted_) {
    if (status == SpamStatus::ham) {
      fmt::format_to(reason, ", and ");
    }
    fmt::format_to(reason, "FCrDNS (or it's registered domain) is whitelisted");
    status = SpamStatus::ham;
  }

  if (status != SpamStatus::ham)
    return {SpamStatus::spam, "it's not ham"};

  return {status, fmt::to_string(reason)};
}

bool Session::msg_new()
{
  CHECK((state_ == xact_step::data) || (state_ == xact_step::bdat));

  auto const& [status, reason]{spam_status_()};

  LOG(INFO) << ((status == SpamStatus::ham) ? "ham since " : "spam since ")
            << reason;

  // All sources of ham get a fresh 5 minute timeout per message.
  if (status == SpamStatus::ham) {
    alarm(5 * 60);
  }

  msg_ = std::make_unique<Message>();

  if (!FLAGS_max_write)
    FLAGS_max_write = max_msg_size();

  try {
    msg_->open(server_id_(), FLAGS_max_write,
               (status == SpamStatus::spam) ? ".Junk" : "");
    auto const hdrs{added_headers_(*(msg_.get()))};
    msg_->write(hdrs);

    fmt::memory_buffer spam_status;
    fmt::format_to(spam_status, "X-Spam-Status: {}, {}\r\n",
                   ((status == SpamStatus::spam) ? "Yes" : "No"), reason);
    msg_->write(spam_status.data(), spam_status.size());
    return true;
  }
  catch (std::system_error const& e) {
    switch (errno) {
    case ENOSPC:
      out_() << "452 4.3.1 mail system full\r\n" << std::flush;
      LOG(ERROR) << "no space";
      msg_->trash();
      msg_.reset();
      return false;

    default:
      out_() << "550 5.0.0 mail system error\r\n" << std::flush;
      LOG(ERROR) << "errno==" << errno << ": " << strerror(errno);
      LOG(ERROR) << e.what();
      msg_->trash();
      msg_.reset();
      return false;
    }
  }
  catch (std::exception const& e) {
    out_() << "550 5.0.0 mail error\r\n" << std::flush;
    LOG(ERROR) << e.what();
    msg_->trash();
    msg_.reset();
    return false;
  }

  // out_() << "550 5.0.0 mail error\r\n" << std::flush;
  // LOG(ERROR) << "msg_new failed with no exception thrown";
  // msg_->trash();
  // msg_.reset();
  // return false;
}

bool Session::msg_write(char const* s, std::streamsize count)
{
  if ((state_ != xact_step::data) && (state_ != xact_step::bdat))
    return false;

  if (!msg_)
    return false;

  try {
    if (msg_->write(s, count))
      return true;
  }
  catch (std::system_error const& e) {
    switch (errno) {
    case ENOSPC:
      out_() << "452 4.3.1 mail system full\r\n" << std::flush;
      LOG(ERROR) << "no space";
      msg_->trash();
      msg_.reset();
      return false;

    default:
      out_() << "550 5.0.0 mail system error\r\n" << std::flush;
      LOG(ERROR) << "errno==" << errno << ": " << strerror(errno);
      LOG(ERROR) << e.what();
      msg_->trash();
      msg_.reset();
      return false;
    }
  }
  catch (std::exception const& e) {
    out_() << "550 5.0.0 mail error\r\n" << std::flush;
    LOG(ERROR) << e.what();
    msg_->trash();
    msg_.reset();
    return false;
  }

  out_() << "550 5.0.0 mail error\r\n" << std::flush;
  LOG(ERROR) << "write failed with no exception thrown";
  msg_->trash();
  msg_.reset();
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
  case xact_step::data: break;
  case xact_step::bdat:
    out_() << "503 5.5.1 sequence error, expecting BDAT" << std::flush;
    LOG(WARNING) << "'DATA' during BDAT transfer"
                 << (sock_.has_peername() ? " from " : "") << client_;
    return false;
  case xact_step::rset:
    out_() << "503 5.5.1 sequence error, expecting RSET" << std::flush;
    LOG(WARNING) << "error state must be cleared with a RSET"
                 << (sock_.has_peername() ? " from " : "") << client_;
    return false;
  }

  if (binarymime_) {
    out_() << "503 5.5.1 DATA does not support BINARYMIME\r\n" << std::flush;
    LOG(WARNING) << "DATA does not support BINARYMIME";
    state_ = xact_step::rset; // RFC 3030 section 3 page 5
    return false;
  }
  // for bounce messages
  // CHECK(!reverse_path_.empty());
  CHECK(!forward_path_.empty());

  if (!msg_new()) {
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

  if (msg_ && msg_->size_error()) {
    data_size_error();
    return;
  }

  CHECK(msg_);
  try {
    msg_->save();
  }
  catch (std::system_error const& e) {
    switch (errno) {
    case ENOSPC:
      out_() << "452 4.3.1 mail system full\r\n" << std::flush;
      LOG(ERROR) << "no space";
      msg_->trash();
      reset_();
      return;

    default:
      out_() << "550 5.0.0 mail system error\r\n" << std::flush;
      LOG(ERROR) << "errno==" << errno << ": " << strerror(errno);
      LOG(ERROR) << e.what();
      msg_->trash();
      reset_();
      return;
    }
  }

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
  out_() << "554 5.3.0 message error of some kind\r\n" << std::flush;
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
  case xact_step::bdat: return true;
  case xact_step::rset:
    out_() << "503 5.5.1 sequence error, expecting RSET" << std::flush;
    LOG(WARNING) << "error state must be cleared with a RSET"
                 << (sock_.has_peername() ? " from " : "") << client_;
    return false;
  }

  CHECK(!forward_path_.empty());

  state_ = xact_step::bdat;

  return msg_new();
}

void Session::bdat_done(size_t n, bool last)
{
  if (state_ != xact_step::bdat) {
    bdat_error();
    return;
  }

  if (!msg_) {
    return;
  }

  if (msg_->size_error()) {
    bdat_size_error();
    return;
  }

  if (!last) {
    out_() << "250 2.0.0 BDAT " << n << " OK\r\n" << std::flush;
    LOG(INFO) << "BDAT " << n;
    return;
  }

  CHECK(msg_);
  try {
    msg_->save();
  }
  catch (std::system_error const& e) {
    switch (errno) {
    case ENOSPC:
      out_() << "452 4.3.1 mail system full\r\n" << std::flush;
      LOG(ERROR) << "no space";
      msg_->trash();
      reset_();
      return;

    default:
      out_() << "550 5.0.0 mail system error\r\n" << std::flush;
      LOG(ERROR) << "errno==" << errno << ": " << strerror(errno);
      LOG(ERROR) << e.what();
      msg_->trash();
      reset_();
      return;
    }
  }

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
  if (msg_) {
    msg_->trash();
  }
  LOG(WARNING) << "BDAT size error";
  reset_();
}

void Session::bdat_error()
{
  out_().clear(); // clear possible eof from input side
  out_() << "503 5.5.1 BDAT sequence error\r\n" << std::flush;
  if (msg_) {
    msg_->trash();
  }
  LOG(WARNING) << "BDAT sequence error";
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
  // last_in_group_("QUIT");
  out_() << "221 2.0.0 closing connection\r\n" << std::flush;
  LOG(INFO) << "QUIT";
  exit_();
}

void Session::auth()
{
  out_() << "454 4.7.0 authentication failure\r\n" << std::flush;
  LOG(INFO) << "AUTH";
  bad_host_("auth");
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
  out_() << "552 5.3.4 message size limit exceeded\r\n" << std::flush;
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
    if (sock_.starttls_server(config_path_)) {
      reset_();
      max_msg_size(Config::max_msg_size_bro);
      LOG(INFO) << "STARTTLS " << sock_.tls_info();
    }
  }
}

void Session::exit_()
{
  // sock_.log_totals();

  timespec time_used{};
  clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &time_used);

  LOG(INFO) << "CPU time " << time_used.tv_sec << "." << std::setw(9)
            << std::setfill('0') << time_used.tv_nsec << " seconds";

  std::exit(EXIT_SUCCESS);
}

namespace {
bool ip4_whitelisted(char const* addr)
{
  struct nw {
    char const* net;
    char const* mask;
    char const* comment;
  };

  // clang-format off

  // 255 0b11111111 8
  // 254 0b11111110 7
  // 252 0b11111100 6
  // 248 0b11111000 5
  // 240 0b11110000 4
  // 224 0b11100000 3
  // 192 0b11000000 2
  // 128 0b10000000 1

  nw const networks[]{
    // the one very special case
    {"108.83.36.112",   "255.255.255.248", "108.83.36.112/29"},

    // accept from major providers:
    {"5.45.198.0",      "255.255.254.0",   "5.45.198.0/23 YANDEX-5-45-198"},
    {"12.153.224.0",    "255.255.255.0",   "12.153.224.0/24 E-TRADE10-224"},
    {"17.0.0.0",        "255.0.0.0",       "17.0.0.0/8 APPLE-WWNET"},
    {"56.0.0.0",        "255.0.0.0",       "56.0.0.0/8 USPS1"},
    {"65.52.0.0",       "255.252.0.0",     "65.52.0.0/14 MICROSOFT-1BLK"},
    {"66.163.160.0",    "255.255.224.0",   "66.163.160.0/19 A-YAHOO-US2"},
    {"66.220.144.0",    "255.255.240.0",   "66.220.144.0/20 TFBNET3"},
    {"68.232.192.0",    "255.255.240.0",   "68.232.192.0/20 EXACT-IP-NET-2"},
    {"70.47.67.0",      "255.255.255.0",   "70.47.67.0/24 NET-462F4300-24"},
    {"74.125.0.0",      "255.255.0.0",     "74.125.0.0/16 GOOGLE"},
    {"98.136.0.0",      "255.252.0.0",     "98.136.0.0/14 A-YAHOO-US9"},
    {"104.40.0.0",      "255.248.0.0",     "104.40.0.0/13 MSFT"},
    {"108.174.0.0",     "255.255.240.0",   "108.174.0.0/20 LINKEDIN"},
    {"159.45.0.0",      "255.255.0.0",     "159.45.0.0/16 AGE-COM"},
    {"159.53.0.0",      "255.255.0.0",     "159.53.0.0/16 JMC"},
    {"159.135.224.0",   "255.255.240.0",   "159.135.224.0/20 MNO87-159-135-224-0-0"},
    {"162.247.72.0",    "255.255.252.0",   "162.247.72.0/22 CALYX-INSTITUTE-V4-1"},
    {"165.107.0.0",     "255.255.0.0",     "NET-LDC-CA-GOV"},
    {"192.175.128.0",   "255.255.128.0",   "192.175.128.0/17 NETBLK-VANGUARD"},
    {"198.2.128.0",     "255.255.192.0",   "198.2.128.0/18 RSG-DELIVERY"},
    {"198.252.206.0",   "255.255.255.0",   "198.252.206.0/24 SE-NET01"},
    {"199.122.120.0",   "255.255.248.0",   "199.122.120.0/21 EXACT-IP-NET-3"},
    {"204.13.164.0",    "255.255.255.0",   "204.13.164.0/24 RISEUP-NETWORKS-SWIFT-BLOCK2"},
    {"204.29.186.0",    "255.255.254.0",   "204.29.186.0/23 ATDN-NSCAPE"},
    {"205.139.104.0",   "255.255.252.0",   "205.139.104.0/22 SAVV-S259964-8"},
    {"205.201.128.0",   "255.255.240.0",   "205.201.128.0/20 RSG-DELIVERY"},
    {"208.118.235.0",   "255.255.255.0",   "208.118.235.0/24 TWDX-208-118-235-0-1"},
    {"208.192.0.0",     "255.192.0.0",     "208.192.0.0/10 UUNET1996B"},
    {"209.85.128.0",    "255.255.128.0",   "209.85.128.0/17 GOOGLE"},
    {"209.132.176.0",   "255.255.240.0",   "209.132.176.0/20 RED-HAT-BLK"},
    {"209.237.224.0",   "255.255.224.0",   "UNITEDLAYER-1"},
  };
  // clang-format on

  uint32_t addr32;
  CHECK_EQ(inet_pton(AF_INET, addr, &addr32), 1)
      << "can't interpret as IPv4 address";

  for (auto const& network : networks) {
    uint32_t net32;
    CHECK_EQ(inet_pton(AF_INET, network.net, &net32), 1)
        << "can't grok " << network.net;
    uint32_t mask32;
    CHECK_EQ(inet_pton(AF_INET, network.mask, &mask32), 1)
        << "can't grok " << network.mask;

    // sanity check: all unmasked bits must be zero
    CHECK_EQ(net32 & (~mask32), 0)
        << "bogus config net=" << network.net << ", mask=" << network.mask;

    if (net32 == (addr32 & mask32)) {
      LOG(INFO) << addr << " whitelisted " << network.comment;
      return true;
    }
  }

  return false;
}
} // namespace

/////////////////////////////////////////////////////////////////////////////

// All of the verify_* functions send their own error messages back to
// the client on failure, and return false.

bool Session::verify_ip_address_(std::string& error_msg)
{
  auto ip_black_db_name = config_path_ / "ip-black";
  CDB  ip_black{ip_black_db_name};
  if (ip_black.lookup(sock_.them_c_str())) {
    error_msg
        = fmt::format("IP address {} on static blacklist", sock_.them_c_str());
    out_() << "554 5.7.1 " << error_msg << "\r\n" << std::flush;
    return false;
  }

  client_fcrdns_.clear();

  if ((sock_.them_address_literal() == IP4::loopback_literal)
      || (sock_.them_address_literal() == IP6::loopback_literal)) {
    LOG(INFO) << "loopback address whitelisted";
    ip_whitelisted_ = true;
    client_fcrdns_.emplace_back("localhost");
    client_ = fmt::format("localhost {}", sock_.them_address_literal());
    return true;
  }

  auto const fcrdns = DNS::fcrdns(res_, sock_.them_c_str());
  for (auto const& fcr : fcrdns) {
    client_fcrdns_.emplace_back(fcr);
  }

  if (!client_fcrdns_.empty()) {
    client_ = fmt::format("{} {}", client_fcrdns_.front().ascii(),
                          sock_.them_address_literal());
    // check blacklist
    for (auto const& client_fcrdns : client_fcrdns_) {
      if (black_.lookup(client_fcrdns.ascii())) {
        error_msg = fmt::format("FCrDNS {} on static blacklist",
                                client_fcrdns.ascii());
        out_() << "554 5.7.1 blacklisted\r\n" << std::flush;
        return false;
      }

      auto const tld{tld_db_.get_registered_domain(client_fcrdns.ascii())};
      if (tld) {
        if (black_.lookup(tld)) {
          error_msg = fmt::format(
              "FCrDNS registered domain {} on static blacklist", tld);
          out_() << "554 5.7.1 blacklisted\r\n" << std::flush;
          return false;
        }
      }
    }

    // check whitelist
    for (auto const& client_fcrdns : client_fcrdns_) {
      if (white_.lookup(client_fcrdns.ascii())) {
        // LOG(INFO) << "FCrDNS " << client_fcrdns << " whitelisted";
        fcrdns_whitelisted_ = true;
        return true;
      }
      auto const tld{tld_db_.get_registered_domain(client_fcrdns.ascii())};
      if (tld) {
        if (white_.lookup(tld)) {
          // LOG(INFO) << "FCrDNS registered domain " << tld << " whitelisted";
          fcrdns_whitelisted_ = true;
          return true;
        }
      }
    }
  }
  else {
    client_ = fmt::format("unknown {}", sock_.them_address_literal());
  }

  if (IP4::is_address(sock_.them_c_str())
      && ip4_whitelisted(sock_.them_c_str())) {
    ip_whitelisted_ = true;
    return true;
  }

  return verify_ip_address_dnsbl_(error_msg);
}

bool Session::verify_ip_address_dnsbl_(std::string& error_msg)
{
  if (IP4::is_address(sock_.them_c_str())) {
    // Check with black hole lists. <https://en.wikipedia.org/wiki/DNSBL>
    auto const reversed{IP4::reverse(sock_.them_c_str())};
    std::shuffle(std::begin(Config::rbls), std::end(Config::rbls),
                 random_device_);
    for (auto rbl : Config::rbls) {
      if (has_record(res_, DNS::RR_type::A, reversed + rbl)) {
        error_msg = fmt::format("blocked on advice from {}", rbl);
        // LOG(INFO) << sock_.them_c_str() << " " << error_msg;
        out_() << "554 5.7.1 " << error_msg << "\r\n" << std::flush;
        return false;
      }
    }
    // LOG(INFO) << "IP address " << sock_.them_c_str() << " cleared by dnsbls";
  }

  return true;
}

// check the identity from HELO/EHLO
bool Session::verify_client_(Domain const& client_identity,
                             std::string&  error_msg)
{
  if (!client_fcrdns_.empty()) {
    if (auto id = std::find(begin(client_fcrdns_), end(client_fcrdns_),
                            client_identity);
        id != end(client_fcrdns_)) {
      if (id != begin(client_fcrdns_)) {
        std::rotate(begin(client_fcrdns_), id, id + 1);
      }
      client_ = fmt::format("{} {}", client_fcrdns_.front().ascii(),
                            sock_.them_address_literal());
      return true;
    }
    LOG(INFO) << "claimed identity " << client_identity
              << " does NOT match any FCrDNS: ";
    for (auto const& client_fcrdns : client_fcrdns_) {
      LOG(INFO) << "                 " << client_fcrdns;
    }
  }

  // Bogus clients claim to be us or some local host.
  if (sock_.has_peername()
      && ((client_identity == server_identity_)
          || (client_identity == "localhost")
          || (client_identity == "localhost.localdomain"))) {

    if ((sock_.them_address_literal() == IP4::loopback_literal)
        || (sock_.them_address_literal() == IP6::loopback_literal)) {
      return true;
    }

    error_msg = fmt::format("liar, claimed to be {}", client_identity.ascii());
    out_() << "550 5.7.1 liar\r\n" << std::flush;
    return false;
  }

  std::vector<std::string> labels;
  boost::algorithm::split(labels, client_identity.ascii(),
                          boost::algorithm::is_any_of("."));
  if (labels.size() < 2) {
    error_msg
        = fmt::format("claimed bogus identity {}", client_identity.ascii());
    out_() << "550 4.7.1 bogus identity\r\n" << std::flush;
    return false;
    // // Sometimes we may want to look at mail from non conforming
    // // sending systems.
    // LOG(WARNING) << "invalid sender" << (sock_.has_peername() ? " " : "")
    //              << client_ << " claiming " << client_identity;
    // return true;
  }

  if (lookup_domain(black_, client_identity)) {
    error_msg = fmt::format("claimed blacklisted identity {}",
                            client_identity.ascii());
    out_() << "550 4.7.1 blacklisted identity\r\n" << std::flush;
    return false;
  }

  auto const tld{tld_db_.get_registered_domain(client_identity.ascii())};
  if (!tld) {
    // Sometimes we may want to look at mail from misconfigured
    // sending systems.
    // LOG(WARNING) << "claimed identity has no registered domain";
    // return true;
  }
  else if (black_.lookup(tld)) {
    error_msg = fmt::format(
        "claimed identity has blacklisted registered domain {}", tld);
    out_() << "550 4.7.1 blacklisted registered domain\r\n" << std::flush;
    return false;
  }

  // not otherwise objectionable
  return true;
}

// check sender from RFC5321 MAIL FROM:
bool Session::verify_sender_(Mailbox const& sender, std::string& error_msg)
{
  std::string const sender_str{sender};

  auto bad_senders_db_name = config_path_ / "bad_senders";
  CDB  bad_senders{bad_senders_db_name}; // Addresses we don't accept mail from.
  if (bad_senders.lookup(sender_str)) {
    error_msg = fmt::format("{} bad sender", sender_str);
    out_() << "501 5.1.8 " << error_msg << "\r\n" << std::flush;
    return false;
  }

  // We don't accept mail /from/ a domain we are expecting to accept
  // mail for on an external network connection.

  if (sock_.them_address_literal() != sock_.us_address_literal()) {
    if ((accept_domains_.is_open()
         && (accept_domains_.lookup(sender.domain().ascii())
             || accept_domains_.lookup(sender.domain().utf8())))
        || (sender.domain() == server_identity_)) {
      out_() << "550 5.7.1 liar\r\n" << std::flush;
      error_msg = fmt::format("liar, claimed to be {}", sender.domain());
      return false;
    }
  }

  if (sender.domain().is_address_literal()) {
    if (sender.domain() != sock_.them_address_literal()) {
      LOG(WARNING) << "sender domain " << sender.domain() << " does not match "
                   << sock_.them_address_literal();
    }
    return true;
  }

  if (!verify_sender_domain_(sender.domain(), error_msg)) {
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
                                    std::string&  error_msg)
{
  if (sender.empty()) {
    // MAIL FROM:<>
    // is used to send bounce messages.
    return true;
  }

  if (white_.lookup(sender.ascii())) {
    LOG(INFO) << "sender " << sender.ascii() << " whitelisted";
    return true;
  }

  // Break sender domain into labels:

  std::vector<std::string> labels;
  boost::algorithm::split(labels, sender.ascii(),
                          boost::algorithm::is_any_of("."));

  if (labels.size() < 2) { // This is not a valid domain.
    error_msg = fmt::format("{} invalid syntax", sender.ascii());
    out_() << "550 5.7.1 " << error_msg << "\r\n" << std::flush;
    return false;
  }

  auto const reg_dom{tld_db_.get_registered_domain(sender.ascii())};
  if (!reg_dom) {
    error_msg = fmt::format("{} has no registered domain", sender.ascii());
    out_() << "550 5.7.1 " << error_msg << "\r\n" << std::flush;
    return false;
  }
  if (white_.lookup(reg_dom)) {
    LOG(INFO) << "sender registered domain \"" << reg_dom << "\" whitelisted";
    return true;
  }

  // Based on <http://www.surbl.org/guidelines>

  auto const two_level = fmt::format("{}.{}", labels[labels.size() - 2],
                                     labels[labels.size() - 1]);

  if (labels.size() > 2) {
    auto const three_level
        = fmt::format("{}.{}", labels[labels.size() - 3], two_level);

    auto three_tld_db_name = config_path_ / "three-level-tlds";
    CDB  three_tld{three_tld_db_name};
    if (three_tld.lookup(three_level)) {
      LOG(INFO) << reg_dom << " found on the three level list";
      if (labels.size() > 3) {
        auto const look_up
            = fmt::format("{}.{}", labels[labels.size() - 4], three_level);
        LOG(INFO) << "looking up " << look_up;
        return verify_sender_domain_uribl_(look_up, error_msg);
      }
      else {
        out_() << "550 5.7.1 bad sender domain\r\n" << std::flush;
        error_msg = fmt::format(
            "{} blocked by exact match on three-level-tlds list", three_level);
        return false;
      }
    }
  }

  auto two_tld_db_name = config_path_ / "two-level-tlds";
  CDB  two_tld{two_tld_db_name};
  if (two_tld.lookup(two_level)) {
    LOG(INFO) << reg_dom << " found on the two level list";
    if (labels.size() > 2) {
      auto const look_up
          = fmt::format("{}.{}", labels[labels.size() - 3], two_level);
      LOG(INFO) << "looking up " << look_up;
      return verify_sender_domain_uribl_(look_up, error_msg);
    }
    else {
      out_() << "550 5.7.1 bad sender domain\r\n" << std::flush;
      error_msg = fmt::format(
          "{} blocked by exact match on two-level-tlds list", two_level);
      return false;
    }
  }

  // LOG(INFO) << "looking up " << reg_dom;
  return verify_sender_domain_uribl_(reg_dom, error_msg);
}

// check sender domain on dynamic URI black lists
bool Session::verify_sender_domain_uribl_(std::string_view sender,
                                          std::string&     error_msg)
{
  if (!sock_.has_peername()) // short circuit
    return true;

  std::shuffle(std::begin(Config::uribls), std::end(Config::uribls),
               random_device_);
  for (auto uribl : Config::uribls) {
    auto const lookup = fmt::format("{}.{}", sender, uribl);
    auto       as     = DNS::get_strings(res_, DNS::RR_type::A, lookup);
    if (!as.empty()) {
      if (as.front() == "127.0.0.1")
        continue;
      error_msg = fmt::format("{} blocked on advice of {}", sender, uribl);
      out_() << "550 5.7.1 sender " << error_msg << "\r\n" << std::flush;
      return false;
    }
  }

  LOG(INFO) << sender << " cleared by URIBLs";
  return true;
}

bool Session::verify_sender_spf_(Mailbox const& sender)
{
  if (!sock_.has_peername() || ip_whitelisted_) {
    auto ip_addr = sock_.them_c_str();
    if (!sock_.has_peername()) {
      ip_addr = "127.0.0.1"; // use localhost for local socket
    }
    spf_received_
        = fmt::format("Received-SPF: pass ({}: whitelisted) client-ip={}; "
                      "envelope-from={}; helo={};",
                      server_id_(), ip_addr, sender, client_identity_);
    spf_sender_domain_ = "localhost";
    return true;
  }

  SPF::Server const spf_srv{server_id_().c_str()};
  SPF::Request      spf_request{spf_srv};

  if (IP4::is_address(sock_.them_c_str())) {
    spf_request.set_ipv4_str(sock_.them_c_str());
  }
  else if (IP6::is_address(sock_.them_c_str())) {
    spf_request.set_ipv6_str(sock_.them_c_str());
  }
  else {
    LOG(FATAL) << "bogus address " << sock_.them_address_literal() << ", "
               << sock_.them_c_str();
  }

  spf_request.set_helo_dom(client_identity_.ascii().c_str());

  auto const from{static_cast<std::string>(sender)};

  spf_request.set_env_from(from.c_str());

  auto const spf_res{SPF::Response{spf_request}};
  spf_result_        = spf_res.result();
  spf_received_      = spf_res.received_spf();
  spf_sender_domain_ = Domain{spf_request.get_sender_dom()};

  if (spf_result_ == SPF::Result::PASS) {
    if (lookup_domain(black_, spf_sender_domain_)) {
      LOG(INFO) << "SPF sender domain (" << spf_sender_domain_
                << ") is blacklisted";
      return false;
    }
  }

  if (spf_result_ == SPF::Result::FAIL) {
    LOG(WARNING) << spf_res.header_comment();
    /*
      If we want to refuse mail that fails SPF.
      Error code from RFC 7372, section 3.2.  Also:
      <https://www.iana.org/assignments/smtp-enhanced-status-codes/smtp-enhanced-status-codes.xhtml>

      out_() << "550 5.7.23 " << spf_res.smtp_comment() << "\r\n" << std::flush;
      return false;
    */
  }
  else {
    LOG(INFO) << spf_res.header_comment();
  }

  return true;
}

bool Session::verify_from_params_(parameters_t const& parameters)
{
  // Take a look at the optional parameters:
  for (auto const& [name, value] : parameters) {
    if (iequal(name, "BODY")) {
      if (iequal(value, "8BITMIME")) {
        // everything is cool, this is our default...
      }
      else if (iequal(value, "7BIT")) {
        // nothing to see here, move along...
      }
      else if (iequal(value, "BINARYMIME")) {
        binarymime_ = true;
      }
      else {
        LOG(WARNING) << "unrecognized BODY type \"" << value << "\" requested";
      }
    }
    else if (iequal(name, "SMTPUTF8")) {
      if (!value.empty()) {
        LOG(WARNING) << "SMTPUTF8 parameter has a value: " << value;
      }
      smtputf8_ = true;
    }
    else if (iequal(name, "SIZE")) {
      if (value.empty()) {
        LOG(WARNING) << "SIZE parameter has no value.";
      }
      else {
        try {
          auto const sz = stoull(value);
          if (sz > max_msg_size()) {
            out_() << "552 5.3.4 message size limit exceeded\r\n" << std::flush;
            LOG(WARNING) << "SIZE parameter too large: " << sz;
            return false;
          }
        }
        catch (std::invalid_argument const& e) {
          LOG(WARNING) << "SIZE parameter has invalid value: " << value;
        }
        catch (std::out_of_range const& e) {
          LOG(WARNING) << "SIZE parameter has out-of-range value: " << value;
        }
        // I guess we just ignore bad size parameters.
      }
    }
    else if (iequal(name, "REQUIRETLS")) {
      if (!sock_.tls()) {
        out_() << "554 5.7.1 REQUIRETLS needed\r\n" << std::flush;
        LOG(WARNING) << "REQUIRETLS needed";
        return false;
      }
    }
    else {
      LOG(WARNING) << "unrecognized 'MAIL FROM' parameter " << name << "="
                   << value;
    }
  }

  return true;
}

bool Session::verify_rcpt_params_(parameters_t const& parameters)
{
  // Take a look at the optional parameters:
  for (auto const& [name, value] : parameters) {
    if (iequal(name, "RRVS")) {
      // rrvs-param = "RRVS=" date-time [ ";" ( "C" / "R" ) ]
      LOG(INFO) << name << "=" << value;
    }
    else {
      LOG(WARNING) << "unrecognized 'RCPT TO' parameter " << name << "="
                   << value;
    }
  }

  return true;
}

// check recipient from RFC5321 RCPT TO:
bool Session::verify_recipient_(Mailbox const& recipient)
{
  if ((recipient.local_part() == "Postmaster") && (recipient.domain() == "")) {
    LOG(INFO) << "magic Postmaster address";
    return true;
  }

  auto const accepted_domain{[this, &recipient] {
    if (recipient.domain().is_address_literal()) {
      if (recipient.domain() != sock_.us_address_literal()) {
        LOG(WARNING) << "recipient.domain address " << recipient.domain()
                     << " does not match ours " << sock_.us_address_literal();
        return false;
      }
      return true;
    }

    // Domains we accept mail for.
    if (accept_domains_.is_open()) {
      if (accept_domains_.lookup(recipient.domain().ascii())
          || accept_domains_.lookup(recipient.domain().utf8())) {
        return true;
      }
    }
    else {
      // If we have no list of domains to accept, at least take our own.
      if (recipient.domain() == server_identity_) {
        return true;
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
  auto bad_recipients_db_name = config_path_ / "bad_recipients";
  CDB  bad_recipients_db{bad_recipients_db_name};
  if (bad_recipients_db.lookup(recipient.local_part())) {
    out_() << "550 5.1.1 bad recipient " << recipient << "\r\n" << std::flush;
    LOG(WARNING) << "bad recipient " << recipient;
    return false;
  }

  return true;
}
