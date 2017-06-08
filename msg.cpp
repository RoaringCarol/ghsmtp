#include <string>
#include <unordered_map>
#include <vector>

#include <glog/logging.h>

#include <boost/algorithm/string.hpp>

#define BOOST_FILESYSTEM_NO_DEPRECATED
#include <boost/filesystem.hpp>

#include <boost/iostreams/device/mapped_file.hpp>

#include <iostream>

#include "DKIM.hpp"
#include "DMARC.hpp"
#include "Mailbox.hpp"

#include <tao/pegtl.hpp>
#include <tao/pegtl/contrib/abnf.hpp>
#include <tao/pegtl/contrib/alphabet.hpp>

#include <tao/pegtl/contrib/tracer.hpp>

using namespace tao::pegtl;
using namespace tao::pegtl::abnf;
using namespace tao::pegtl::alphabet;

using std::experimental::string_view;

using namespace std::string_literals;

namespace RFC5322 {

// clang-format off
constexpr char const* defined_fields[]{

    // Trace Fields
    "Return-Path",
    "Received",
    "Received-SPF",   // RFC 7208 added trace field

    // Sig
    "DKIM-Signature", // RFC 7489

    // Originator Fields
    "Date",
    "From",
    "Sender",
    "Reply-To",

    // Destination Address Fields
    "To",
    "Cc",
    "Bcc",

    // Identification Fields
    "Message-ID",
    "In-Reply-To",
    "References",

    // Informational Fields
    "Subject",
    "Comments",
    "Keywords",

    // Resent Fields
    "Resent-Date",
    "Resent-From",
    "Resent-Sender",
    "Resent-To",
    "Resent-Cc",
    "Resent-Bcc",
    "Resent-Message-ID",

};
// clang-format on

bool is_defined_field(string_view value)
{
  auto first = std::begin(defined_fields);
  auto last = std::end(defined_fields);
  for (; first != last; ++first) {
    if (0 == strcasecmp(*first, value.data())) { // US-ASCII
      return true;
    }
  }
  return false;
}

struct ci_less : public std::binary_function<std::string, std::string, bool> {
  bool operator()(std::string const& lhs, std::string const& rhs) const
  {
    return strcasecmp(lhs.c_str(), rhs.c_str()) < 0;
  }
};

struct Ctx {
  OpenDKIM::Lib dkl;
  OpenDKIM::Verify dkv{dkl};

  OpenDMARC::Lib dml;
  OpenDMARC::Policy dmp;

  std::string mb_loc;
  std::string mb_dom;

  std::vector<::Mailbox> mb_list; // temporary accumulator

  std::vector<::Mailbox> from_list;

  ::Mailbox sender;

  std::string key;
  std::string value;

  std::vector<std::pair<std::string, std::string>> kv_list;

  std::map<std::string, std::string, ci_less> spf_info;
  std::string spf_result;

  std::string unstructured;

  std::string opt_name;
  std::string opt_value;

