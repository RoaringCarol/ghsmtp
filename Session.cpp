#include <algorithm>
#include <iostream>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

#include <sys/utsname.h>

#include "CDB.hpp"
#include "DNS.hpp"
#include "Domain.hpp"
#include "IP.hpp"
#include "IP4.hpp"
#include "IP6.hpp"
#include "Message.hpp"
#include "SPF.hpp"
#include "Session.hpp"

#include <boost/algorithm/string/case_conv.hpp>
#include <boost/algorithm/string/classification.hpp>
#include <boost/algorithm/string/split.hpp>
#include <boost/lexical_cast.hpp>

#include <syslog.h>

namespace Config {
constexpr char const* const accept_domains[] = {};

constexpr char const* const bad_recipients[] = {
    "a",         "ene",    "h.gene",   "jay",         "lizard",
    "mixmaster", "nobody", "oq6_2nbq", "truthfinder",
};

constexpr char const* const bad_senders[] = {};

constexpr char const* const rbls[] = {
    "zen.spamhaus.org", "b.barracudacentral.org",
};

constexpr char const* const uribls[] = {
    "dbl.spamhaus.org", "black.uribl.com", "multi.surbl.org",
};

constexpr auto greeting_wait_ms = 3'000;
constexpr auto max_recipients_per_message = 1'000;
}

using namespace std::string_literals;

Session::Session(int fd_in, int fd_out, std::string our_fqdn)
  : sock_(fd_in, fd_out)
{
  if (our_fqdn.empty()) {
    utsname un;
    PCHECK(uname(&un) == 0);
    our_fqdn = un.nodename;

    if (our_fqdn.find('.') == std::string::npos) {
      if (sock_.us_c_str()[0]) {
        our_fqdn = "["s + sock_.us_c_str() + "]"s;
      }
    }
  }
  our_fqdn_.set(our_fqdn.c_str());
}

void Session::greeting()
{
  if (sock_.has_peername()) {
    CDB black("ip-black");
    if (black.lookup(sock_.them_c_str())) {
      syslog(LOG_MAIL | LOG_WARNING, "bad host [%s] blacklisted",
             sock_.them_c_str());
      out() << "550 5.3.2 service currently unavailable\r\n" << std::flush;
      std::exit(EXIT_SUCCESS);
    }

    fcrdns_ = IP::fcrdns(sock_.them_c_str());

    if (!fcrdns_.empty()) {
      client_ = fcrdns_.ascii() + " "s + sock_.them_address_literal();
    }
    else {
      client_ = "unknown "s + sock_.them_address_literal();
    }

    CDB white("ip-white");
    if (white.lookup(sock_.them_c_str())) {
      LOG(INFO) << "IP address " << sock_.them_c_str() << " whitelisted";
      ip_whitelisted_ = true;
    }
    else if (IP4::is_address(sock_.them_c_str())) {
      using namespace DNS;
      Resolver res;

      // Check with black hole lists. <https://en.wikipedia.org/wiki/DNSBL>
      auto reversed = IP4::reverse(sock_.them_c_str());
      for (const auto& rbl : Config::rbls) {
        if (has_record<RR_type::A>(res, reversed + rbl)) {
          syslog(LOG_MAIL | LOG_WARNING, "bad host [%s] blocked by %s",
                 sock_.them_c_str(), rbl);
          out() << "554 5.7.1 blocked by " << rbl << "\r\n" << std::flush;
          std::exit(EXIT_SUCCESS);
        }
      }
      // LOG(INFO) << "IP address " << sock_.them_c_str() << " not
      // blacklisted";
    }

    // Wait a bit of time for pre-greeting traffic.
    std::chrono::milliseconds wait{Config::greeting_wait_ms};

    if (sock_.input_ready(wait)) {
      syslog(LOG_MAIL | LOG_WARNING, "bad host [%s] input before greeting",
             sock_.them_c_str());
      out() << "550 5.3.2 service currently unavailable\r\n" << std::flush;
      std::exit(EXIT_SUCCESS);
    }
  } // if (sock_.has_peername())

  out() << "220 " << our_fqdn_ << " ESMTP - ghsmtp\r\n" << std::flush;

  LOG(INFO) << "connect from " << client_;
}

