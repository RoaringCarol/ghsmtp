#include <algorithm>
#include <iomanip>
#include <iostream>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

#include "CDB.hpp"
#include "DNS.hpp"
#include "Domain.hpp"
#include "IP.hpp"
#include "IP4.hpp"
#include "IP6.hpp"
#include "Message.hpp"
#include "SPF.hpp"
#include "Session.hpp"
#include "esc.hpp"
#include "hostname.hpp"
#include "iequal.hpp"

#include <boost/algorithm/string/case_conv.hpp>
#include <boost/algorithm/string/classification.hpp>
#include <boost/algorithm/string/split.hpp>

#include <syslog.h>

using namespace std::string_literals;

namespace Config {
constexpr auto max_unrecognized_cmds{20};

// clang-format off
constexpr char const* const bad_recipients[]{
    "a",
    "ene",
    "h.gene",
    "jay",
    "lizard",
    "mixmaster",
    "nobody",
    "oq6_2nbq",
    "sales",
    "truthfinder",
    "zk4k7g",
};
// clang-format on

constexpr char const* const bad_senders[]{};

constexpr char const* const rbls[]{
    "zen.spamhaus.org",
    "b.barracudacentral.org",
};

constexpr char const* const uribls[]{
    "dbl.spamhaus.org",
    "black.uribl.com",
    "multi.surbl.org",
};

constexpr auto greeting_wait = std::chrono::seconds(3);
constexpr auto max_recipients_per_message = 100;

// Read timeout value gleaned from RFC-1123 section 5.3.2 and RFC-5321
// section 4.5.3.2.7.
constexpr auto read_timeout = std::chrono::minutes(5);
constexpr auto write_timeout = std::chrono::seconds(30);
} // namespace Config

Session::Session(std::function<void(void)> read_hook, int fd_in, int fd_out)
  : sock_(fd_in, fd_out, read_hook, Config::read_timeout, Config::write_timeout)
{
  const std::string our_id = [&] {
    char const* const id_from_env = getenv("GHSMTP_SERVER_ID");
    if (id_from_env)
      return std::string(id_from_env);

    auto const hostname = get_hostname();
    if (hostname.find('.') != std::string::npos)
      return hostname;

    if (IP::is_routable(sock_.us_c_str()))
      return "["s + sock_.us_c_str() + "]";

    LOG(FATAL) << "Can't determine my server ID.";
  }();

  server_identity_.set(our_id);

  max_msg_size(Config::max_msg_size_initial);
}

// Error codes from connection establishment are 220 or 554, according
// to RFC 5321.  That's it.

void Session::greeting()
{
  if (sock_.has_peername()) {
    std::string error_msg;
    if (!verify_ip_address_(error_msg)) {
      syslog(LOG_MAIL | LOG_WARNING, "bad host [%s] %s", sock_.them_c_str(),
             error_msg.c_str());
      std::exit(EXIT_SUCCESS);
    }
  }
  out_() << "220 " << server_id() << " ESMTP - ghsmtp\r\n" << std::flush;
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
  constexpr auto verb{"EHLO"};

  last_in_group_(verb);
  reset_();
  extensions_ = true;
  protocol_ = sock_.tls() ? "ESMTPS" : "ESMTP";
  client_identity_.set(client_identity);

  std::string error_msg;
  if (!verify_client_(client_identity_, error_msg)) {
    syslog(LOG_MAIL | LOG_WARNING, "bad host [%s] EHLO failed: %s",
           sock_.them_c_str(), error_msg.c_str());
    std::exit(EXIT_SUCCESS);
  }

  out_() << "250-" << server_id();
  if (sock_.has_peername()) {
    out_() << " at your service, " << client_;
  }
  out_() << "\r\n";

  // RFC 1870
  out_() << "250-SIZE " << max_msg_size_ << "\r\n";
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
  constexpr auto verb{"HELO"};

  last_in_group_(verb);
  reset_();
  extensions_ = false;
  protocol_ = sock_.tls() ? "ESMTPS" : "SMTP"; // there is no SMTPS
  client_identity_.set(client_identity);

  std::string error_msg;
  if (!verify_client_(client_identity_, error_msg)) {
    syslog(LOG_MAIL | LOG_WARNING, "bad host [%s] HELO failed: %s",
           sock_.them_c_str(), error_msg.c_str());
    std::exit(EXIT_SUCCESS);
  }

  out_() << "250 " << server_id() << "\r\n" << std::flush;

  log_lo_(verb, client_identity);
}

