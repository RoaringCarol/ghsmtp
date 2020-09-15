#ifndef MESSAGE_DOT_HPP_INCLUDED
#define MESSAGE_DOT_HPP_INCLUDED

#include <iostream>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>

#include "Mailbox.hpp"
#include "fs.hpp"
#include "iequal.hpp"

namespace message {
struct header {
  header(std::string_view n, std::string_view v)
    : name(n)
    , value(v)
  {
  }

  std::string as_string() const;

  std::string_view as_view() const
  {
    return {name.begin(),
            static_cast<size_t>(std::distance(name.begin(), value.end()))};
  }

  bool operator==(std::string_view n) const { return iequal(n, name); }

  std::string_view name;
  std::string_view value;
}; // namespace header

struct parsed {
  bool parse(std::string_view input);
  bool parse_hdr(std::string_view input);

  std::string as_string() const;

  bool write(std::ostream& out) const;

  std::vector<header> headers;

  std::string_view get_header(std::string_view hdr) const;

  std::string_view field_name;
  std::string_view field_value;

  std::string_view body;

  // Parsing of the RFC-5322.From header
  std::vector<std::string> from_addrs;
  std::string              dmarc_from;
  std::string              dmarc_from_domain;

  // RFC5322.Reply
  std::string reply_to;

  // New RFC5322.From
  std::string new_22from;

  // New body
  std::string body_str;

  // New Authentication_Results field
  std::string ar_str;

  // New DKIM-Signature that includes above AR
  std::string sig_str;

  // Added ARC headers
  std::vector<std::string> arc_hdrs;
};

bool authentication(fs::path         config_path,
                    char const*      domain,
                    message::parsed& msg);

void dkim_check(fs::path config_path, char const* domain, message::parsed& msg);

void remove_delivery_headers(message::parsed& msg);

void rewrite(fs::path         config_path,
             Domain const&    sender,
             message::parsed& msg,
             std::string      mail_from,
             std::string      reply_to);

void print_spf_envelope_froms(char const* domain, message::parsed& msg);

} // namespace message

#endif // MESSAGE_DOT_HPP_INCLUDED
