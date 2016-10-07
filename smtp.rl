#include "Session.hpp"

using std::experimental::string_view;
using std::string;

constexpr size_t BUFSIZE = 128;

%%{
machine smtp;

action mb_loc_beg {
  mb_loc_beg = fpc;
}

action mb_dom_beg {
  mb_dom_beg = fpc;
}

action mb_loc_end {
  CHECK_NOTNULL(mb_loc_beg);

  mb_loc_end = fpc;
  mb_loc = string(mb_loc_beg, mb_loc_end - mb_loc_beg);

  mb_loc_beg = nullptr;
  mb_loc_end = nullptr;
}

action mb_dom_end {
  CHECK_NOTNULL(mb_dom_beg);

  mb_dom_end = fpc;
  mb_dom = string(mb_dom_beg, mb_dom_end - mb_dom_beg);

  mb_dom_beg = nullptr;
  mb_dom_end = nullptr;
}

action key_end {
  param.first += ::toupper(fc);
}

action val_end {
  param.second += fc;
}

action param {
  parameters.insert(param);
  param.first.clear();
  param.second.clear();
}

action last {
  last = true;
}

UTF8_tail = 0x80..0xBF;

UTF8_1 = 0x00..0x7F;

UTF8_2 = 0xC2..0xDF UTF8_tail;

UTF8_3 = (0xE0 0xA0..0xBF UTF8_tail)
       | (0xE1..0xEC UTF8_tail{2})
       | (0xED 0x80..0x9F UTF8_tail)
       | (0xEE..0xEF UTF8_tail{2})
       ;

UTF8_4 = (0xF0 0x90..0xBF UTF8_tail{2})
       | (0xF1..0xF3 UTF8_tail{3})
       | (0xF4 0x80..0x8F UTF8_tail{2})
       ;

UTF8_char = UTF8_1 | UTF8_2 | UTF8_3 | UTF8_4;

UTF8_non_ascii = UTF8_2 | UTF8_3 | UTF8_4;

CRLF = "\r\n";
# CRLF = '\n' | '\r\n';

SP = ' ';

Let_dig = alpha | digit;

Ldh_str = (alpha | digit | '-')* Let_dig;

sub_domain = Let_dig Ldh_str?;

Domain = sub_domain ('.' sub_domain)*;

snum = ('2' '5' '0'..'5')
     | ('2' '0'..'4' digit)
     | ('0'..'1' digit{1,2})
     | digit{1,2}
     ;

IPv4_address_literal = snum ('.' snum){3};

IPv6_hex = xdigit{1,4};

IPv6_full = IPv6_hex (':' IPv6_hex){7};

IPv6_comp = (IPv6_hex (':' IPv6_hex){0,5})? '::' (IPv6_hex (':' IPv6_hex){0,5})?;

IPv6v4_full = IPv6_hex (':' IPv6_hex){5} ':' IPv4_address_literal;

IPv6v4_comp = (IPv6_hex (':' IPv6_hex){0,3})? '::' (IPv6_hex (':' IPv6_hex){0,3} ':')? IPv4_address_literal;

IPv6_addr = IPv6_full | IPv6_comp | IPv6v4_full | IPv6v4_comp;

IPv6_address_literal = 'IPv6:' IPv6_addr;

dcontent = graph - '\[' - '\\' - '\]';   # 33..90 | 94..126

standardized_tag = Ldh_str;

General_address_literal = standardized_tag ':' dcontent{1};

# See rfc 5321 Section 4.1.3
address_literal = '[' (IPv4_address_literal |
                  IPv6_address_literal | General_address_literal) ']';

At_domain = '@' Domain;

A_d_l = At_domain (',' At_domain)*;

qtextSMTP = print - '"' - '\\';   # 32..33 | 35..91 | 93..126

quoted_pairSMTP = '\\' print;

QcontentSMTP = qtextSMTP | quoted_pairSMTP;

Quoted_string = '"' QcontentSMTP* '"';

atext = alpha | digit |
        '!' | '#' |
        '$' | '%' |
        '&' | "'" |
        '*' | '+' |
        '-' | '/' |
        '=' | '?' |
        '^' | '_' |
        '`' | '{' |
        '|' | '}' |
        '~' |
        UTF8_non_ascii;

WSP = ' ' | '\t';

obs_FWS = WSP+ (CRLF WSP+)*;

FWS = ((WSP* CRLF)? WSP+) | obs_FWS;

# ccontent = ctext | quoted_pair | comment;
# comment = '(' (fws? ccontent)* fws? ')';
# cfws = ((fws? comment)+ fws?) | fws;

# Atom = cfws atext+ cfws;

Atom = atext+;

Dot_string = Atom ('.'  Atom)*;

Local_part = Dot_string | Quoted_string;

Mailbox = Local_part >mb_loc_beg %mb_loc_end '@' ((Domain | address_literal) >mb_dom_beg %mb_dom_end);

Path = "<" ((A_d_l ":")? Mailbox) ">";

Reverse_path = Path | "<>";

Forward_path = Path;

esmtp_keyword = ((alpha | digit) (alpha | digit | '-')*) @key_end;

esmtp_value = ((graph - '=')+) @val_end;

esmtp_param = (esmtp_keyword ('=' esmtp_value)?) %param;

Mail_parameters = esmtp_param (' ' esmtp_param)*;

Rcpt_parameters = esmtp_param (' ' esmtp_param)*;

String = Atom | Quoted_string;

chunk_size = digit+;

data := |*

 /[^\.\r\n]/ /[^\r\n]/+ CRLF =>
 {
   auto len = te - ts - 2; // minus crlf
   msg.out().write(ts, len);
   msg.out() << '\n';
 };

 '.' /[^\r\n]/+ CRLF =>
 {
   auto len = te - ts - 3; // minus crlf and leading '.'
   msg.out().write(ts + 1, len);
   msg.out() << '\n';
 };

 CRLF =>
 {
   msg.out() << '\n';
 };

 '.' CRLF =>
 {
   session.data_msg_done(msg);
   fgoto main;
 };

 any =>
 {
   LOG(ERROR) << "data protocol error [[" << string(ts, static_cast<size_t>(te - ts)) << "]]";
   fgoto main;
 };

*|;

main := |*

 "EHLO"i SP (Domain | address_literal) CRLF =>
 {
   char const* beg = ts + 5;
   char const* end = te - 2;
   session.ehlo(string(beg, end - beg));
 };

 "HELO"i SP Domain CRLF =>
 {
   char const* beg = ts + 5;
   char const* end = te - 2;
   session.helo(string(beg, end - beg));
 };

 "MAIL FROM:"i Reverse_path (SP Mail_parameters)? CRLF =>
 {
   session.mail_from(Mailbox(mb_loc, mb_dom), parameters);

   mb_loc.clear();
   mb_dom.clear();
   parameters.clear();
 };

 "RCPT TO:"i Forward_path (SP Rcpt_parameters)? CRLF =>
 {
   session.rcpt_to(Mailbox(mb_loc, mb_dom), parameters);

   mb_loc.clear();
   mb_dom.clear();
   parameters.clear();
 };

 "DATA"i CRLF =>
 {
   if (session.data_start()) {
     msg = session.data_msg();
     LOG(INFO) << "calling data\n";
     fgoto data;
   }
 };

 "BDAT"i SP chunk_size (SP "LAST"i @last)? CRLF =>
 {
   LOG(FATAL) << "BDAT not supported";
   // eat data from our buffer
   if (last) {
     last = false;
   }
 };

 "RSET"i CRLF =>
 {
   session.rset();
 };

 "NOOP"i (SP String)? CRLF =>
 {
   session.noop();
 };

 "VRFY"i (SP String)? CRLF =>
 {
   session.vrfy();
 };

 "HELP"i (SP String)? CRLF =>
 {
   session.help();
 };

 "STARTTLS"i CRLF =>
 {
   session.starttls();
 };

 "QUIT"i CRLF =>
 {
   session.quit();
 };

*|;

}%%