void Session::mail_from(Mailbox&& reverse_path, parameters_t const& parameters)
{
  if (client_identity_.empty()) {
    out_() << "503 5.5.1 'MAIL FROM' before 'HELO' or 'EHLO'\r\n" << std::flush;
    LOG(WARNING) << "'MAIL FROM' before 'HELO' or 'EHLO'"
                 << (sock_.has_peername() ? " from " : "") << client_;
    return;
  }

  bool smtputf8{false};

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
      smtputf8 = true;
    }
    else if (iequal(name, "SIZE")) {
      if (val.empty()) {
        LOG(WARNING) << "SIZE parameter has no value.";
      }
      else {
        try {
          auto sz = stoull(val);
          if (sz > max_msg_size_) {
            out_() << "552 5.3.4 message size exceeds maximium size\r\n"
                   << std::flush;
            LOG(WARNING) << "SIZE parameter too large: " << sz;
            return;
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

  if (smtputf8) {
    protocol_ = sock_.tls() ? "UTF8SMTPS" : "UTF8SMTP";
  }
  else {
    if (extensions_) {
      protocol_ = sock_.tls() ? "ESMTPS" : "ESMTP";
    }
    else {
      protocol_ = sock_.tls() ? "SMTPS" : "SMTP";
    }
  }

  std::string const sender{reverse_path};

  for (auto bad_sender : Config::bad_senders) {
    if (sender == bad_sender) {
      out_() << "550 5.1.8 bad sender\r\n" << std::flush;
      LOG(WARNING) << "bad sender " << sender;
      return;
    }
  }

  std::ostringstream params;
  for (auto const& [name, value] : parameters) {
    params << " " << name << (value.empty() ? "" : "=") << value;
  }

  if (verify_sender_(reverse_path)) {
    reverse_path_ = std::move(reverse_path);
    forward_path_.clear();
    out_() << "250 2.1.0 OK\r\n";
    // No flush RFC-2920 section 3.1, this could be part of a command group.
    LOG(INFO) << "MAIL FROM:<" << reverse_path_ << ">" << params.str();
  }
  else {
    LOG(ERROR) << "** Failed! ** MAIL FROM:<" << reverse_path << ">"
               << params.str();
    syslog(LOG_MAIL | LOG_WARNING, "bad host [%s] verify_sender_ fail",
           sock_.them_c_str());
    std::exit(EXIT_SUCCESS);
  }
}

void Session::rcpt_to(Mailbox&& forward_path, parameters_t const& parameters)
{
  if (!reverse_path_verified_) {
    out_() << "503 5.5.1 'RCPT TO' before 'MAIL FROM'\r\n" << std::flush;
    LOG(WARNING) << "'RCPT TO' before 'MAIL FROM'"
                 << (sock_.has_peername() ? " from " : "") << client_;
    return;
  }

  // Take a look at the optional parameters, we don't accept any:
  for (auto const& [name, value] : parameters) {
    LOG(WARNING) << "unrecognized 'RCPT TO' parameter " << name << "=" << value;
  }

  if (verify_recipient_(forward_path)) {
    if (forward_path_.size() >= Config::max_recipients_per_message) {
      out_() << "452 4.5.3 Too many recipients\r\n" << std::flush;
      LOG(WARNING) << "too many recipients <" << forward_path << ">";
    }
    else {
      forward_path_.push_back(std::move(forward_path));
      out_() << "250 2.1.5 OK\r\n";
      // No flush RFC-2920 section 3.1, this could be part of a command group.
      LOG(INFO) << "RCPT TO:<" << forward_path_.back() << ">";
    }
  }
  // We're lenient on most bad recipients, no else/exit here.
}

bool Session::data_start()
{
  last_in_group_("DATA");

  if (binarymime_) {
    out_() << "503 5.5.1 DATA does not support BINARYMIME\r\n" << std::flush;
    LOG(ERROR) << "DATA does not support BINARYMIME";
    return false;
  }
  if (!reverse_path_verified_) {
    out_() << "503 5.5.1 need 'MAIL FROM' before 'DATA'\r\n" << std::flush;
    LOG(ERROR) << "need 'MAIL FROM' before 'DATA'";
    return false;
  }
  if (forward_path_.empty()) {
    out_() << "503 5.5.1 need 'RCPT TO' before 'DATA'\r\n" << std::flush;
    LOG(ERROR) << "no valid recipients";
    return false;
  }
  out_() << "354 go, end with <CR><LF>.<CR><LF>\r\n" << std::flush;

  LOG(INFO) << "DATA";
  return true;
}

std::string Session::added_headers_(Message const& msg)
{
  // The headers Return-Path, Received-SPF, and Received are returned
  // as a string.

  std::ostringstream headers;
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
          << protocol_ << " id " << msg.id();

  if (forward_path_.size()) {
    auto len = 12;
    headers << "\r\n        for ";
    for (size_t i = 0; i < forward_path_.size(); ++i) {
      auto fwd = static_cast<std::string>(forward_path_[i]);
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
    if (cdb.lookup(domain.ascii())) {
      return true;
    }
    if (domain.is_unicode() && cdb.lookup(domain.utf8())) {
      return true;
    }
  }
  return false;
}

void Session::data_msg(Message& msg) // called /after/ {data/bdat}_start
{
  auto const status = [&] {
    if (sock_.tls()) { // Anything enciphered tastes a lot like ham.
      return Message::SpamStatus::ham;
    }

    // I will allow this as sort of the gold standard for naming.
    if (client_identity_ == client_fcrdns_)
      return Message::SpamStatus::ham;

    CDB white("white");

    if (lookup_domain(white, client_fcrdns_))
      return Message::SpamStatus::ham;

    char const* tld
        = tld_db_.get_registered_domain(client_fcrdns_.utf8().c_str());
    if (tld && white.lookup(tld))
      return Message::SpamStatus::ham;

    if (lookup_domain(white, client_identity_))
      return Message::SpamStatus::ham;

    char const* tld_client
        = tld_db_.get_registered_domain(client_identity_.utf8().c_str());
    if (tld_client && white.lookup(tld_client))
      return Message::SpamStatus::ham;

    return Message::SpamStatus::spam;
  }();

  // All sources of ham get a fresh 5 minute timeout per message.
  if (status == Message::SpamStatus::ham) {
    alarm(5 * 60);
  }

  msg.open(server_id(), max_msg_size(), status);
  auto hdrs = added_headers_(msg);
  msg.write(hdrs);
}

void Session::data_msg_done(Message& msg)
{
  msg.save();
  out_() << "250 2.0.0 OK\r\n" << std::flush;
  LOG(INFO) << "message delivered, " << msg.size() << " octets, with id "
            << msg.id();
}

void Session::data_size_error(Message& msg)
{
  msg.trash();
  out_() << "552 5.3.4 message size limit exceeded\r\n" << std::flush;
  LOG(WARNING) << "DATA size error";
}

bool Session::bdat_start()
{
  if (!reverse_path_verified_) {
    out_() << "503 5.5.1 need 'MAIL FROM' before 'BDAT'\r\n" << std::flush;
    LOG(ERROR) << "need 'MAIL FROM' before 'DATA'";
    return false;
  }
  if (forward_path_.empty()) {
    out_() << "503 5.5.1 need 'RCPT TO' before 'BDAT'\r\n" << std::flush;
    LOG(ERROR) << "no valid recipients";
    return false;
  }

  return true;
}

void Session::bdat_msg(Message& msg, size_t n)
{
  out_() << "250 2.0.0 OK " << n << " octets received\r\n" << std::flush;
  LOG(INFO) << "BDAT " << n;
}

void Session::bdat_msg_last(Message& msg, size_t n)
{
  msg.save();
  out_() << "250 2.0.0 OK " << n << " octets received\r\n" << std::flush;
  LOG(INFO) << "BDAT " << n << " LAST";
  LOG(INFO) << "message delivered, " << msg.size() << " octets, with id "
            << msg.id();
}

void Session::bdat_error(Message& msg)
{
  msg.trash();
  out_() << "503 5.5.1 BDAT sequence error\r\n" << std::flush;
  LOG(WARNING) << "DATA error";
}

void Session::rset()
{
  reset_();
  out_() << "250 2.0.0 OK\r\n";
  // No flush RFC-2920 section 3.1, this could be part of a command group.
  LOG(INFO) << "RSET";
}

void Session::noop(std::string_view str)
{
  last_in_group_("NOOP");
  out_() << "250 2.0.0 OK\r\n" << std::flush;
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
  out_() << "214 2.0.0 see https://digilicious.com/smtp.html and "
            "https://tools.ietf.org/html/rfc5321\r\n"
         << std::flush;
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
  syslog(LOG_MAIL | LOG_WARNING, "bad host [%s] auth", sock_.them_c_str());
  out_() << "454 4.7.0 authentication failure\r\n" << std::flush;
  exit_();
}

void Session::error(std::string_view log_msg)
{
  out_() << "421 4.3.5 system error\r\n" << std::flush;
  LOG(ERROR) << log_msg;
}

void Session::cmd_unrecognized(std::string_view cmd)
{
  auto escaped = esc(cmd);
  LOG(ERROR) << "command unrecognized: \"" << escaped << "\"";

  if (++n_unrecognized_cmds_ >= Config::max_unrecognized_cmds) {
    out_() << "500 5.5.1 command unrecognized: \"" << escaped
           << "\" exceeds limit\r\n"
           << std::flush;
    LOG(ERROR) << n_unrecognized_cmds_ << " unrecognized commands is too many";
    exit_();
  }

  out_() << "500 5.5.1 command unrecognized: \"" << escaped << "\"\r\n"
         << std::flush;
}

void Session::bare_lf()
{
  // Error code used by Office 365.
  out_() << "554 5.6.11 bare LF\r\n" << std::flush;
  LOG(ERROR) << "bare LF";
  exit_();
}

void Session::max_out()
{
  out_() << "552 5.3.4 message size exceeds maximium size\r\n" << std::flush;
  LOG(ERROR) << "message size maxed out";
  exit_();
}

void Session::time_out()
{
  out_() << "421 4.4.2 time-out\r\n" << std::flush;
  LOG(ERROR) << "time-out" << (sock_.has_peername() ? " from " : "") << client_;
  exit_();
}

void Session::starttls()
{
  last_in_group_("STARTTLS");
  if (sock_.tls()) {
    out_() << "554 5.5.1 TLS already active\r\n" << std::flush;
    LOG(ERROR) << "STARTTLS issued with TLS already active";
  }
  else {
    out_() << "220 2.0.0 go for TLS\r\n" << std::flush;
    sock_.starttls_server();
    reset_();

    max_msg_size(Config::max_msg_size_bro);

    LOG(INFO) << "STARTTLS " << sock_.tls_info();
  }
}

void Session::exit_()
{
  sock_.log_totals();

  timespec time_used;
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
  CDB black("ip-black");
  if (black.lookup(sock_.them_c_str())) {
    error_msg = sock_.them_address_literal() + " blacklisted";
    out_() << "554 5.7.1 on my personal blacklist\r\n" << std::flush;
    return false;
  }

  client_fcrdns_ = IP::fcrdns(sock_.them_c_str());

  if (!client_fcrdns_.empty()) {
    client_ = client_fcrdns_.ascii() + " " + sock_.them_address_literal();

    // check domain
    char const* tld
        = tld_db_.get_registered_domain(client_fcrdns_.utf8().c_str());
    CDB white("white");
    if (tld && white.lookup(tld)) {
      LOG(INFO) << "TLD domain " << tld << " whitelisted";
      fcrdns_whitelisted_ = true;
    }
    else {
      if (white.lookup(client_fcrdns_.utf8())) {
        LOG(INFO) << "domain " << client_fcrdns_ << " whitelisted";
        fcrdns_whitelisted_ = true;
      }
    }
  }
  else {
    client_ = "unknown "s + sock_.them_address_literal();
  }

  if ((sock_.them_address_literal() == "[127.0.0.1]")
      || (sock_.them_address_literal() == "[IPv6:::1]")) {
    ip_whitelisted_ = true;
  }
  else {
    CDB white("ip-white");
    if (white.lookup(sock_.them_c_str())) {
      LOG(INFO) << "IP address " << sock_.them_c_str() << " whitelisted";
      ip_whitelisted_ = true;
    }
    else if (IP4::is_address(sock_.them_c_str())) {
      using namespace DNS;

      // Check with black hole lists. <https://en.wikipedia.org/wiki/DNSBL>
      auto reversed = IP4::reverse(sock_.them_c_str());
      DNS::Resolver res;
      for (auto rbl : Config::rbls) {
        if (has_record<RR_type::A>(res, reversed + rbl)) {
          error_msg = sock_.them_address_literal() + " blocked by " + rbl;
          out_() << "554 5.7.1 blocked on advice from " << rbl << "\r\n"
                 << std::flush;
          return false;
        }
      }
      // LOG(INFO) << "IP address " << sock_.them_c_str() << " not
      // blacklisted";
    }
  }

  // Wait a bit of time for pre-greeting traffic.
  if (!(ip_whitelisted_ || fcrdns_whitelisted_)) {
    if (sock_.input_ready(Config::greeting_wait)) {
      error_msg = sock_.them_address_literal() + " input before greeting";
      out_() << "554 5.3.2 not accepting network messages\r\n" << std::flush;
      return false;
    }
  }

  return true;
}

bool Session::verify_client_(Domain const& client_identity,
                             std::string& error_msg)
// check the identity from the HELO/EHLO
{
  if (!sock_.has_peername() || ip_whitelisted_ || fcrdns_whitelisted_
      || client_identity.is_address_literal()) {
    return true;
  }

  // Bogus clients claim to be us or some local host.
  if ((client_identity == server_identity_) || (client_identity == "localhost")
      || (client_identity == "localhost.localdomain")) {

    if (server_identity_ != client_fcrdns_) {
      error_msg = "liar";
      out_() << "550 5.7.1 liar\r\n" << std::flush;
      return false;
    }
  }

  std::vector<std::string> labels;
  boost::algorithm::split(labels, client_identity.ascii(),
                          boost::algorithm::is_any_of("."));
  if (labels.size() < 2) {
    // Sometimes we may went to look at mail from misconfigured
    // sending systems.
    LOG(WARNING) << "invalid sender" << (sock_.has_peername() ? " " : "")
                 << client_ << " claiming " << client_identity;
    return true;
  }

  CDB black("black");
  if (lookup_domain(black, client_identity)) {
    std::stringstream em;
    em << "blacklisted identity " << client_identity;
    error_msg = em.str();
    out_() << "550 4.7.1 blacklisted identity\r\n" << std::flush;
    return false;
  }

  char const* tld
      = tld_db_.get_registered_domain(client_identity.ascii().c_str());
  if (tld) {
    if (client_identity == tld) {
      if (black.lookup(tld)) {
        error_msg = "blacklisted TLD "s + tld;
        out_() << "550 4.7.1 blacklisted TLD\r\n" << std::flush;
        return false;
      }
    }
  }

  return true;
}

bool Session::verify_recipient_(Mailbox const& recipient)
{
  if ((recipient.local_part() == "Postmaster") && (recipient.domain() == "")) {
    LOG(INFO) << "magic Postmaster address";
    return true;
  }

  auto accepted_domain = false;
  if (recipient.domain().is_address_literal()) {
    if (recipient.domain() == sock_.us_address_literal()) {
      accepted_domain = true;
    }
  }
  else {
    CDB accept_domains("accept_domains");
    if (accept_domains.open()) {
      if (accept_domains.lookup(recipient.domain().ascii())
          || accept_domains.lookup(recipient.domain().utf8())) {
        accepted_domain = true;
      }
    }
    else {
      // If we have no list of domains to accept, take our own.
      if (recipient.domain() == server_identity_) {
        accepted_domain = true;
      }
    }
  }

  if (!accepted_domain) {
    out_() << "554 5.7.1 relay access denied\r\n" << std::flush;
    LOG(WARNING) << "relay access denied for domain " << recipient.domain();
    return false;
  }

  // Check for local addresses we reject.
  for (auto bad_recipient : Config::bad_recipients) {
    if (0 == recipient.local_part().compare(bad_recipient)) {
      out_() << "550 5.1.1 bad recipient " << recipient << "\r\n" << std::flush;
      LOG(WARNING) << "bad recipient " << recipient;
      return false;
    }
  }

  return true;
}

bool Session::verify_sender_(Mailbox const& sender)
{
  if (sender.domain().is_address_literal()) {
    if (sender.domain() != sock_.them_address_literal()) {
      LOG(WARNING) << "sender domain " << sender.domain() << " does not match "
                   << sock_.them_address_literal();
    }
  }
  else {
    // If the reverse path domain matches the Forward-confirmed reverse
    // DNS of the sending IP address, we skip the uribl check.
    if (sender.domain() != client_fcrdns_) {
      if (!verify_sender_domain_(sender.domain()))
        return false;
    }

    if (sock_.has_peername()) {
      if (!verify_sender_spf_(sender))
        return false;
    }
  }

  return reverse_path_verified_ = true;
}

bool Session::verify_sender_domain_(Domain const& sender)
{
  if (sender.empty()) {
    // MAIL FROM:<>
    // is used to send bounce messages.
    return true;
  }

  std::string sndr_lc = sender.ascii();
  std::transform(sndr_lc.begin(), sndr_lc.end(), sndr_lc.begin(), ::tolower);

  // Break sender domain into labels:

  std::vector<std::string> labels;
  boost::algorithm::split(labels, sndr_lc, boost::algorithm::is_any_of("."));

  CDB white("white");
  if (white.lookup(sndr_lc)) {
    LOG(INFO) << "sender \"" << sndr_lc << "\" whitelisted";
    return true;
  }
  else if (sender.ascii() != sender.utf8()) {
    if (white.lookup(sender.utf8())) {
      LOG(INFO) << "sender \"" << sender.utf8() << "\" whitelisted";
      return true;
    }
  }

  if (labels.size() < 2) { // This is not a valid domain.
    out_() << "550 5.7.1 invalid sender domain " << sndr_lc << "\r\n"
           << std::flush;
    LOG(ERROR) << "sender \"" << sndr_lc << "\" invalid syntax";
    return false;
  }

  auto tld = tld_db_.get_registered_domain(sndr_lc.c_str());
  if (tld) {
    if (white.lookup(tld)) {
      LOG(INFO) << "sender TLD \"" << tld << "\" whitelisted";
      return true;
    }
  }

  // Based on <http://www.surbl.org/guidelines>

  auto two_level = labels[labels.size() - 2] + "." + labels[labels.size() - 1];

  if (labels.size() > 2) {
    auto three_level = labels[labels.size() - 3] + "." + two_level;

    CDB three_tld("three-level-tlds");
    if (three_tld.lookup(three_level)) {
      if (labels.size() > 3) {
        return verify_sender_domain_uribl_(labels[labels.size() - 4] + "."
                                           + three_level);
      }
      else {
        out_() << "554 5.7.1 bad sender domain\r\n" << std::flush;
        LOG(ERROR) << "sender \"" << sender
                   << "\" blocked by exact match on three-level-tlds list";
        return false;
      }
    }
  }

  CDB two_tld("two-level-tlds");
  if (two_tld.lookup(two_level)) {
    if (labels.size() > 2) {
      return verify_sender_domain_uribl_(labels[labels.size() - 3] + "."
                                         + two_level);
    }
    else {
      out_() << "554 5.7.1 bad sender domain\r\n" << std::flush;
      LOG(ERROR) << "sender \"" << sender
                 << "\" blocked by exact match on two-level-tlds list";
      return false;
    }
  }

  if (two_level.compare(tld)) {
    LOG(INFO) << "two level '" << two_level << "' != TLD '" << tld << "'";
  }

  return verify_sender_domain_uribl_(tld);
}

bool Session::verify_sender_domain_uribl_(std::string const& sender)
{
  DNS::Resolver res;
  for (auto uribl : Config::uribls) {
    if (DNS::has_record<DNS::RR_type::A>(res, (sender + ".") + uribl)) {
      out_() << "554 5.7.1 sender blocked on advice of " << uribl << "\r\n"
             << std::flush;
      LOG(ERROR) << sender << " blocked by " << uribl;
      return false;
    }
  }

  LOG(INFO) << sender << " cleared by URIBLs";
  return true;
}

bool Session::verify_sender_spf_(Mailbox const& sender)
{
  auto srvr_id = server_id();

  if (!sock_.has_peername() || ip_whitelisted_) {
    auto ip_addr = sock_.them_c_str();
    if (!sock_.has_peername()) {
      ip_addr = "127.0.0.1"; // use localhost for local socket
    }
    std::ostringstream received_spf;
    received_spf << "Received-SPF: pass (" << srvr_id << ": " << ip_addr
                 << " is whitelisted.) client-ip=" << ip_addr
                 << "; envelope-from=" << sender
                 << "; helo=" << client_identity_ << ";";
    received_spf_ = received_spf.str();
    return true;
  }

  auto sid = std::string(srvr_id.data(), srvr_id.length());
  SPF::Server spf_srv(sid.c_str());
  SPF::Request spf_req(spf_srv);

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

  auto from = static_cast<std::string>(sender);

  spf_req.set_env_from(from.c_str());

  SPF::Response spf_res(spf_req);

  if (spf_res.result() == SPF::Result::FAIL) {
    // Error code from RFC 7372, section 3.2.  Also:
    // <https://www.iana.org/assignments/smtp-enhanced-status-codes/smtp-enhanced-status-codes.xhtml>
    out_() << "550 5.7.23 " << spf_res.smtp_comment() << "\r\n" << std::flush;
    LOG(ERROR) << spf_res.header_comment();
    return false;
  }

  LOG(INFO) << spf_res.header_comment();
  received_spf_ = spf_res.received_spf();
  return true;
}