void Session::ehlo(std::experimental::string_view client_identity)
{
  reset_();
  extensions_ = true;
  protocol_ = sock_.tls() ? "ESMTPS" : "ESMTP";
  client_identity_.set(client_identity);

  if (!verify_client_(client_identity_)) {
    syslog(LOG_MAIL | LOG_WARNING, "bad host [%s] verify_client_ fail",
           sock_.them_c_str());
    std::exit(EXIT_SUCCESS);
  }

  out() << "250-" << our_fqdn_ << "\r\n";
  // RFC 1870
  out() << "250-SIZE " << Config::max_msg_size << "\r\n";
  // RFC 6152
  out() << "250-8BITMIME\r\n";
  if (!sock_.tls()) { // If we're not already TLS, offer TLS
    // RFC 3207
    out() << "250-STARTTLS\r\n";
  }
  // RFC 2034
  out() << "250-ENHANCEDSTATUSCODES\r\n";
  // RFC 2920
  out() << "250-PIPELINING\r\n";
  // RFC 3030
  // out() << "250-BINARYMIME\r\n";
  out() << "250-CHUNKING\r\n";
  // RFC 6531
  out() << "250 SMTPUTF8\r\n" << std::flush;

  // Log this client
  if (sock_.has_peername()) {
    LOG(INFO) << "EHLO " << client_identity
              << ((fcrdns_ == client_identity_) ? "" : " **unmatched**")
              << " from " << client_;
  }
  else {
    LOG(INFO) << "EHLO " << client_identity;
  }
}

void Session::helo(std::experimental::string_view client_identity)
{
  reset_();
  extensions_ = false;
  protocol_ = sock_.tls() ? "ESMTPS" : "SMTP"; // there is no SMTPS
  client_identity_.set(client_identity);

  if (!verify_client_(client_identity_)) {
    syslog(LOG_MAIL | LOG_WARNING, "bad host [%s] verify_client_ fail",
           sock_.them_c_str());
    std::exit(EXIT_SUCCESS);
  }

  out() << "250 " << our_fqdn_ << "\r\n" << std::flush;

  // Log this client
  if (sock_.has_peername()) {
    LOG(INFO) << "HELO " << client_identity
              << ((fcrdns_ == client_identity_) ? "" : " **unmatched**")
              << " from " << client_;
  }
  else {
    LOG(INFO) << "HELO " << client_identity;
  }
}

