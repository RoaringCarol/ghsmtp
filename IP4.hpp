#ifndef IP4_DOT_HPP
#define IP4_DOT_HPP

#include <experimental/string_view>

#include <boost/xpressive/xpressive.hpp>

#include <glog/logging.h>

namespace IP4 {

inline boost::xpressive::cregex single_octet()
{
  using namespace boost::xpressive;

  // clang-format off
  return (as_xpr('2') >> '5' >> range('0', '5'))  // 250->255
         | ('2' >> range('0', '4') >> _d)         // 200->249
         | (range('0', '1') >> repeat<2>(_d))     // 000->199
         | repeat<1, 2>(_d);                      //   0->99
  // clang-format on
}

inline bool is_address(std::experimental::string_view addr)
{
  using namespace boost::xpressive;

  auto octet = single_octet();
  cregex re = octet >> '.' >> octet >> '.' >> octet >> '.' >> octet;
  cmatch matches;
  return regex_match(addr.begin(), addr.end(), matches, re);
}

inline bool is_bracket_address(std::experimental::string_view addr)
{
  using namespace boost::xpressive;

  auto octet = single_octet();
  cregex re
      = '[' >> octet >> '.' >> octet >> '.' >> octet >> '.' >> octet >> ']';
  cmatch matches;
  return regex_match(addr.begin(), addr.end(), matches, re);
}

inline std::string reverse(std::experimental::string_view addr)
{
  using namespace boost::xpressive;

  auto octet = single_octet();
  cregex re = (s1 = octet) >> '.' >> (s2 = octet) >> '.' >> (s3 = octet) >> '.'
              >> (s4 = octet);
  cmatch matches;
  CHECK(regex_match(addr.begin(), addr.end(), matches, re))
      << "IP4::reverse called with bad dotted quad: " << addr;

  std::ostringstream reverse;
  for (int n = 4; n > 0; --n) {
    std::experimental::string_view octet(matches[n].first,
                                         matches[n].second - matches[n].first);
    reverse << octet << '.'; // and leave a trailing '.'
  }
  return reverse.str();
}
}

#endif // IP4_DOT_HPP