%% write data nofinal;

template <typename T>
class Stack {
public:
  using size_type = size_t;

  T& operator[](size_type idx)
  {
    CHECK_EQ(idx, 0);
    return x_;
  }

  Stack() { x_ = 0; }

private:
  T x_;
};

void scanner(Session& session)
{
  Message msg("");

  char const* mb_loc_beg{nullptr};
  char const* mb_loc_end{nullptr};
  char const* mb_dom_beg{nullptr};
  char const* mb_dom_end{nullptr};

  bool last{false};

  std::string mb_loc;
  std::string mb_dom;

  std::pair<string, string> param;
  std::unordered_map<string, string> parameters;

  static char buf[BUFSIZE];

  char* ts = nullptr;
  char* te = nullptr;
  char* pe = nullptr;

  size_t have = 0;
  bool done = false;

  Stack<int> stack;

#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
  char const* eof = nullptr;
  int cs, act;

  %% write init;

  while (!done) {
    std::streamsize space = BUFSIZE - have;

    if (space == 0) {
      // We've used up the entire buffer storing an already-parsed token
      // prefix that must be preserved.
      session.error("out of buffer space");
      LOG(FATAL) << "out of buffer space";
    }

    char* p = buf + have;
    session.in().peek(); // buffer up some input
    std::streamsize len = session.in().readsome(p, space);
    pe = p + len;
    eof = nullptr;

    // Check if this is the end of file.
    if (len == 0) {
      if (have == 0) {
        LOG(INFO) << "no more input";
        std::exit(EXIT_SUCCESS);
      }
      eof = pe;
      LOG(INFO) << "done";
      done = true;
    }

    LOG(INFO) << "exec \'" << string_view(buf, have + len) << "'";

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wold-style-cast"

    %% write exec;

#pragma GCC diagnostic pop

    if (cs == smtp_error) {
      session.error("parse error");
      break;
    }

    if (ts == nullptr) {
      have = 0;
    }
    else {
      // There is a prefix to preserve, shift it over.
      have = pe - ts;
      memmove(buf, ts, have);

      // adjust ptrs
      auto delta = ts - buf;

      mb_loc_beg -= delta;
      mb_loc_end -= delta;
      mb_dom_beg -= delta;
      mb_dom_end -= delta;

      te = buf + (te - ts);
      ts = buf;
    }
  }
}

int main(int argc, char const* argv[])
{
  std::ios::sync_with_stdio(false);
  //  google::InitGoogleLogging(argv[0]);

  Session session;
  session.greeting();

  scanner(session);
  LOG(INFO) << "scanner returned";
}