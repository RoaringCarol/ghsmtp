#include <fstream>

#include "Session.hpp"

#include <cstdlib>
#include <memory>

#include <tao/pegtl.hpp>
#include <tao/pegtl/contrib/abnf.hpp>
#include <tao/pegtl/contrib/alphabet.hpp>

using namespace tao::pegtl;
using namespace tao::pegtl::abnf;
using namespace tao::pegtl::alphabet;

using namespace std::string_literals;

using std::experimental::string_view;

namespace Config {
constexpr std::streamsize bfr_size = 4 * 1024;
constexpr std::streamsize max_chunk_size = max_msg_size;
constexpr std::streamsize max_hdr_size = 16 * 1024;
constexpr std::streamsize max_xfer_size = 64 * 1024;
}

namespace RFC5321 {

struct Ctx {
  Session session;

  std::unique_ptr<Message> msg;

  std::string hdr;

  std::string mb_loc;
  std::string mb_dom;

  std::pair<std::string, std::string> param;
  std::unordered_map<std::string, std::string> parameters;

  size_t chunk_size;
  bool chunk_first{true};
  bool chunk_last{false};
  bool bdat_error{false};

  bool hdr_end{false};
  bool hdr_parsed{false};

  void bdat_rset()
  {
    chunk_first = true;
    chunk_last = false;
    bdat_error = false;
    hdr_end = false;
    hdr_parsed = false;
  }

  void new_msg()
  {
    msg = std::make_unique<Message>();
    hdr.clear();
    hdr_end = false;
    hdr_parsed = false;
    session.data_msg(*msg);
  }