  std::vector<std::string> msg_errors;
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

struct VUCHAR : sor<VCHAR, UTF8_non_ascii> {
};

struct text : sor<ranges<1, 9, 11, 12, 14, 127>, UTF8_non_ascii> {
};

// struct obs_body {};

struct body : seq<star<seq<rep_max<998, text>, eol>>, rep_max<998, text>> {
};

struct FWS : seq<opt<seq<star<WSP>, eol>>, plus<WSP>> {
};

struct qtext : sor<one<33>, ranges<35, 91, 93, 126>, UTF8_non_ascii> {
};

struct quoted_pair : seq<one<'\\'>, sor<VUCHAR, WSP>> {
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

struct ctext : ranges<33, 39, 42, 91, 93, 126> {
};

struct comment;

struct ccontent : sor<ctext, quoted_pair, comment> {
};

struct comment
    : seq<one<'('>, star<seq<opt<FWS>, ccontent>>, opt<FWS>, one<')'>> {
};

struct CFWS : sor<seq<plus<seq<opt<FWS>, comment>, opt<FWS>>>, FWS> {
};

struct qcontent : sor<qtext, quoted_pair> {
};

struct quoted_string : seq<opt<CFWS>,
                           DQUOTE,
                           star<seq<opt<FWS>, qcontent>>,
                           opt<FWS>,
                           DQUOTE,
                           opt<CFWS>> {
};

struct unstructured : seq<star<seq<opt<FWS>, VUCHAR>>, star<WSP>> {
};

struct atom : seq<opt<CFWS>, plus<atext>, opt<CFWS>> {
};

struct dot_atom_text : list<plus<atext>, one<'.'>> {
};

struct dot_atom : seq<opt<CFWS>, dot_atom_text, opt<CFWS>> {
};

struct word : sor<atom, quoted_string> {
};

struct phrase : plus<word> {
};

struct local_part : sor<dot_atom, quoted_string> {
};

struct dtext : ranges<33, 90, 94, 126> {
};

struct domain_literal : seq<opt<CFWS>,
                            one<'['>,
                            star<seq<opt<FWS>, dtext>>,
                            opt<FWS>,
                            one<']'>,
                            opt<CFWS>> {
};

struct domain : sor<dot_atom, domain_literal> {
};

struct addr_spec : seq<local_part, one<'@'>, domain> {
};

struct angle_addr : seq<opt<CFWS>, one<'<'>, addr_spec, one<'>'>, opt<CFWS>> {
};

struct path : sor<angle_addr,
                  seq<opt<CFWS>, one<'<'>, opt<CFWS>, one<'>'>, opt<CFWS>>> {
};

struct display_name : phrase {
};

struct name_addr : seq<opt<display_name>, angle_addr> {
};

struct mailbox : sor<name_addr, addr_spec> {
};

struct group_list;

struct group
    : seq<display_name, one<':'>, opt<group_list>, one<';'>, opt<CFWS>> {
};

struct address : sor<mailbox, group> {
};

struct mailbox_list : list<mailbox, one<','>> {
};

struct address_list : list<address, one<','>> {
};

struct group_list : sor<mailbox_list, CFWS> {
};

struct day : seq<opt<FWS>, plus<rep_min_max<1, 2, DIGIT>>, FWS> {
};

struct month : sor<TAOCPP_PEGTL_ISTRING("Jan"),
                   TAOCPP_PEGTL_ISTRING("Feb"),
                   TAOCPP_PEGTL_ISTRING("Mar"),
                   TAOCPP_PEGTL_ISTRING("Apr"),
                   TAOCPP_PEGTL_ISTRING("May"),
                   TAOCPP_PEGTL_ISTRING("Jun"),
                   TAOCPP_PEGTL_ISTRING("Jul"),
                   TAOCPP_PEGTL_ISTRING("Aug"),
                   TAOCPP_PEGTL_ISTRING("Sep"),
                   TAOCPP_PEGTL_ISTRING("Oct"),
                   TAOCPP_PEGTL_ISTRING("Nov"),
                   TAOCPP_PEGTL_ISTRING("Dec")> {
};

struct year : seq<FWS, rep<4, DIGIT>, FWS> {
};

struct date : seq<day, month, year> {
};

struct day_name : sor<TAOCPP_PEGTL_ISTRING("Mon"),
                      TAOCPP_PEGTL_ISTRING("Tue"),
                      TAOCPP_PEGTL_ISTRING("Wed"),
                      TAOCPP_PEGTL_ISTRING("Thu"),
                      TAOCPP_PEGTL_ISTRING("Fri"),
                      TAOCPP_PEGTL_ISTRING("Sat"),
                      TAOCPP_PEGTL_ISTRING("Sun")> {
};

struct day_of_week : seq<opt<FWS>, day_name> {
};

struct hour : rep<2, DIGIT> {
};

struct minute : rep<2, DIGIT> {
};

struct second : rep<2, DIGIT> {
};

struct time_of_day : seq<hour, one<':'>, minute, opt<seq<one<':'>, second>>> {
};

struct zone : seq<FWS, sor<one<'+'>, one<'-'>>, rep<4, DIGIT>> {
};

struct time : seq<time_of_day, zone> {
};

struct date_time : seq<opt<seq<day_of_week, one<','>>>, date, time, opt<CFWS>> {
};

// The Origination Date Field
struct orig_date : seq<TAOCPP_PEGTL_ISTRING("Date:"), date_time, eol> {
};

// Originator Fields
struct from : seq<TAOCPP_PEGTL_ISTRING("From:"), mailbox_list, eol> {
};

struct sender : seq<TAOCPP_PEGTL_ISTRING("Sender:"), mailbox, eol> {
};

struct reply_to : seq<TAOCPP_PEGTL_ISTRING("Reply-To:"), address_list, eol> {
};

// Destination Address Fields
struct to : seq<TAOCPP_PEGTL_ISTRING("To:"), address_list, eol> {
};

struct cc : seq<TAOCPP_PEGTL_ISTRING("Cc:"), address_list, eol> {
};

struct bcc
    : seq<TAOCPP_PEGTL_ISTRING("Bcc:"), opt<sor<address_list, CFWS>>, eol> {
};

// Identification Fields

struct no_fold_literal : seq<one<'['>, star<dtext>, one<']'>> {
};

struct id_left : dot_atom_text {
};

struct id_right : sor<dot_atom_text, no_fold_literal> {
};

struct msg_id : seq<opt<CFWS>,
                    one<'<'>,
                    id_left,
                    one<'@'>,
                    id_right,
                    one<'>'>,
                    opt<CFWS>> {
};

struct message_id : seq<TAOCPP_PEGTL_ISTRING("Message-ID:"), msg_id, eol> {
};

struct in_reply_to
    : seq<TAOCPP_PEGTL_ISTRING("In-Reply-To:"), plus<msg_id>, eol> {
};

struct references
    : seq<TAOCPP_PEGTL_ISTRING("References:"), plus<msg_id>, eol> {
};

// Informational Fields

struct subject : seq<TAOCPP_PEGTL_ISTRING("Subject:"), unstructured, eol> {
};

struct comments : seq<TAOCPP_PEGTL_ISTRING("Comments:"), unstructured, eol> {
};

struct keywords
    : seq<TAOCPP_PEGTL_ISTRING("Keywords:"), list<phrase, one<','>>, eol> {
};

// Resent Fields

struct resent_date : seq<TAOCPP_PEGTL_ISTRING("Resent-Date:"), date_time, eol> {
};

struct resent_from
    : seq<TAOCPP_PEGTL_ISTRING("Resent-From:"), mailbox_list, eol> {
};

struct resent_sender
    : seq<TAOCPP_PEGTL_ISTRING("Resent-Sender:"), mailbox, eol> {
};

struct resent_to : seq<TAOCPP_PEGTL_ISTRING("Resent-To:"), address_list, eol> {
};

struct resent_cc : seq<TAOCPP_PEGTL_ISTRING("Resent-Cc:"), address_list, eol> {
};

struct resent_bcc : seq<TAOCPP_PEGTL_ISTRING("Resent-Bcc:"),
                        opt<sor<address_list, CFWS>>,
                        eol> {
};

struct resent_msg_id
    : seq<TAOCPP_PEGTL_ISTRING("Resent-Message-ID:"), msg_id, eol> {
};

// Trace Fields

struct return_path : seq<TAOCPP_PEGTL_ISTRING("Return-Path:"), path, eol> {
};

// qmail style received line:
struct qmail_terd : seq<FWS, comment> {
};

struct received_token : sor<angle_addr, addr_spec, domain, word, qmail_terd> {
};

struct received : seq<TAOCPP_PEGTL_ISTRING("Received:"),
                      star<received_token>,
                      one<';'>,
                      date_time,
                      eol> {
};

struct result : sor<TAOCPP_PEGTL_ISTRING("Pass"),
                    TAOCPP_PEGTL_ISTRING("Fail"),
                    TAOCPP_PEGTL_ISTRING("SoftFail"),
                    TAOCPP_PEGTL_ISTRING("Neutral"),
                    TAOCPP_PEGTL_ISTRING("None"),
                    TAOCPP_PEGTL_ISTRING("TempError"),
                    TAOCPP_PEGTL_ISTRING("PermError")> {
};

struct key : sor<TAOCPP_PEGTL_ISTRING("client-ip"),
                 TAOCPP_PEGTL_ISTRING("envelope-from"),
                 TAOCPP_PEGTL_ISTRING("helo")> {
};

// This value syntax (allowing mailbox) is not in accordance with RFC
// 7208 (or 4408) but is what is effectivly used by libspf2 1.2.10 and
// before.

struct value : sor<mailbox, dot_atom, quoted_string> {
};

struct key_value_pair : seq<key, opt<CFWS>, one<'='>, value> {
};

struct key_value_list : seq<key_value_pair,
                            star<seq<one<';'>, opt<CFWS>, key_value_pair>>,
                            opt<one<';'>>> {
};

struct received_spf : seq<TAOCPP_PEGTL_ISTRING("Received-SPF:"),
                          opt<CFWS>,
                          result,
                          FWS,
                          opt<seq<comment, FWS>>,
                          opt<key_value_list>,
                          eol> {
};

struct dkim_signature
    : seq<TAOCPP_PEGTL_ISTRING("DKIM-Signature:"), unstructured, eol> {
};

// Optional Fields

struct ftext : ranges<33, 57, 59, 126> {
};

struct field_name : plus<ftext> {
};

struct field_value : unstructured {
};

struct optional_field : seq<field_name, one<':'>, field_value, eol> {
};

// message header

// clang-format off
struct fields : star<sor<
                         return_path,
                         received,
                         received_spf,

