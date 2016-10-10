/*
    This file is part of ghsmtp - Gene's simple SMTP server.
    Copyright (C) 2014  Gene Hightower <gene@digilicious.com>

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
#include "IP4.hpp"
#include "Message.hpp"
#include "SPF.hpp"
#include "Session.hpp"
#include "TLD.hpp"

#include <boost/algorithm/string/case_conv.hpp>
#include <boost/algorithm/string/classification.hpp>
#include <boost/algorithm/string/split.hpp>
#include <boost/lexical_cast.hpp>

namespace Config {
constexpr char const* const very_bad_recipients[] = {
    "a", "ene", "lizard", "oq6_2nbq",
};

constexpr char const* const bad_recipients[] = {
    "mixmaster", "nobody",
};

constexpr char const* const rbls[] = {
    "zen.spamhaus.org", "b.barracudacentral.org",
};

constexpr char const* const uribls[] = {
    "dbl.spamhaus.org", "black.uribl.com", "multi.surbl.org",
};

constexpr auto greeting_max_wait_ms = 10'000;
constexpr auto greeting_min_wait_ms = 500;

constexpr size_t size = 150 * 1024 * 1024;
}

Session::Session(int fd_in, int fd_out, std::string fqdn)
  : sock_(fd_in, fd_out)
  , fqdn_{std::move(fqdn)}
{
  if (fqdn_.empty()) {
    utsname un;
    PCHECK(uname(&un) == 0);
    fqdn_ = un.nodename;

    if (fqdn_.find('.') == std::string::npos) {
      if (sock_.us_c_str()[0]) {
        std::ostringstream ss;
        ss << "[" << sock_.us_c_str() << "]";
        fqdn_ = ss.str();
      }
    }
    else {
      std::transform(fqdn_.begin(), fqdn_.end(), fqdn_.begin(), ::tolower);
    }
  }
}

void Session::greeting()
{
  if (sock_.has_peername()) {

    CDB black("ip-black");
    if (black.lookup(sock_.them_c_str())) {
      LOG(ERROR) << "IP address " << sock_.them_c_str() << " blacklisted";
      std::exit(EXIT_SUCCESS);
    }

    LOG(INFO) << "connect from " << sock_.them_c_str();

    // This is just a teaser, the first line of a multi-line response.
    out_() << "220-" << fqdn_ << " ESMTP ghsmtp\r\n" << std::flush;

    using namespace DNS;
    Resolver res;

    // "0.1.2.3" => "3.2.1.0."
    std::string reversed{IP4::reverse(sock_.them_c_str())};

    // <https://en.wikipedia.org/wiki/Forward-confirmed_reverse_DNS>

    // The reverse part, check PTR records.
    std::vector<std::string> ptrs
        = get_records<RR_type::PTR>(res, reversed + "in-addr.arpa");

    char const* them = sock_.them_c_str();

    auto ptr = std::find_if(
        ptrs.begin(), ptrs.end(), [&res, them](std::string const& s) {
          // The forward part, check each PTR for matching A record.
          std::vector<std::string> addrs = get_records<RR_type::A>(res, s);
          return std::find(addrs.begin(), addrs.end(), them) != addrs.end();
        });

    if (ptr != ptrs.end()) {
      fcrdns_ = *ptr;
      client_ = fcrdns_ + " [" + sock_.them_c_str() + "]";
      LOG(INFO) << "connect from " << fcrdns_;
    }
    else {
      client_ = std::string("unknown [") + sock_.them_c_str() + "]";
    }

    CDB white("ip-white");
    if (white.lookup(sock_.them_c_str())) {
      LOG(INFO) << "IP address " << sock_.them_c_str() << " whitelisted";
    }
    else {
      // Check with black hole lists. <https://en.wikipedia.org/wiki/DNSBL>
      for (const auto& rbl : Config::rbls) {
        if (has_record<RR_type::A>(res, reversed + rbl)) {
          out_() << "421 blocked by " << rbl << "\r\n" << std::flush;
          LOG(ERROR) << client_ << " blocked by " << rbl;
          std::exit(EXIT_SUCCESS);
        }
      }
    }

    // Wait a (random) bit of time for pre-greeting traffic.
    std::uniform_int_distribution<> uni_dist(Config::greeting_min_wait_ms,
                                             Config::greeting_max_wait_ms);
    std::chrono::milliseconds wait{uni_dist(rd_)};

    if (sock_.input_ready(wait)) {
      out_() << "421 input before greeting\r\n" << std::flush;
      LOG(ERROR) << client_ << " input before greeting";
      std::exit(EXIT_SUCCESS);
    }
  } // if (sock_.has_peername())

  out_() << "220 " << fqdn_ << " ESMTP - ghsmtp\r\n" << std::flush;
}

void Session::ehlo(std::string client_identity)
{
  protocol_ = sock_.tls() ? "ESMTPS" : "ESMTP";
  if (verify_client_(client_identity)) {
    client_identity_ = std::move(client_identity);
    reset_();
    out_() << "250-" << fqdn_ << "\r\n";
    // out_() << "250-SIZE " << Config::size << "\r\n";
    out_() << "250-8BITMIME\r\n";
    if (!sock_.tls()) {
      out_() << "250-STARTTLS\r\n";
    }
    // out_() << "250-ENHANCEDSTATUSCODES\r\n";
    out_() << "250-PIPELINING\r\n";
    // out_() << "250-CHUNKING\r\n";
    out_() << "250 SMTPUTF8\r\n";
    out_() << std::flush;
  }
  else {
    std::exit(EXIT_SUCCESS);
  }
}

void Session::helo(std::string client_identity)
{
  protocol_ = "SMTP";
  if (verify_client_(client_identity)) {
    client_identity_ = std::move(client_identity);
    reset_();
    out_() << "250 " << fqdn_ << "\r\n" << std::flush;
  }
  else {
    std::exit(EXIT_SUCCESS);
  }
}

void Session::mail_from(Mailbox&& reverse_path, parameters_t const& parameters)
{
  if (client_identity_.empty()) {
    out_() << "503 'MAIL FROM' before 'HELO' or 'EHLO'\r\n" << std::flush;
    LOG(WARNING) << "'MAIL FROM' before 'HELO' or 'EHLO'"
                 << (sock_.has_peername() ? " from " : "") << client_;
    return;
  }

  // Take a look at the optional parameters:
  for (auto const& p : parameters) {
    if (p.first == "BODY") {
      if (p.second == "8BITMIME") {
        // everything is cool, this is our default...
      }
      else if (p.second == "7BIT") {
        LOG(WARNING) << "7BIT transport requested";
      }
      else if (p.second == "BINARYMIME") {
        LOG(WARNING) << "We don't support BINARYMIME yet";
      }
      else {
        LOG(WARNING) << "unrecognized BODY type \"" << p.second
                     << "\" requested";
      }
    }
    if (p.first == "SMTPUTF8") {
      if (p.second != "") {
        LOG(WARNING) << "SMTPUTF8 parameter has a value: " << p.second;
      }
    }
    else {
      LOG(WARNING) << "unrecognized MAIL FROM parameter " << p.first << "="
                   << p.second;
    }
  }

  if (verify_sender_(reverse_path)) {
    reset_();
    reverse_path_ = std::move(reverse_path);
    out_() << "250 mail ok\r\n" << std::flush;
  }
  else {
    std::exit(EXIT_SUCCESS);
  }
}

void Session::rcpt_to(Mailbox&& forward_path, parameters_t const& parameters)
{
  if (!reverse_path_verified_) {
    out_() << "503 'RCPT TO' before 'MAIL FROM'\r\n" << std::flush;
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
    forward_path_.push_back(std::move(forward_path));
    out_() << "250 rcpt ok\r\n" << std::flush;
    LOG(INFO) << "RCPT TO " << forward_path_.back();
  }
  // We're lenient on most bad recipients, no else/exit here.
}

bool Session::data_start()
{
  if (!reverse_path_verified_) {
    out_() << "503 need 'MAIL FROM' before 'DATA'\r\n" << std::flush;
    LOG(WARNING) << "need 'MAIL FROM' before 'DATA'";
    return false;
  }
  if (forward_path_.empty()) {
    out_() << "554 no valid recipients\r\n" << std::flush;
    LOG(WARNING) << "no valid recipients";
    return false;
  }
  out_() << "354 go\r\n" << std::flush;
  LOG(INFO) << "DATA";
  return true;
}

std::string Session::added_headers_(Message const& msg)
{
  // The headers Return-Path, X-Original-To and Received are returned
  // as a string.

  std::ostringstream headers;
  headers << "Return-Path: <" << reverse_path_ << ">\n";
  headers << "X-Original-To: <" << forward_path_[0] << ">\n";
  for (size_t i = 1; i < forward_path_.size(); ++i) {
    headers << "\t<" << forward_path_[i] << ">\n";
  }
  headers << "Received: from " << client_identity_;
  if (sock_.has_peername()) {
    headers << " (" << client_ << ")";
  }
  headers << "\n\tby " << fqdn_ << " with " << protocol_ << " id " << msg.id()
          << "\n\tfor <" << forward_path_[0] << '>';

  std::string tls_info{sock_.tls_info()};
  if (tls_info.length()) {
    headers << "\n\t(" << tls_info << ")";
  }
  headers << ";\n\t" << msg.when() << "\n";
  if (!received_spf_.empty()) {
    headers << received_spf_ << "\n";
  }

  return headers.str();
}

Message Session::data_msg() // called /after/ data_start
{
  Message msg(fqdn_);

  // The headers Return-Path, X-Original-To and Received are added to
  // the top of the message.

  msg.out() << added_headers_(msg);

  return msg;
}

void Session::data_msg_done(Message& msg)
{
  msg.save();
  LOG(INFO) << "message delivered with id " << msg.id();
  out_() << "250 data ok\r\n" << std::flush;
  LOG(INFO) << "end DATA";
}

void Session::rset()
{
  reset_();
  out_() << "250 ok\r\n" << std::flush;
  LOG(INFO) << "RSET";
}

void Session::noop()
{
  out_() << "250 nook\r\n" << std::flush;
  LOG(INFO) << "NOOP";
}

void Session::vrfy()
{
  out_() << "252 try it\r\n" << std::flush;
  LOG(INFO) << "VRFY";
}

void Session::help()
{
  out_() << "214-see https://digilicious.com/smtp.html\r\n"
            "214 and https://www.ietf.org/rfc/rfc5321.txt\r\n"
         << std::flush;
  LOG(INFO) << "HELP";
}

void Session::quit()
{
  out_() << "221 bye\r\n" << std::flush;
  LOG(INFO) << "QUIT";
  std::exit(EXIT_SUCCESS);
}

void Session::error(std::experimental::string_view msg)
{
  out_() << "500 command unrecognized\r\n" << std::flush;
  LOG(WARNING) << msg;
}

void Session::time()
{
  out_() << "421 timeout\r\n" << std::flush;
  LOG(ERROR) << "timeout" << (sock_.has_peername() ? " from " : "") << client_;
  std::exit(EXIT_SUCCESS);
}

void Session::starttls()
{
  if (sock_.tls()) {
    out_() << "554 TLS already active\r\n" << std::flush;
    LOG(WARNING) << "STARTTLS issued with TLS already active";
  }
  else {
    out_() << "220 go ahead\r\n" << std::flush;
    sock_.starttls();
    LOG(INFO) << "STARTTLS " << sock_.tls_info();
  }
}

/////////////////////////////////////////////////////////////////////////////

// All of the verify_* functions send their own error messages back to
// the client on failure, and return false.  The exception is the very
// bad recipient list that exits right away.

bool Session::verify_client_(std::string const& client_identity)
{
  if (IP4::is_address(client_identity)
      && (client_identity != sock_.them_c_str())) {
    LOG(WARNING) << "client claiming questionable IP address "
                 << client_identity;
  }

  // Bogus clients claim to be us or some local host.
  if (Domain::match(client_identity, fqdn_)
      || Domain::match(client_identity, "localhost")
      || Domain::match(client_identity, "localhost.localdomain")) {

    if (!Domain::match(fqdn_, fcrdns_)
        && strcmp(sock_.them_c_str(), "127.0.0.1")) {
      out_() << "421 liar\r\n" << std::flush;
      LOG(ERROR) << "liar: client" << (sock_.has_peername() ? " " : "")
                 << client_ << " claiming " << client_identity;
      return false;
    }
  }

  std::vector<std::string> labels;
  boost::algorithm::split(labels, client_identity,
                          boost::algorithm::is_any_of("."));

  if (labels.size() < 2) {
    out_() << "421 invalid sender\r\n" << std::flush;
    LOG(ERROR) << "invalid sender" << (sock_.has_peername() ? " " : "")
               << client_ << " claiming " << client_identity;
    return false;
  }

  CDB black("black");
  if (black.lookup(client_identity)) {
    out_() << "421 blacklisted identity\r\n" << std::flush;
    LOG(ERROR) << "blacklisted identity" << (sock_.has_peername() ? " " : "")
               << client_ << " claiming " << client_identity;
    return false;
  }

  TLD tld_db;
  char const* tld = tld_db.get_registered_domain(client_identity.c_str());
  if (!tld) {
    tld = client_identity.c_str();
  }
  if (black.lookup(tld)) {
    out_() << "421 blacklisted identity\r\n" << std::flush;
    LOG(ERROR) << "blacklisted TLD" << (sock_.has_peername() ? " " : "")
               << client_ << " claiming " << client_identity;
    return false;
  }

  // Log this client
  if (sock_.has_peername()) {
    if (Domain::match(fcrdns_, client_identity)) {
      LOG(INFO) << protocol_ << " connection from " << client_;
    }
    else {
      LOG(INFO) << protocol_ << " connection from " << client_ << " claiming "
                << client_identity;
    }
  }
  else {
    LOG(INFO) << protocol_ << " connection claiming " << client_identity;
  }

  return true;
}

bool Session::verify_recipient_(Mailbox const& recipient)
{
  // Make sure the domain matches.
  if (!Domain::match(recipient.domain(), fqdn_)) {
    out_() << "554 relay access denied\r\n" << std::flush;
    LOG(WARNING) << "relay access denied for " << recipient;
    return false;
  }

  // Check for local addresses we reject.
  for (const auto bad_recipient : Config::bad_recipients) {
    if (0 == recipient.local_part().compare(bad_recipient)) {
      out_() << "550 no such mailbox " << recipient << "\r\n" << std::flush;
      LOG(WARNING) << "no such mailbox " << recipient;
      return false;
    }
  }

  // Check for local addresses we reject with prejudice.
  for (const auto very_bad_recipient : Config::very_bad_recipients) {
    if (0 == recipient.local_part().compare(very_bad_recipient)) {
      out_() << "421 very bad recipient " << recipient << "\r\n" << std::flush;
      LOG(ERROR) << "very bad recipient " << recipient;
      std::exit(EXIT_SUCCESS);
    }
  }

  return true;
}

bool Session::verify_sender_(Mailbox const& sender)
{
  // If the reverse path domain matches the Forward-confirmed reverse
  // DNS of the sending IP address, we skip the uribl check.
  if (!Domain::match(sender.domain(), fcrdns_)) {
    if (!verify_sender_domain_(sender.domain()))
      return false;
  }

  if (sock_.has_peername()) {
    if (!verify_sender_spf_(sender))
      return false;
  }

  return reverse_path_verified_ = true;
}

bool Session::verify_sender_domain_(std::string const& sender)
{
  if (0 == sender.length()) {
    // MAIL FROM:<>
    // is used to send bounce messages.
    return true;
  }

  std::string domain = sender;
  std::transform(domain.begin(), domain.end(), domain.begin(), ::tolower);

  // Break sender domain into labels:

  std::vector<std::string> labels;
  boost::algorithm::split(labels, domain, boost::algorithm::is_any_of("."));

  if (labels.size() < 2) { // This is not a valid domain.
    out_() << "421 invalid sender domain " << domain << "\r\n" << std::flush;
    LOG(ERROR) << "sender \"" << domain << "\" invalid syntax";
    return false;
  }

  CDB white("white");
  if (white.lookup(domain)) {
    LOG(INFO) << "sender \"" << domain << "\" whitelisted";
    return true;
  }

  TLD tld_db;
  char const* tld = tld_db.get_registered_domain(domain.c_str());
  if (!tld) {
    tld = domain.c_str(); // If ingoing domain is a TLD.
  }
  if (white.lookup(tld)) {
    LOG(INFO) << "sender tld \"" << tld << "\" whitelisted";
    return true;
  }

  // Based on <http://www.surbl.org/guidelines>

  std::string two_level
      = labels[labels.size() - 2] + "." + labels[labels.size() - 1];

  if (labels.size() > 2) {
    std::string three_level = labels[labels.size() - 3] + "." + two_level;

    CDB three_tld("three-level-tlds");
    if (three_tld.lookup(three_level.c_str())) {
      if (labels.size() > 3) {
        return verify_sender_domain_uribl_(labels[labels.size() - 4] + "."
                                           + three_level);
      }
      else {
        out_() << "421 bad sender domain\r\n" << std::flush;
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
      out_() << "421 bad sender domain\r\n" << std::flush;
      LOG(ERROR) << "sender \"" << sender
                 << "\" blocked by exact match on two-level-tlds list";
      return false;
    }
  }

  if (two_level.compare(tld)) {
    LOG(WARNING) << "two level " << two_level << " != tld " << tld;
  }

  return verify_sender_domain_uribl_(tld);
}

bool Session::verify_sender_domain_uribl_(std::string const& sender)
{
  DNS::Resolver res;
  for (const auto& uribl : Config::uribls) {
    if (DNS::has_record<DNS::RR_type::A>(res, (sender + ".") + uribl)) {
      out_() << "421 blocked by " << uribl << "\r\n" << std::flush;
      LOG(ERROR) << sender << " blocked by " << uribl;
      return false;
    }
  }

  LOG(INFO) << sender << " cleared by URIBLs";
  return true;
}

bool Session::verify_sender_spf_(Mailbox const& sender)
{
  SPF::Server spf_srv(fqdn_.c_str());
  SPF::Request spf_req(spf_srv);

  spf_req.set_ipv4_str(sock_.them_c_str());
  spf_req.set_helo_dom(client_identity_.c_str());

  auto from = boost::lexical_cast<std::string>(sender);

  spf_req.set_env_from(from.c_str());

  SPF::Response spf_res(spf_req);

  if (spf_res.result() == SPF::Result::FAIL) {
    out_() << "421 " << spf_res.smtp_comment() << std::flush;
    LOG(ERROR) << spf_res.header_comment();
    return false;
  }

  LOG(INFO) << spf_res.header_comment();
  received_spf_ = spf_res.received_spf();
  return true;
}
