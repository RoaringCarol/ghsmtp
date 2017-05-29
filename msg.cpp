#include <glog/logging.h>

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

namespace msg {

struct Ctx {
  OpenDKIM::Lib dkl;
  OpenDKIM::Verify dkv{dkl};

  OpenDMARC::Lib dmarc;

  std::string mb_loc;
  std::string mb_dom;

  std::vector<::Mailbox> mb_list;

  std::vector<::Mailbox> RFC5322_From;
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

struct received_token : sor<angle_addr, addr_spec, domain, word> {
};

struct received : seq<TAOCPP_PEGTL_ISTRING("Received:"),
                      star<received_token>,
                      one<';'>,
                      date_time,
                      eol> {
};

struct old_x_original_to
    : seq<TAOCPP_PEGTL_ISTRING("X-Original-To:"), unstructured, eol> {
};

struct delivered_to
    : seq<TAOCPP_PEGTL_ISTRING("Delivered-To:"), addr_spec, eol> {
};

struct x_original_to
    : seq<TAOCPP_PEGTL_ISTRING("X-Original-To:"), address_list, eol> {
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

struct fields : star<sor<return_path,
                         received,
                         resent_date,
                         resent_from,
                         resent_sender,
                         resent_to,
                         resent_cc,
                         resent_bcc,
                         resent_msg_id,
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
                         optional_field>> {
};

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
    LOG(INFO) << "fields";
  }
};

template <>
struct action<optional_field> {
  template <typename Input>
  static void apply(Input const& in, Ctx& ctx)
  {
    LOG(INFO) << in.string();
    ctx.dkv.header(string_view(in.begin(), in.end() - in.begin()));
    ctx.mb_list.clear();
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
    ctx.mb_list.emplace_back(ctx.mb_loc, ctx.mb_dom);
  }
};

template <>
struct action<orig_date> {
  template <typename Input>
  static void apply(const Input& in, Ctx& ctx)
  {
    LOG(INFO) << "Date:";
    ctx.dkv.header(string_view(in.begin(), in.end() - in.begin()));
  }
};

// Originator Fields

template <>
struct action<from> {
  template <typename Input>
  static void apply(const Input& in, Ctx& ctx)
  {
    LOG(INFO) << "From:";
    ctx.dkv.header(string_view(in.begin(), in.end() - in.begin()));
    ctx.RFC5322_From = std::move(ctx.mb_list);
    ctx.mb_list.clear();
    LOG(INFO) << "list length == " << ctx.RFC5322_From.size();
  }
};

template <>
struct action<sender> {
  template <typename Input>
  static void apply(const Input& in, Ctx& ctx)
  {
    ctx.dkv.header(string_view(in.begin(), in.end() - in.begin()));
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
  }
};
template <>
struct action<comments> {
  template <typename Input>
  static void apply(const Input& in, Ctx& ctx)
  {
    ctx.dkv.header(string_view(in.begin(), in.end() - in.begin()));
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
    LOG(INFO) << "Return-Path:";
    // ctx.dkv.header(string_view(in.begin(), in.end() - in.begin()));
    ctx.mb_list.clear();
  }
};

template <>
struct action<received> {
  template <typename Input>
  static void apply(const Input& in, Ctx& ctx)
  {
    LOG(INFO) << "Received:";
    // ctx.dkv.header(string_view(in.begin(), in.end() - in.begin()));
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
    LOG(INFO) << "body";
    ctx.dkv.eoh();
    ctx.dkv.body(string_view(in.begin(), in.end() - in.begin()));
    ctx.dkv.eom();
    ctx.dkv.check();
  }
};
};

int main(int argc, char const* argv[])
{
  for (auto i = 1; i < argc; ++i) {
    auto fn = argv[i];
    boost::filesystem::path name(fn);
    boost::iostreams::mapped_file_source f(name);
    memory_input<> in(f.data(), f.size(), fn);
    try {
      msg::Ctx ctx;
      if (parse<msg::message, msg::action>(in, ctx)) {
        LOG(INFO) << "parse returned true";
      }
      else {
        LOG(INFO) << "parse returned false";
      }
    }
    catch (parse_error const& e) {
      std::cerr << e.what();
      return 1;
    }
  }
}