                         dkim_signature,

                         orig_date,
                         from,
                         sender,
                         reply_to,

                         to,
                         cc,
                         bcc,

                         message_id,
                         in_reply_to,
                         references,

                         subject,
                         comments,
                         keywords,

                         resent_date,
                         resent_from,
                         resent_sender,
                         resent_to,
                         resent_cc,
                         resent_bcc,
                         resent_msg_id,

                         optional_field
                       >> {
};
// clang-format on

struct message : seq<fields, opt<seq<eol, body>>> {
};

template <typename Rule>
struct action : nothing<Rule> {
};

template <>
struct action<fields> {
  template <typename Input>
  static void apply(Input const& in, Ctx& ctx)
  {
    // LOG(INFO) << "fields";
  }
};

template <>
struct action<unstructured> {
  template <typename Input>
  static void apply(Input const& in, Ctx& ctx)
  {
    ctx.unstructured = in.string();
  }
};

template <>
struct action<field_name> {
  template <typename Input>
  static void apply(Input const& in, Ctx& ctx)
  {
    ctx.opt_name = in.string();
  }
};

template <>
struct action<field_value> {
  template <typename Input>
  static void apply(Input const& in, Ctx& ctx)
  {
    ctx.opt_value = in.string();
  }
};

template <>
struct action<optional_field> {
  template <typename Input>
  static void apply(Input const& in, Ctx& ctx)
  {
    // LOG(INFO) << "optional_field";
    if (is_defined_field(ctx.opt_name)) {
      // So, this is a syntax error in a defined field.
      auto err = "syntax error in: \""s + in.string() + "\""s;
      ctx.msg_errors.push_back(err);
      LOG(ERROR) << err;
    }
    ctx.dkv.header(string_view(in.begin(), in.end() - in.begin()));
    ctx.unstructured.clear();
  }
};

template <>
struct action<local_part> {
  template <typename Input>
  static void apply(Input const& in, Ctx& ctx)
  {
    ctx.mb_loc = in.string();
    boost::trim(ctx.mb_loc);
  }
};

template <>
struct action<domain> {
  template <typename Input>
  static void apply(Input const& in, Ctx& ctx)
  {
    ctx.mb_dom = in.string();
  }
};

template <>
struct action<mailbox> {
  static void apply0(Ctx& ctx)
  {
    // LOG(INFO) << "mailbox emplace_back(" << ctx.mb_loc << '@' << ctx.mb_dom
    // << ')';
    ctx.mb_list.emplace_back(ctx.mb_loc, ctx.mb_dom);
  }
};

template <>
struct action<orig_date> {
  template <typename Input>
  static void apply(const Input& in, Ctx& ctx)
  {
    // LOG(INFO) << "Date:";
    ctx.dkv.header(string_view(in.begin(), in.end() - in.begin()));
  }
};

// Originator Fields

template <>
struct action<from> {
  template <typename Input>
  static void apply(const Input& in, Ctx& ctx)
  {
    if (!ctx.from_list.empty()) {
      auto msg = "multiple 'From:' address headers, previous list: \n"s;
      for (auto const& add : ctx.from_list) {
        msg += " "s + static_cast<std::string>(add) + "\n"s;
      }
      msg += "new: " + in.string();
      ctx.msg_errors.push_back(msg);
    }

    ctx.dkv.header(string_view(in.begin(), in.end() - in.begin()));
    ctx.from_list = std::move(ctx.mb_list);
    ctx.mb_list.clear();
  }
};

template <>
struct action<sender> {
  template <typename Input>
  static void apply(const Input& in, Ctx& ctx)
  {
    if (!ctx.sender.empty()) {
      auto err = "multiple 'Sender:' headers, previous: "s
                 + static_cast<std::string>(ctx.sender);
      ctx.msg_errors.push_back(err);
      err += ", this: "s + in.string();
    }
    ctx.dkv.header(string_view(in.begin(), in.end() - in.begin()));
    CHECK_EQ(ctx.mb_list.size(), 1);
    ctx.sender = std::move(ctx.mb_list[0]);
    ctx.mb_list.clear();
  }
};

template <>
struct action<reply_to> {
  template <typename Input>
  static void apply(const Input& in, Ctx& ctx)
  {
    ctx.dkv.header(string_view(in.begin(), in.end() - in.begin()));
    ctx.mb_list.clear();
  }
};

// Destination Address Fields

template <>
struct action<to> {
  template <typename Input>
  static void apply(const Input& in, Ctx& ctx)
  {
    ctx.dkv.header(string_view(in.begin(), in.end() - in.begin()));
    ctx.mb_list.clear();
  }
};

template <>
struct action<cc> {
  template <typename Input>
  static void apply(const Input& in, Ctx& ctx)
  {
    ctx.dkv.header(string_view(in.begin(), in.end() - in.begin()));
    ctx.mb_list.clear();
  }
};

template <>
struct action<bcc> {
  template <typename Input>
  static void apply(const Input& in, Ctx& ctx)
  {
    ctx.dkv.header(string_view(in.begin(), in.end() - in.begin()));
    ctx.mb_list.clear();
  }
};

// Identification Fields

template <>
struct action<message_id> {
  template <typename Input>
  static void apply(const Input& in, Ctx& ctx)
  {
    ctx.dkv.header(string_view(in.begin(), in.end() - in.begin()));
  }
};

template <>
struct action<in_reply_to> {
  template <typename Input>
  static void apply(const Input& in, Ctx& ctx)
  {
    ctx.dkv.header(string_view(in.begin(), in.end() - in.begin()));
  }
};

template <>
struct action<references> {
  template <typename Input>
  static void apply(const Input& in, Ctx& ctx)
  {
    ctx.dkv.header(string_view(in.begin(), in.end() - in.begin()));
  }
};

// Informational Fields

template <>
struct action<subject> {
  template <typename Input>
  static void apply(const Input& in, Ctx& ctx)
  {
    ctx.dkv.header(string_view(in.begin(), in.end() - in.begin()));
    ctx.unstructured.clear();
  }
};

template <>
struct action<comments> {
  template <typename Input>
  static void apply(const Input& in, Ctx& ctx)
  {
    ctx.dkv.header(string_view(in.begin(), in.end() - in.begin()));
    ctx.unstructured.clear();
  }
};

template <>
struct action<keywords> {
  template <typename Input>
  static void apply(const Input& in, Ctx& ctx)
  {
    ctx.dkv.header(string_view(in.begin(), in.end() - in.begin()));
  }
};

// Resent Fields

template <>
struct action<resent_date> {
  template <typename Input>
  static void apply(const Input& in, Ctx& ctx)
  {
    ctx.dkv.header(string_view(in.begin(), in.end() - in.begin()));
  }
};

template <>
struct action<resent_from> {
  template <typename Input>
  static void apply(const Input& in, Ctx& ctx)
  {
    ctx.dkv.header(string_view(in.begin(), in.end() - in.begin()));
    ctx.mb_list.clear();
  }
};

template <>
struct action<resent_sender> {
  template <typename Input>
  static void apply(const Input& in, Ctx& ctx)
  {
    ctx.dkv.header(string_view(in.begin(), in.end() - in.begin()));
    ctx.mb_list.clear();
  }
};

template <>
struct action<resent_to> {
  template <typename Input>
  static void apply(const Input& in, Ctx& ctx)
  {
    ctx.dkv.header(string_view(in.begin(), in.end() - in.begin()));
    ctx.mb_list.clear();
  }
};

template <>
struct action<resent_cc> {
  template <typename Input>
  static void apply(const Input& in, Ctx& ctx)
  {
    ctx.dkv.header(string_view(in.begin(), in.end() - in.begin()));
    ctx.mb_list.clear();
  }
};

template <>
struct action<resent_bcc> {
  template <typename Input>
  static void apply(const Input& in, Ctx& ctx)
  {
    ctx.dkv.header(string_view(in.begin(), in.end() - in.begin()));
    ctx.mb_list.clear();
  }
};

template <>
struct action<resent_msg_id> {
  template <typename Input>
  static void apply(const Input& in, Ctx& ctx)
  {
    ctx.dkv.header(string_view(in.begin(), in.end() - in.begin()));
  }
};

// Trace Fields

template <>
struct action<return_path> {
  template <typename Input>
  static void apply(const Input& in, Ctx& ctx)
  {
    // LOG(INFO) << "Return-Path:";
    ctx.dkv.header(string_view(in.begin(), in.end() - in.begin()));
    ctx.mb_list.clear();
  }
};

template <>
struct action<received> {
  template <typename Input>
  static void apply(const Input& in, Ctx& ctx)
  {
    // LOG(INFO) << "Received:";
    ctx.dkv.header(string_view(in.begin(), in.end() - in.begin()));
    ctx.mb_list.clear();
  }
};

template <>
struct action<result> {
  template <typename Input>
  static void apply(const Input& in, Ctx& ctx)
  {
    ctx.spf_result = std::move(in.string());
    boost::to_lower(ctx.spf_result);
  }
};

template <>
struct action<key> {
  template <typename Input>
  static void apply(const Input& in, Ctx& ctx)
  {
    ctx.key = std::move(in.string());
  }
};

template <>
struct action<value> {
  template <typename Input>
  static void apply(const Input& in, Ctx& ctx)
  {
    ctx.value = std::move(in.string());
    boost::trim(ctx.value);
  }
};

template <>
struct action<key_value_pair> {
  template <typename Input>
  static void apply(const Input& in, Ctx& ctx)
  {
    ctx.kv_list.emplace_back(ctx.key, ctx.value);
  }
};

template <>
struct action<key_value_list> {
  static void apply0(Ctx& ctx)
  {
    for (auto kvp : ctx.kv_list) {
      ctx.spf_info[kvp.first] = kvp.second;
    }
  }
};

template <>
struct action<received_spf> {
  template <typename Input>
  static void apply(const Input& in, Ctx& ctx)
  {
    // LOG(INFO) << "Received-SPF:";

    // *** Do the check now?

    // utsname un;
    // PCHECK(uname(&un) == 0);
    // SPF::Server spf_srv(un.nodename);
    // SPF::Request spf_req(spf_srv);

    // spf_req.set_ipv4_str(ctx.spf_info["client-ip"].c_str());
    // spf_req.set_helo_dom(ctx.spf_info["helo"].c_str());
    // spf_req.set_env_from(ctx.spf_info["envelope-from"].c_str());

    // SPF::Response spf_res(spf_req);
    // auto res = spf_res.result();
    // CHECK_NE(res, SPF::Result::INVALID);

    // *** Get result from header:

    int pol_spf = DMARC_POLICY_SPF_OUTCOME_PASS;

    // Pass is the default:
    // if (ctx.spf_result == "pass") {
    //   pol_spf = DMARC_POLICY_SPF_OUTCOME_PASS;
    // }

    // if ((ctx.spf_result == "neutral") || (ctx.spf_result == "softfail")) {
    //   // could also be a FAIL maybe...
    //   pol_spf = DMARC_POLICY_SPF_OUTCOME_PASS;
    // }

    if (ctx.spf_result == "none") {
      pol_spf = DMARC_POLICY_SPF_OUTCOME_NONE;
    }

    if (ctx.spf_result == "temperror") {
      pol_spf = DMARC_POLICY_SPF_OUTCOME_TMPFAIL;
    }

    if ((ctx.spf_result == "fail") || (ctx.spf_result == "permerror")) {
      pol_spf = DMARC_POLICY_SPF_OUTCOME_FAIL;
    }

    auto dom = ctx.spf_info["envelope-from"];
    auto origin = DMARC_POLICY_SPF_ORIGIN_MAILFROM;

    if (dom == "<>") {
      dom = ctx.spf_info["helo"];
      origin = DMARC_POLICY_SPF_ORIGIN_HELO;
      LOG(INFO) << "SPF: HELO " << dom;
    }
    else {
      auto pos = dom.find_first_of('@');
      if (pos != std::string::npos) {
        dom = dom.substr(pos + 1);
      }
      LOG(INFO) << "SPF: MAIL FROM " << dom;
    }

    ctx.dmp.init(ctx.spf_info["client-ip"].c_str());
    ctx.dmp.store_spf(dom.c_str(), pol_spf, origin, nullptr);

    LOG(INFO) << "SPF: ip==" << ctx.spf_info["client-ip"] << ", "
              << ctx.spf_result;

    ctx.mb_list.clear();
  }
};

template <>
struct action<dkim_signature> {
  template <typename Input>
  static void apply(const Input& in, Ctx& ctx)
  {
    ctx.dkv.header(string_view(in.begin(), in.end() - in.begin()));
    // LOG(INFO) << "dkim_signature check";
    CHECK(ctx.dkv.check_signature(ctx.unstructured)) << ctx.unstructured;
    ctx.unstructured.clear();
  }
};

template <>
struct action<received_token> {
  template <typename Input>
  static void apply(const Input& in, Ctx& ctx)
  {
  }
};

template <>
struct action<body> {
  template <typename Input>
  static void apply(const Input& in, Ctx& ctx)
  {
    // LOG(INFO) << "body";
    ctx.dkv.eoh();
    ctx.dkv.body(string_view(in.begin(), in.end() - in.begin()));
  }
};

template <>
struct action<message> {
  template <typename Input>
  static void apply(const Input& in, Ctx& ctx)
  {
    // LOG(INFO) << "message";
    ctx.dkv.eom();

    // ctx.dkv.check();

    std::string from_domain;

    if (ctx.from_list.empty()) {
      // RFC-5322 says message must have a 'From:' header.
      LOG(ERROR) << "No 'From:' header";
      return;
    }

    if (ctx.from_list.size() > 1) {

      LOG(INFO) << ctx.from_list.size() << " RFC5322.From addresses";
      for (auto& f : ctx.from_list) {
        LOG(INFO) << f;
      }

      if (ctx.sender.empty()) {
        // Must have 'Sender:' says RFC-5322 section 3.6.2.
        LOG(ERROR) << "No 'Sender:' header with multiple From: mailboxes";
        return;
      }

      // find sender in from list
      auto s = find(begin(ctx.from_list), end(ctx.from_list), ctx.sender);
      if (s == end(ctx.from_list)) {
        // can't be found, not an error
        LOG(INFO) << "No 'From:' match to 'Sender:'";
      }

      from_domain = ctx.sender;
      LOG(INFO) << "using 'Sender:' domain " << ctx.sender.domain();
    }
    else {

      from_domain = ctx.from_list[0].domain();

      if (!ctx.sender.empty()) {
        if (from_domain != ctx.sender.domain()) {
          LOG(INFO) << "using 'Sender:' domain " << ctx.sender.domain()
                    << " in place of 'From:' domain " << from_domain;
          from_domain = ctx.sender.domain();
        }
      }
    }

    // if the domain is UTF-8, must convert to A-label here

    ctx.dmp.store_from_domain(from_domain.c_str());

    ctx.dkv.foreach_sig([&ctx](char const* domain, bool passed) {
      LOG(INFO) << "DKIM check for " << domain
                << (passed ? " passed" : " failed");

      int result = passed ? DMARC_POLICY_DKIM_OUTCOME_PASS
                          : DMARC_POLICY_DKIM_OUTCOME_FAIL;
      ctx.dmp.store_dkim(domain, result, nullptr);
    });

    ctx.dmp.query_dmarc(from_domain.c_str());

    LOG(INFO) << "Final DMARC advice for domain " << from_domain << ": "
              << Advice_to_string(ctx.dmp.get_policy());

    if (ctx.msg_errors.size()) {
      for (auto e : ctx.msg_errors) {
        LOG(ERROR) << e;
      }
    }
  }
};
}

int main(int argc, char const* argv[])
{
  CHECK(RFC5322::is_defined_field("Subject"));
  CHECK(!RFC5322::is_defined_field("X-Subject"));

  for (auto i = 1; i < argc; ++i) {
    auto fn = argv[i];
    boost::filesystem::path name(fn);
    boost::iostreams::mapped_file_source f(name);
    memory_input<> in(f.data(), f.size(), fn);
    try {
      RFC5322::Ctx ctx;
      if (!parse<RFC5322::message, RFC5322::action>(in, ctx)) {
        LOG(ERROR) << "parse returned false";
      }
    }
    catch (parse_error const& e) {
      std::cerr << e.what();
      return 1;
    }
  }
}
