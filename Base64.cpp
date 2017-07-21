#include "Base64.hpp"

#include <algorithm>
#include <cctype>

using std::string;
using std::experimental::string_view;

namespace Base64 {

constexpr char const CHARSET[]{
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"};

constexpr std::string::size_type LINE_LENGTH = 78;

namespace {
unsigned char CHARSET_find(unsigned char ch)
{
  return static_cast<unsigned char>(
      std::find(std::begin(CHARSET), std::end(CHARSET), ch)
      - std::begin(CHARSET));
}
}

std::string enc(string_view text, bool wrap)
{
  unsigned char group_8bit[3];
  unsigned char group_6bit[4];
  int count_3_chars = 0;

  std::string enc_text;
  string::size_type line_len = 0;

  for (string::size_type cur_char = 0; cur_char < text.length(); cur_char++) {
    group_8bit[count_3_chars++] = text[cur_char];
    if (count_3_chars == 3) {
      group_6bit[0] = (group_8bit[0] & 0xfc) >> 2;
      group_6bit[1]
          = ((group_8bit[0] & 0x03) << 4) + ((group_8bit[1] & 0xf0) >> 4);
      group_6bit[2]
          = ((group_8bit[1] & 0x0f) << 2) + ((group_8bit[2] & 0xc0) >> 6);
      group_6bit[3] = group_8bit[2] & 0x3f;

      for (int i = 0; i < 4; i++)
        enc_text += CHARSET[group_6bit[i]];
      count_3_chars = 0;
      line_len += 4;
    }

    if (wrap && (line_len == LINE_LENGTH - 2)) {
      enc_text += "\r\n";
      line_len = 0;
    }
  }

  // encode remaining characters if any

  if (count_3_chars > 0) {
    for (int i = count_3_chars; i < 3; i++)
      group_8bit[i] = '\0';

    group_6bit[0] = (group_8bit[0] & 0xfc) >> 2;
    group_6bit[1]
        = ((group_8bit[0] & 0x03) << 4) + ((group_8bit[1] & 0xf0) >> 4);
    group_6bit[2]
        = ((group_8bit[1] & 0x0f) << 2) + ((group_8bit[2] & 0xc0) >> 6);
    group_6bit[3] = group_8bit[2] & 0x3f;

    for (int i = 0; i < count_3_chars + 1; i++) {
      if (wrap && (line_len == LINE_LENGTH - 2)) {
        enc_text += "\r\n";
        line_len = 0;
      }
      enc_text += CHARSET[group_6bit[i]];
      line_len++;
    }

    while (count_3_chars++ < 3) {
      if (wrap && (line_len == LINE_LENGTH - 2)) {
        enc_text += "\r\n";
        line_len = 0;
      }
      enc_text += '=';
      line_len++;
    }
  }

  return enc_text;
}

constexpr bool is_base64char(char ch)
{
  return std::isalnum(ch) || ch == '+' || ch == '/';
}

std::string dec(string_view text)
{
  std::string dec_text;
  unsigned char group_6bit[4];
  unsigned char group_8bit[3];
  int count_4_chars = 0;

  for (string::size_type ch = 0; ch < text.length() && text[ch] != '='; ch++) {
    if ((text[ch] == '\r') || (text[ch] == '\n'))
      continue;

    if (!is_base64char(text[ch]))
      throw std::invalid_argument("bad character in decode");

    group_6bit[count_4_chars++] = text[ch];
    if (count_4_chars == 4) {
      for (int i = 0; i < 4; i++)
        group_6bit[i] = CHARSET_find(group_6bit[i]);

      group_8bit[0] = (group_6bit[0] << 2) + ((group_6bit[1] & 0x30) >> 4);
      group_8bit[1]
          = ((group_6bit[1] & 0xf) << 4) + ((group_6bit[2] & 0x3c) >> 2);
      group_8bit[2] = ((group_6bit[2] & 0x3) << 6) + group_6bit[3];

      for (int i = 0; i < 3; i++)
        dec_text += group_8bit[i];
      count_4_chars = 0;
    }
  }

  // decode remaining characters if any

  if (count_4_chars > 0) {
    for (int i = count_4_chars; i < 4; i++)
      group_6bit[i] = '\0';

    for (int i = 0; i < 4; i++)
      group_6bit[i] = CHARSET_find(group_6bit[i]);

    group_8bit[0] = (group_6bit[0] << 2) + ((group_6bit[1] & 0x30) >> 4);
    group_8bit[1]
        = ((group_6bit[1] & 0xf) << 4) + ((group_6bit[2] & 0x3c) >> 2);
    group_8bit[2] = ((group_6bit[2] & 0x3) << 6) + group_6bit[3];

    for (int i = 0; i < count_4_chars - 1; i++)
      dec_text += group_8bit[i];
  }

  return dec_text;
}
}