void Session::mail_from(Mailbox&& reverse_path, parameters_t const& parameters)
{
  if (client_identity_.empty()) {
    out() << "503 5.5.1 'MAIL FROM' before 'HELO' or 'EHLO'\r\n" << std::flush;
    LOG(WARNING) << "'MAIL FROM' before 'HELO' or 'EHLO'"
                 << (sock_.has_peername() ? " from " : "") << client_;
    return;
  }

  bool smtputf8 = false;

  // Take a look at the optional parameters:
  for (auto const& p : parameters) {
    std::string name = p.first;
    std::transform(name.begin(), name.end(), name.begin(), ::toupper);

    std::string val = p.second;
    std::transform(val.begin(), val.end(), val.begin(), ::toupper);

    if (name == "BODY") {
      if (val == "8BITMIME") {
        // everything is cool, this is our default...
      }
      else if (val == "7BIT") {
        LOG(WARNING) << "7BIT transport requested";
      }
      else if (val == "BINARYMIME") {
        binarymime_ = true;
      }
      else {
        LOG(WARNING) << "unrecognized BODY type \"" << val << "\" requested";
      }
    }
    else if (name == "SMTPUTF8") {
      if (!val.empty()) {
        LOG(WARNING) << "SMTPUTF8 parameter has a value: " << val;
      }
      smtputf8 = true;
    }
    else if (name == "SIZE") {
      if (val.empty()) {
        LOG(WARNING) << "SIZE parameter has no value.";
      }
      else {
        try {
          size_t sz = stoull(val);
          if (sz > Config::max_msg_size) {
            out() << "552 5.3.4 message size exceeds fixed maximium message "
                     "size\r\n"
                  << std::flush;
            LOG(WARNING) << "SIZE parameter too large: " << sz;
            return;
          }
        }
        catch (std::invalid_argument const& e) {
          LOG(WARNING) << "SIZE parameter has invalid value: " << p.second;
        }
        catch (std::out_of_range const& e) {
          LOG(WARNING) << "SIZE parameter has out-of-range value: " << p.second;
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

  auto sender = static_cast<std::string>(reverse_path);

  for (const auto bad_sender : Config::bad_senders) {
    if (sender == bad_sender) {
      out() << "550 5.7.26 bad sender\r\n" << std::flush;
      LOG(WARNING) << "bad sender " << sender;
      return;
    }
  }

  if (verify_sender_(reverse_path)) {
    reverse_path_ = std::move(reverse_path);
    forward_path_.clear();
    out() << "250 2.1.0 mail ok\r\n" << std::flush;
    LOG(INFO) << "MAIL FROM:<" << reverse_path_ << ">";
  }
  else {
    syslog(LOG_MAIL | LOG_WARNING, "bad host [%s] verify_sender_ fail",
           sock_.them_c_str());
    std::exit(EXIT_SUCCESS);
  }
}

void Session::rcpt_to(Mailbox&& forward_path, parameters_t const& parameters)
{
  if (!reverse_path_verified_) {
    out() << "503 5.5.1 'RCPT TO' before 'MAIL FROM'\r\n" << std::flush;
    LOG(WARNING) << "'RCPT TO' before 'MAIL FROM'"
                 << (sock_.has_peername() ? " from " : "") << client_;
    return;
  }

  // Take a look at the optional parameters, we don't accept any:
  for (auto& p : parameters) {
    LOG(WARNING) << "unrecognized 'RCPT TO' parameter " << p.first << "="
                 << p.second;
  }

  if (verify_recipient_(forward_path)) {
    if (forward_path_.size() >= Config::max_recipients_per_message) {
      out() << "452 4.5.3 Too many recipients\r\n" << std::flush;
      LOG(WARNING) << "too many recipients <" << forward_path << ">";
    }
    else {
      forward_path_.push_back(std::move(forward_path));
      out() << "250 2.1.5 OK\r\n" << std::flush;
      LOG(INFO) << "RCPT TO:<" << forward_path_.back() << ">";
    }
  }
  // We're lenient on most bad recipients, no else/exit here.
}

bool Session::data_start()
{
  if (binarymime_) {
    out() << "503 5.5.1 DATA does not support BINARYMIME\r\n" << std::flush;
    LOG(ERROR) << "DATA does not support BINARYMIME";
    return false;
  }
  if (!reverse_path_verified_) {
    out() << "503 5.5.1 need 'MAIL FROM' before 'DATA'\r\n" << std::flush;
    LOG(ERROR) << "need 'MAIL FROM' before 'DATA'";
    return false;
  }
  if (forward_path_.empty()) {
    out() << "503 5.5.1 need 'RCPT TO' before 'DATA'\r\n" << std::flush;
    LOG(ERROR) << "no valid recipients";
    return false;
  }
  out() << "354 go, end with <CR><LF>.<CR><LF>\r\n" << std::flush;
  LOG(INFO) << "DATA";
  return true;
}

std::string Session::added_headers_(Message const& msg)
{
  // The headers Return-Path, Received, X-Original-To and Received-SPF
  // are returned as a string.

  std::ostringstream headers;
  headers << "Return-Path: <" << reverse_path_ << ">\r\n";

  // Received-SPF:
  if (!received_spf_.empty()) {
    headers << received_spf_ << "\r\n";
  }

  headers << "Received: from " << client_identity_;
  if (sock_.has_peername()) {
    headers << " (" << client_ << ')';
  }
  headers << "\r\n        by " << our_fqdn_ << " with " << protocol_ << " id "
          << msg.id();

  // STD 3 section 5.2.8
  if (forward_path_.size()) {
    auto len = 0;
    headers << "\r\n        for ";
    for (size_t i = 0; i < forward_path_.size(); ++i) {
      if (i) {
        headers << ',';
        if ((len + forward_path_[i].length()) > 80) {
          headers << "\r\n        ";
          len = 0;
        }
      }
      headers << '<' << forward_path_[i] << '>';
      len += forward_path_[i].length();
    }
  }

  std::string tls_info{sock_.tls_info()};
  if (tls_info.length()) {
    headers << "\r\n        (" << tls_info << ')';
  }
  headers << ";\r\n        " << msg.when() << "\r\n";

  return headers.str();
}

void Session::data_msg(Message& msg) // called /after/ {data/bdat}_start
{
  auto status = Message::SpamStatus::spam;

  // Anything enciphered tastes a lot like ham.
  if (sock_.tls()) {
    status = Message::SpamStatus::ham;
  }

  CDB white("white");

  if (!fcrdns_.empty()) {
    if (white.lookup(fcrdns_.utf8().c_str())) {
      status = Message::SpamStatus::ham;
    }

    char const* tld = tld_db_.get_registered_domain(fcrdns_.utf8().c_str());
    if (tld && white.lookup(tld)) {
      status = Message::SpamStatus::ham;
    }

    // I will allow this as sort of the gold standard for naming.
    if (client_identity_ == fcrdns_) {
      status = Message::SpamStatus::ham;
    }
  }

  if (white.lookup(client_identity_.utf8().c_str())) {
    status = Message::SpamStatus::ham;
  }

  char const* tld_client
      = tld_db_.get_registered_domain(client_identity_.utf8().c_str());
  if (tld_client && white.lookup(tld_client)) {
    status = Message::SpamStatus::ham;
  }

  msg.open(our_fqdn_.utf8(), status);
  auto hdrs = added_headers_(msg);
  msg.write(hdrs.data(), hdrs.size());
}

void Session::data_msg_done(Message& msg)
{
  msg.save();
  out() << "250 2.6.0 Message OK\r\n" << std::flush;
  LOG(INFO) << "message delivered, " << msg.count() << " octets, with id "
            << msg.id();
}

void Session::data_size_error(Message& msg)
{
  msg.trash();
  out() << "552 5.3.4 Channel size limit exceeded\r\n" << std::flush;
  LOG(WARNING) << "DATA size error";
}

bool Session::bdat_start()
{
  if (!reverse_path_verified_) {
    out() << "503 5.5.1 need 'MAIL FROM' before 'BDAT'\r\n" << std::flush;
    LOG(ERROR) << "need 'MAIL FROM' before 'DATA'";
    return false;
  }
  if (forward_path_.empty()) {
    out() << "503 5.5.1 need 'RCPT TO' before 'BDAT'\r\n" << std::flush;
    LOG(ERROR) << "no valid recipients";
    return false;
  }
  return true;
}

void Session::bdat_msg(Message& msg, size_t n)
{
  out() << "250 2.0.0 " << n << " octets received\r\n" << std::flush;
  LOG(INFO) << "BDAT " << n;
}

void Session::bdat_msg_last(Message& msg, size_t n)
{
  msg.save();
  out() << "250 2.6.0 Message OK\r\n" << std::flush;
  LOG(INFO) << "BDAT " << n << " LAST";
  LOG(INFO) << "message delivered, " << msg.count() << " octets, with id "
            << msg.id();
}

void Session::bdat_error(Message& msg)
{
  msg.trash();
  out() << "550 5.3.5 system error\r\n" << std::flush;
  LOG(WARNING) << "DATA error";
}

void Session::rset()
{
  reset_();
  out() << "250 2.0.0 OK\r\n" << std::flush;
  LOG(INFO) << "RSET";
}

void Session::noop()
{
  out() << "250 2.0.0 OK\r\n" << std::flush;
  LOG(INFO) << "NOOP";
}

void Session::vrfy()
{
  out() << "252 2.0.0 try it\r\n" << std::flush;
  LOG(INFO) << "VRFY";
}

void Session::help()
{
  out() << "214 2.0.0 see https://digilicious.com/smtp.html and "
           "https://tools.ietf.org/html/rfc5321\r\n"
        << std::flush;
  LOG(INFO) << "HELP";
}

void Session::quit()
{
  out() << "221 2.0.0 bye\r\n" << std::flush;
  LOG(INFO) << "QUIT";
  std::exit(EXIT_SUCCESS);
}

void Session::error(std::experimental::string_view log_msg)
{
  out() << "550 5.3.5 system error\r\n" << std::flush;
  LOG(ERROR) << log_msg;
}

void Session::cmd_unrecognized(std::experimental::string_view log_msg)
{
  out() << "502 5.5.1 command unrecognized\r\n" << std::flush;
  LOG(ERROR) << log_msg;
}

void Session::bare_lf(std::experimental::string_view log_msg)
{
  out() << "554 5.6.11 malformed data/bare LF or CR, see "
           "<https://cr.yp.to/docs/smtplf.html>\r\n"
        << std::flush;
  LOG(ERROR) << "Session::bare_lf: " << log_msg;
}

void Session::time_out()
{
  out() << "421 4.4.2 time-out\r\n" << std::flush;
  LOG(ERROR) << "time-out" << (sock_.has_peername() ? " from " : "") << client_;
  std::exit(EXIT_SUCCESS);
}

void Session::starttls()
{
  if (sock_.tls()) {
    out() << "554 5.5.1 TLS already active\r\n" << std::flush;
    LOG(ERROR) << "STARTTLS issued with TLS already active";
  }
  else {
    out() << "220 2.0.0 go for TLS\r\n" << std::flush;
    sock_.starttls();
    reset_();
    LOG(INFO) << "STARTTLS " << sock_.tls_info();
  }
}

/////////////////////////////////////////////////////////////////////////////

// All of the verify_* functions send their own error messages back to
// the client on failure, and return false.  The exception is the very
// bad recipient list that exits right away.

bool Session::verify_client_(Domain const& client_identity)
// check the identity from the HELO/EHLO
{
  if (client_identity.is_address_literal()) {
    return true;
  }

  // Bogus clients claim to be us or some local host.
  if ((client_identity == our_fqdn_) || (client_identity == "localhost")
      || (client_identity == "localhost.localdomain")) {

    if ((our_fqdn_ != fcrdns_)
        && (sock_.them_address_literal() != "[127.0.0.1]")
        && (sock_.them_address_literal() != "[IPv6:::1]")) {
      LOG(ERROR) << "liar: client" << (sock_.has_peername() ? " " : "")
                 << client_ << " claiming " << client_identity;
      out() << "550 5.7.1 liar\r\n" << std::flush;
      return false;
    }
  }

  std::string cid_lc = client_identity.ascii();
  std::transform(cid_lc.begin(), cid_lc.end(), cid_lc.begin(), ::tolower);

  std::vector<std::string> labels;
  boost::algorithm::split(labels, client_identity.ascii(),
                          boost::algorithm::is_any_of("."));

  if (labels.size() < 2) {
    LOG(ERROR) << "invalid sender" << (sock_.has_peername() ? " " : "")
               << client_ << " claiming " << client_identity;
    out() << "550 4.1.8 invalid sender system address\r\n" << std::flush;
    return false;
  }

  CDB black("black");
  if (black.lookup(cid_lc)) {
    LOG(ERROR) << "blacklisted identity" << (sock_.has_peername() ? " " : "")
               << client_ << " claiming " << cid_lc;
    out() << "550 4.7.1 blacklisted identity\r\n" << std::flush;
    return false;
  }
  else if (client_identity.ascii() != client_identity.utf8()) {
    if (black.lookup(client_identity.utf8())) {
      LOG(ERROR) << "blacklisted identity" << (sock_.has_peername() ? " " : "")
                 << client_ << " claiming " << client_identity.utf8();
      out() << "550 4.7.1 blacklisted identity\r\n" << std::flush;
      return false;
    }
  }
  else {
    LOG(INFO) << "unblack client identity " << client_identity;
  }

  char const* tld
      = tld_db_.get_registered_domain(client_identity.ascii().c_str());
  if (tld) {
    if (!Domain::match(cid_lc, tld)) {
      if (black.lookup(tld)) {
        LOG(ERROR) << "blacklisted TLD " << tld;
        out() << "550 4.7.0 blacklisted identity\r\n" << std::flush;
        return false;
      }
      else {
        LOG(INFO) << "unblack TLD " << tld;
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
  for (const auto d : Config::accept_domains) {
    if (recipient.domain() == d) {
      accepted_domain = true;
      break;
    }
  }

  if (recipient.domain().is_address_literal()) {
    if (recipient.domain() == sock_.us_address_literal()) {
      accepted_domain = true;
    }
  }

  // Make sure the domain matches.
  if (!accepted_domain && (recipient.domain() != our_fqdn_)) {
    out() << "554 5.7.1 relay access denied\r\n" << std::flush;
    LOG(WARNING) << "relay access denied for " << recipient;
    return false;
  }

  // Check for local addresses we reject.
  for (const auto bad_recipient : Config::bad_recipients) {
    if (0 == recipient.local_part().compare(bad_recipient)) {
      out() << "550 5.1.1 bad recipient " << recipient << "\r\n" << std::flush;
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
    if (sender.domain() != fcrdns_) {
      if (!verify_sender_domain_(sender.domain()))
        return false;
    }

    if (sock_.has_peername() && !ip_whitelisted_) {
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

  if (labels.size() < 2) { // This is not a valid domain.
    out() << "550 5.7.1 invalid sender domain " << sndr_lc << "\r\n"
          << std::flush;
    LOG(ERROR) << "sender \"" << sndr_lc << "\" invalid syntax";
    return false;
  }

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

  auto tld = tld_db_.get_registered_domain(sndr_lc.c_str());
  if (tld) {
    if (white.lookup(tld)) {
      LOG(INFO) << "sender tld \"" << tld << "\" whitelisted";
      return true;
    }
  }

  // Based on <http://www.surbl.org/guidelines>

  auto two_level = labels[labels.size() - 2] + "." + labels[labels.size() - 1];

  if (labels.size() > 2) {
    auto three_level = labels[labels.size() - 3] + "." + two_level;

    CDB three_tld("three-level-tlds");
    if (three_tld.lookup(three_level.c_str())) {
      if (labels.size() > 3) {
        return verify_sender_domain_uribl_(labels[labels.size() - 4] + "."
                                           + three_level);
      }
      else {
        out() << "550 4.7.1 bad sender domain\r\n" << std::flush;
        LOG(ERROR) << "sender \"" << sender
                   << "\" blocked by exact match on three-level-tlds list";
        return false;
      }
    }
  }

  CDB two_tld("two-level-tlds");
  if (two_tld.lookup(two_level.c_str())) {
    if (labels.size() > 2) {
      return verify_sender_domain_uribl_(labels[labels.size() - 3] + "."
                                         + two_level);
    }
    else {
      out() << "550 4.7.1 bad sender domain\r\n" << std::flush;
      LOG(ERROR) << "sender \"" << sender
                 << "\" blocked by exact match on two-level-tlds list";
      return false;
    }
  }

  if (two_level.compare(tld)) {
    LOG(INFO) << "two level " << two_level << " != tld " << tld;
  }

  return verify_sender_domain_uribl_(tld);
}

bool Session::verify_sender_domain_uribl_(std::string const& sender)
{
  DNS::Resolver res;
  for (const auto& uribl : Config::uribls) {
    if (DNS::has_record<DNS::RR_type::A>(res, (sender + ".") + uribl)) {
      out() << "550 4.7.1 blocked by " << uribl << "\r\n" << std::flush;
      LOG(ERROR) << sender << " blocked by " << uribl;
      return false;
    }
  }

  LOG(INFO) << sender << " cleared by URIBLs";
  return true;
}

bool Session::verify_sender_spf_(Mailbox const& sender)
{
  SPF::Server spf_srv(our_fqdn_.ascii().c_str());
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

  auto from = boost::lexical_cast<std::string>(sender);

  spf_req.set_env_from(from.c_str());

  SPF::Response spf_res(spf_req);

  if (spf_res.result() == SPF::Result::FAIL) {
    out() << "550 4.7.23 " << spf_res.smtp_comment() << "\r\n" << std::flush;
    LOG(ERROR) << spf_res.header_comment();
    return false;
  }

  LOG(INFO) << spf_res.header_comment();
  received_spf_ = spf_res.received_spf();
  return true;
}