  bool hdr_parse()
  {
    if (hdr_parsed)
      return true;

    if (!hdr_end) {
      LOG(ERROR) << "may not have whole header";
      return false;
    }
    if (hdr.size() > Config::max_hdr_size) {
      LOG(ERROR) << "header size too large";
      return false;
    }

    // parse header

    return hdr_parsed = true;
  }
};

struct no_last_dash { // not used now...
  // Something like this to fix up U_ldh_str and Ldh_str rules.
  template <tao::pegtl::apply_mode A,
            tao::pegtl::rewind_mode M,
            template <typename...> class Action,
            template <typename...> class Control,
            typename Input>
  static bool match(Input& in)
  {
    if (in.string().back() != '-') {
      in.bump(in.string().size());
      return true;
    }
    return false;
  }
};

struct UTF8_tail : range<0x80, 0xBF> {
};

struct UTF8_1 : range<0x00, 0x7F> {
};

struct UTF8_2 : seq<range<0xC2, 0xDF>, UTF8_tail> {
};

struct UTF8_3 : sor<seq<one<0xE0>, range<0xA0, 0xBF>, UTF8_tail>,
                    seq<range<0xE1, 0xEC>, rep<2, UTF8_tail>>,
                    seq<one<0xED>, range<0x80, 0x9F>, UTF8_tail>,
                    seq<range<0xEE, 0xEF>, rep<2, UTF8_tail>>> {
};

struct UTF8_4 : sor<seq<one<0xF0>, range<0x90, 0xBF>, rep<2, UTF8_tail>>,
                    seq<range<0xF1, 0xF3>, rep<3, UTF8_tail>>,
                    seq<one<0xF4>, range<0x80, 0x8F>, rep<2, UTF8_tail>>> {
};

// UTF8_char = UTF8_1 | UTF8_2 | UTF8_3 | UTF8_4;

struct UTF8_non_ascii : sor<UTF8_2, UTF8_3, UTF8_4> {
};

struct quoted_pair : seq<one<'\\'>, sor<VCHAR, WSP>> {
};

using dot = one<'.'>;
using colon = one<':'>;
using dash = one<'-'>;

struct u_let_dig : sor<ALPHA, DIGIT, UTF8_non_ascii> {
};

struct u_ldh_str : plus<sor<ALPHA, DIGIT, UTF8_non_ascii, dash>> {
  // verify last char is a U_Let_dig
};

struct u_label : seq<u_let_dig, opt<u_ldh_str>> {
};

struct let_dig : sor<ALPHA, DIGIT> {
};

struct ldh_str : plus<sor<ALPHA, DIGIT, dash>> {
  // verify last char is a U_Let_dig
};

struct label : seq<let_dig, opt<ldh_str>> {
};

struct sub_domain : sor<label, u_label> {
};

struct domain : list<sub_domain, dot> {
};

// clang-format off
struct dec_octet : sor<one<'0'>,
                       rep_min_max<1, 2, DIGIT>,
                       seq<one<'1'>, DIGIT, DIGIT>,
                       seq<one<'2'>, range<'0', '4'>, DIGIT>,
                       seq<string<'2','5'>, range<'0','5'>>> {};
// clang-format on

struct IPv4_address_literal
    : seq<dec_octet, dot, dec_octet, dot, dec_octet, dot, dec_octet> {
};

struct h16 : rep_min_max<1, 4, HEXDIG> {
};

struct ls32 : sor<seq<h16, colon, h16>, IPv4_address_literal> {
};

struct dcolon : two<':'> {
};

// clang-format off
struct IPv6address : sor<seq<                                          rep<6, h16, colon>, ls32>,
                         seq<                                  dcolon, rep<5, h16, colon>, ls32>,
                         seq<opt<h16                        >, dcolon, rep<4, h16, colon>, ls32>, 
                         seq<opt<h16,     opt<   colon, h16>>, dcolon, rep<3, h16, colon>, ls32>,
                         seq<opt<h16, rep_opt<2, colon, h16>>, dcolon, rep<2, h16, colon>, ls32>,
                         seq<opt<h16, rep_opt<3, colon, h16>>, dcolon,        h16, colon,  ls32>,
                         seq<opt<h16, rep_opt<4, colon, h16>>, dcolon,                     ls32>,
                         seq<opt<h16, rep_opt<5, colon, h16>>, dcolon,                      h16>,
                         seq<opt<h16, rep_opt<6, colon, h16>>, dcolon                          >> {};
// clang-format on

struct IPv6_address_literal : seq<TAOCPP_PEGTL_STRING("IPv6:"), IPv6address> {
};

struct dcontent : ranges<33, 90, 94, 126> {
};

struct standardized_tag : ldh_str {
};

struct General_address_literal : seq<standardized_tag, colon, plus<dcontent>> {
};

// See rfc 5321 Section 4.1.3
struct address_literal : seq<one<'['>,
                             sor<IPv4_address_literal,
                                 IPv6_address_literal,
                                 General_address_literal>,
                             one<']'>> {
};

struct at_domain : seq<one<'@'>, domain> {
};

struct a_d_l : list<at_domain, one<','>> {
};

struct qtextSMTP : sor<ranges<32, 33, 35, 91, 93, 126>, UTF8_non_ascii> {
};

struct graphic : range<32, 126> {
};

struct quoted_pairSMTP : seq<one<'\\'>, graphic> {
};

struct qcontentSMTP : sor<qtextSMTP, quoted_pairSMTP> {
};

struct quoted_string : seq<one<'"'>, star<qcontentSMTP>, one<'"'>> {
};

// clang-format off
struct atext : sor<ALPHA, DIGIT,
                   one<'!'>, one<'#'>,
                   one<'$'>, one<'%'>,
                   one<'&'>, one<'\''>,
                   one<'*'>, one<'+'>,
                   one<'-'>, one<'/'>,
                   one<'='>, one<'?'>,
                   one<'^'>, one<'_'>,
                   one<'`'>, one<'{'>,
                   one<'|'>, one<'}'>,
                   one<'~'>,
                   UTF8_non_ascii> {
};
// clang-format on

struct atom : plus<atext> {
};

struct dot_string : list<atom, dot> {
};

struct local_part : sor<dot_string, quoted_string> {
};

struct non_local_part : sor<domain, address_literal> {
};

struct mailbox : seq<local_part, one<'@'>, non_local_part> {
};

struct path : seq<one<'<'>, seq<opt<seq<a_d_l, colon>>, mailbox, one<'>'>>> {
};

struct bounce_path : TAOCPP_PEGTL_STRING("<>") {
};

struct reverse_path : sor<path, bounce_path> {
};

struct magic_postmaster : TAOCPP_PEGTL_STRING("<Postmaster>") {
};

struct forward_path : sor<path, magic_postmaster> {
};

struct esmtp_keyword : seq<sor<ALPHA, DIGIT>, star<sor<ALPHA, DIGIT, dash>>> {
};

struct esmtp_value : plus<sor<range<33, 60>, range<62, 126>, UTF8_non_ascii>> {
};

struct esmtp_param : seq<esmtp_keyword, opt<seq<one<'='>, esmtp_value>>> {
};

struct mail_parameters : list<esmtp_param, SP> {
};

struct rcpt_parameters : list<esmtp_param, SP> {
};

struct string : sor<quoted_string, atom> {
};

struct helo : seq<TAOCPP_PEGTL_ISTRING("HELO"), SP, domain, CRLF> {
};

struct ehlo : seq<TAOCPP_PEGTL_ISTRING("EHLO"),
                  SP,
                  sor<domain, address_literal>,
                  CRLF> {
};

struct mail_from : seq<TAOCPP_PEGTL_ISTRING("MAIL"),
                       TAOCPP_PEGTL_ISTRING(" FROM:"),
                       opt<SP>, // obsolete in RFC5321, but kosher in RFC821
                       reverse_path,
                       opt<seq<SP, mail_parameters>>,
                       CRLF> {
};

struct rcpt_to : seq<TAOCPP_PEGTL_ISTRING("RCPT"),
                     TAOCPP_PEGTL_ISTRING(" TO:"),
                     opt<SP>, // obsolete in RFC5321, but kosher in RFC821
                     forward_path,
                     opt<seq<SP, rcpt_parameters>>,
                     CRLF> {
};

struct chunk_size : plus<DIGIT> {
};

struct end_marker : TAOCPP_PEGTL_ISTRING(" LAST") {
};

struct bdat : seq<TAOCPP_PEGTL_ISTRING("BDAT"), SP, chunk_size, CRLF> {
};

struct bdat_last
    : seq<TAOCPP_PEGTL_ISTRING("BDAT"), SP, chunk_size, end_marker, CRLF> {
};

struct data : seq<TAOCPP_PEGTL_ISTRING("DATA"), CRLF> {
};

struct data_end : seq<dot, CRLF> {
};

struct data_blank : CRLF {
};

struct data_dot : seq<one<'.'>, plus<not_one<'\r', '\n'>>, CRLF> {
};

struct data_plain : seq<not_one<'.'>, star<not_one<'\r', '\n'>>, CRLF> {
};

struct data_line : sor<data_blank, data_dot, data_plain> {
};

struct anything_else : seq<rep_min_max<1, Config::bfr_size, any>, eof> {
};

struct data_grammar
    : sor<seq<star<seq<data_line, discard>>, data_end>, anything_else> {
};

struct rset : seq<TAOCPP_PEGTL_ISTRING("RSET"), CRLF> {
};

struct noop : seq<TAOCPP_PEGTL_ISTRING("NOOP"), opt<seq<SP, string>>, CRLF> {
};

struct vrfy : seq<TAOCPP_PEGTL_ISTRING("VRFY"), opt<seq<SP, string>>, CRLF> {
};

struct help : seq<TAOCPP_PEGTL_ISTRING("HELP"), opt<seq<SP, string>>, CRLF> {
};

struct starttls
    : seq<TAOCPP_PEGTL_ISTRING("STAR"), TAOCPP_PEGTL_ISTRING("TTLS"), CRLF> {
};

struct quit : seq<TAOCPP_PEGTL_ISTRING("QUIT"), CRLF> {
};

struct bogus_cmd_0 : CRLF {
};
struct bogus_cmd_1 : seq<not_one<'\r', '\n'>, CRLF> {
};
struct bogus_cmd_2 : seq<not_one<'\r', '\n'>, not_one<'\r', '\n'>, CRLF> {
};
struct bogus_cmd_3
    : seq<not_one<'\r', '\n'>, not_one<'\r', '\n'>, not_one<'\r', '\n'>, CRLF> {
};
struct bogus_cmd : seq<star<not_one<'\r', '\n'>>, CRLF> {
};

// commands in size order

struct any_cmd : seq<sor<bogus_cmd_0,
                         bogus_cmd_1,
                         bogus_cmd_2,
                         bogus_cmd_3,
                         data,
                         quit,
                         rset,
                         noop,
                         vrfy,
                         help,
                         helo,
                         ehlo,
                         bdat,
                         bdat_last,
                         starttls,
                         rcpt_to,
                         mail_from,
                         bogus_cmd,
                         anything_else>,
                     discard> {
};

struct grammar : plus<any_cmd> {
};

template <typename Rule>
struct action : nothing<Rule> {
};

template <typename Rule>
struct data_action : nothing<Rule> {
};

template <>
struct action<esmtp_keyword> {
  template <typename Input>
  static void apply(Input const& in, Ctx& ctx)
  {
    ctx.param.first = in.string();
  }
};

template <>
struct action<bogus_cmd_1> {
  template <typename Input>
  static void apply(Input const& in, Ctx& ctx)
  {
    ctx.session.cmd_unrecognized("bogus command: \""s + in.string() + "\""s);
  }
};

template <>
struct action<bogus_cmd_2> {
  template <typename Input>
  static void apply(Input const& in, Ctx& ctx)
  {
    ctx.session.cmd_unrecognized("bogus command: \""s + in.string() + "\""s);
  }
};

template <>
struct action<bogus_cmd_3> {
  template <typename Input>
  static void apply(Input const& in, Ctx& ctx)
  {
    ctx.session.cmd_unrecognized("bogus command: \""s + in.string() + "\""s);
  }
};

template <>
struct action<bogus_cmd> {
  template <typename Input>
  static void apply(Input const& in, Ctx& ctx)
  {
    ctx.session.cmd_unrecognized("bogus command: \""s + in.string() + "\""s);
  }
};

template <>
struct action<anything_else> {
  template <typename Input>
  static void apply(Input const& in, Ctx& ctx)
  {
    ctx.session.cmd_unrecognized("garbage in cmd stream: \""s + in.string()
                                 + "\""s);
  }
};

template <>
struct action<esmtp_value> {
  template <typename Input>
  static void apply(Input const& in, Ctx& ctx)
  {
    ctx.param.second = in.string();
  }
};

template <>
struct action<esmtp_param> {
  template <typename Input>
  static void apply(Input const& in, Ctx& ctx)
  {
    ctx.parameters.insert(ctx.param);
    ctx.param.first.clear();
    ctx.param.second.clear();
  }
};

template <>
struct action<local_part> {
  template <typename Input>
  static void apply(Input const& in, Ctx& ctx)
  {
    ctx.mb_loc = in.string();
  }
};

template <>
struct action<non_local_part> {
  template <typename Input>
  static void apply(Input const& in, Ctx& ctx)
  {
    ctx.mb_dom = in.string();
  }
};

template <>
struct action<magic_postmaster> {
  static void apply0(Ctx& ctx)
  {
    ctx.mb_loc = std::string("Postmaster");
    ctx.mb_dom.clear();
  }
};

template <>
struct action<helo> {
  template <typename Input>
  static void apply(Input const& in, Ctx& ctx)
  {
    auto beg = in.begin() + 5; // +5 for the length of "HELO "
    auto end = in.end() - 2;   // -2 for the CRLF
    ctx.session.helo(string_view(beg, end - beg));
    ctx.bdat_rset();
  }
};

template <>
struct action<ehlo> {
  template <typename Input>
  static void apply(Input const& in, Ctx& ctx)
  {
    auto beg = in.begin() + 5; // +5 for the length of "EHLO "
    auto end = in.end() - 2;   // -2 for the CRLF
    ctx.session.ehlo(string_view(beg, end - beg));
    ctx.bdat_rset();
  }
};

template <>
struct action<mail_from> {
  static void apply0(Ctx& ctx)
  {
    ctx.session.mail_from(::Mailbox(ctx.mb_loc, ctx.mb_dom), ctx.parameters);

    ctx.mb_loc.clear();
    ctx.mb_dom.clear();
    ctx.parameters.clear();
  }
};

template <>
struct action<rcpt_to> {
  static void apply0(Ctx& ctx)
  {
    ctx.session.rcpt_to(::Mailbox(ctx.mb_loc, ctx.mb_dom), ctx.parameters);

    ctx.mb_loc.clear();
    ctx.mb_dom.clear();
    ctx.parameters.clear();
  }
};

template <>
struct action<chunk_size> {
  template <typename Input>
  static void apply(Input const& in, Ctx& ctx)
  {
    ctx.chunk_size = std::strtoul(in.string().c_str(), nullptr, 10);
  }
};

template <>
struct action<end_marker> {
  static void apply0(Ctx& ctx) { ctx.chunk_last = true; }
};

void bdat_act(Ctx& ctx)
{
  LOG(INFO) << "BDAT " << ctx.chunk_size << (ctx.chunk_last ? " LAST" : "");

  if (ctx.chunk_first) {
    ctx.chunk_first = false;

    if (!ctx.session.bdat_start()) {
      // no need to ctx.msg.reset() when bdat_start fails
      ctx.bdat_error = true;
      LOG(ERROR) << "bdat_start() returned error!";

      // seek over BDAT data
      auto pos = ctx.session.in().tellg();
      pos += ctx.chunk_size;
      ctx.session.in().seekg(pos, ctx.session.in().beg);

      return;
    }

    ctx.new_msg();
  }

  if (ctx.bdat_error) { // If we've already failed...
    LOG(ERROR) << "BDAT continuing data error, skiping " << ctx.chunk_size
               << " octets";

    ctx.session.bdat_error(*ctx.msg);

    // seek over BDAT data
    auto pos = ctx.session.in().tellg();
    pos += ctx.chunk_size;
    ctx.session.in().seekg(pos, ctx.session.in().beg);

    return;
  }
  else if (ctx.chunk_size > Config::max_chunk_size) {
    LOG(ERROR) << "BDAT size error, skiping " << ctx.chunk_size << " octets";

    ctx.session.data_size_error(*ctx.msg);
    ctx.bdat_error = true;
    ctx.msg.reset();

    // seek over BDAT data
    auto pos = ctx.session.in().tellg();
    pos += ctx.chunk_size;
    ctx.session.in().seekg(pos, ctx.session.in().beg);

    return;
  }

  // First off, for every BDAT, we /must/ read the data, if there is any.
  std::string bfr;

  std::streamsize to_xfer = ctx.chunk_size;

  while (to_xfer) {
    auto xfer_sz = std::min(to_xfer, Config::max_xfer_size);
    bfr.resize(xfer_sz);

    ctx.session.in().read(&bfr[0], xfer_sz);
    CHECK(ctx.session.in()) << "read failed";

    if (!ctx.hdr_end) {
      auto e = bfr.find("\r\n\r\n");
      if (ctx.hdr.size() < Config::max_hdr_size) {
        ctx.hdr += bfr.substr(0, e);
        if (e == std::string::npos) {
          LOG(WARNING) << "may not have all headers in this chunk";
        }
        else {
          ctx.hdr.append("\r\n");
          ctx.hdr_end = true;
        }
      }
    }

    ctx.msg->write(&bfr[0], xfer_sz);

    to_xfer -= xfer_sz;
  }

  if (ctx.msg->size_error()) {
    LOG(ERROR) << "message size error after " << ctx.msg->count() << " octets";
    ctx.session.data_size_error(*ctx.msg);
    ctx.bdat_error = true;
    ctx.msg.reset();
    return;
  }

  if (ctx.chunk_last) {
    if (!ctx.hdr_end) {
      LOG(WARNING) << "may not have all headers in this email";
    }
    else {
      LOG(INFO) << "header size " << ctx.hdr.size() << " octets";
    }
    ctx.session.data_msg_done(*ctx.msg);
    ctx.msg.reset();
    ctx.chunk_first = true;
  }
  else {
    ctx.session.bdat_msg(*ctx.msg, ctx.chunk_size);
  }
}

template <>
struct action<bdat> {
  static void apply0(Ctx& ctx) { bdat_act(ctx); }
};

template <>
struct action<bdat_last> {
  static void apply0(Ctx& ctx)
  {
    bdat_act(ctx);
    ctx.bdat_error = true; // to make next BDAT fail.
  }
};

template <>
struct data_action<data_end> {
  static void apply0(Ctx& ctx)
  {
    if (ctx.msg->size_error()) {
      ctx.session.data_size_error(*ctx.msg);
      ctx.msg.reset();
    }
    else {
      if (!ctx.hdr_end) {
        LOG(WARNING) << "may not have all headers in this email";
      }
      else {
        LOG(INFO) << "header size " << ctx.hdr.size() << " octets";
      }
      ctx.session.data_msg_done(*ctx.msg);
      ctx.msg.reset();
    }
  }
};

template <>
struct data_action<data_blank> {
  static void apply0(Ctx& ctx)
  {
    constexpr char CRLF[]{'\r', '\n'};
    ctx.msg->write(CRLF, sizeof(CRLF));
    ctx.hdr.append(CRLF, sizeof(CRLF));
    ctx.hdr_end = true;
  }
};

template <>
struct data_action<data_plain> {
  template <typename Input>
  static void apply(Input const& in, Ctx& ctx)
  {
    size_t len = in.end() - in.begin();
    ctx.msg->write(in.begin(), len);
    if (!ctx.hdr_end) {
      if (ctx.hdr.size() < Config::max_hdr_size) {
        ctx.hdr.append(in.begin(), len);
      }
    }
  }
};

template <>
struct data_action<data_dot> {
  template <typename Input>
  static void apply(Input const& in, Ctx& ctx)
  {
    size_t len = in.end() - in.begin() - 1;
    ctx.msg->write(in.begin() + 1, len);
    if (!ctx.hdr_end) {
      LOG(WARNING) << "suspicious encoding used in header";
      if (ctx.hdr.size() < Config::max_hdr_size) {
        auto hlen = std::min(len, Config::max_hdr_size - ctx.hdr.size());
        std::copy_n(in.begin() + 1, hlen, std::back_inserter(ctx.hdr));
      }
    }
  }
};

template <>
struct data_action<anything_else> {
  template <typename Input>
  static void apply(Input const& in, Ctx& ctx)
  {
    ctx.session.bare_lf("garbage in data stream: \""s + in.string() + "\""s);
  }
};

template <>
struct action<data> {
  template <typename Input>
  static void apply(Input const& in, Ctx& ctx)
  {
    if (ctx.session.data_start()) {
      ctx.new_msg();

      istream_input<eol::crlf> data_in(ctx.session.in(), Config::bfr_size,
                                       "data");

      try {
        if (!parse_nested<RFC5321::data_grammar, RFC5321::data_action>(
                in, data_in, ctx)) {
          ctx.session.error("bad data syntax");
        }
        return;
      }
      catch (parse_error const& e) {
        ctx.session.error(e.what());
      }
      ctx.session.error("unknown data parser problem");
    }
  }
};

template <>
struct action<rset> {
  static void apply0(Ctx& ctx)
  {
    ctx.session.rset();
    ctx.bdat_rset();
  }
};

template <>
struct action<noop> {
  static void apply0(Ctx& ctx) { ctx.session.noop(); }
};

template <>
struct action<vrfy> {
  static void apply0(Ctx& ctx) { ctx.session.vrfy(); }
};

template <>
struct action<help> {
  static void apply0(Ctx& ctx) { ctx.session.help(); }
};

template <>
struct action<starttls> {
  static void apply0(Ctx& ctx) { ctx.session.starttls(); }
};

template <>
struct action<quit> {
  static void apply0(Ctx& ctx) __attribute__((noreturn)) { ctx.session.quit(); }
};
}

int main(int argc, char const* argv[])
{
  close(2); // hackage to stop glog from spewing

  std::ios::sync_with_stdio(false);
  google::InitGoogleLogging(argv[0]);

  // Don't wait for STARTTLS to fail if no cert.
  CHECK(boost::filesystem::exists(TLS::cert_path)) << "can't find cert file";

  RFC5321::Ctx ctx;

  ctx.session.greeting();

  ctx.session.in().unsetf(std::ios::skipws);

  istream_input<eol::crlf> in(ctx.session.in(), Config::bfr_size, "session");

  try {
    if (!parse<RFC5321::grammar, RFC5321::action>(in, ctx)) {
      if (ctx.session.timed_out()) {
        ctx.session.time_out();
      }
      else {
        ctx.session.error("syntax error from parser");
      }
      return 1;
    }
  }
  catch (parse_error const& e) {
    ctx.session.error(e.what());
    return 1;
  }
  ctx.session.error("unknown parser problem");
}
