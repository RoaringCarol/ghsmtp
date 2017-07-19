#ifndef SESSION_DOT_HPP
#define SESSION_DOT_HPP

#include <random>
#include <string>
#include <unordered_map>
#include <vector>

#include <experimental/string_view>

#include "DNS.hpp"
#include "Domain.hpp"
#include "Mailbox.hpp"
#include "Message.hpp"
#include "Sock.hpp"
#include "TLD.hpp"

class Session {
public:
  using parameters_t = std::unordered_map<std::string, std::string>;

  Session(Session const&) = delete;
  Session& operator=(Session const&) = delete;

  explicit Session(std::function<void(void)> read_hook = []() {},
                   int fd_in = STDIN_FILENO,
                   int fd_out = STDOUT_FILENO,
                   std::string fqdn = "");

  void greeting();
  void ehlo(std::experimental::string_view client_identity);
  void helo(std::experimental::string_view client_identity);
  void mail_from(Mailbox&& reverse_path, parameters_t const& parameters);
  void rcpt_to(Mailbox&& forward_path, parameters_t const& parameters);

  bool data_start();
  void data_msg(Message& msg);
  void data_msg_done(Message& msg);
  void data_size_error(Message& msg);

  bool bdat_start();
  void bdat_msg(Message& msg, size_t n);
  void bdat_msg_last(Message& msg, size_t n);
  void bdat_error(Message& msg);

  void rset();
  void noop(std::experimental::string_view str);
  void vrfy(std::experimental::string_view str);
  void help(std::experimental::string_view str);
  void quit() __attribute__((noreturn));
  void error(std::experimental::string_view log_msg);
  void cmd_unrecognized(std::experimental::string_view log_msg);
  void bare_lf(std::experimental::string_view log_msg);

  void max_out() __attribute__((noreturn));
  void time_out() __attribute__((noreturn));
  void starttls();

  bool maxed_out() { return sock_.maxed_out(); }
  bool timed_out() { return sock_.timed_out(); }
  std::istream& in() { return sock_.in(); }

  void flush();

private:
  friend struct Session_test;

  std::string added_headers_(Message const& msg);

  std::ostream& out_() { return sock_.out(); }
  void log_lo_(char const* verb,
               std::experimental::string_view client_identity) const;

  std::experimental::string_view server_id() const;

  void reset_()
  {
    reverse_path_.clear();
    forward_path_.clear();
    protocol_ = "";
    binarymime_ = false;
    extensions_ = false;
    reverse_path_verified_ = false;
  }

  bool verify_client_(Domain const& client_identity);
  bool verify_recipient_(Mailbox const& recipient);
  bool verify_sender_(Mailbox const& sender);
  bool verify_sender_domain_(Domain const& sender);
  bool verify_sender_domain_uribl_(std::string const& sender);
  bool verify_sender_spf_(Mailbox const& sender);

  void exit_() __attribute__((noreturn));

private:
  Sock sock_;

  DNS::Resolver res_;

  Domain our_fqdn_;                   // who we identify as
  Domain fcrdns_;                     // who they look-up as
  std::string client_;                // (fcrdns_ [sock_.them_c_str()])
  Domain client_identity_;            // from ehlo/helo
  Mailbox reverse_path_;              // "mail from"
  std::vector<Mailbox> forward_path_; // for each "rcpt to"

  std::string received_spf_; // from libspf2

  char const* protocol_{""};

  std::random_device rd_;

  TLD tld_db_;

  bool binarymime_{false};
  bool extensions_{false};
  bool ip_whitelisted_{false};
  bool reverse_path_verified_{false};
};

#endif // SESSION_DOT_HPP
