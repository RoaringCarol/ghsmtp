#include "IP.hpp"

#include "IP4.hpp"
#include "IP6.hpp"

#include <glog/logging.h>

namespace IP {
bool is_routable(std::string_view addr)
{
  if (IP4::is_address(addr))
    return IP4::is_routable(addr);
  if (IP6::is_address(addr))
    return IP6::is_routable(addr);
  return false;
}

bool is_address(std::string_view addr)
{
  return IP4::is_address(addr) || IP6::is_address(addr);
}

bool is_address_literal(std::string_view addr)
{
  return IP4::is_address_literal(addr) || IP6::is_address_literal(addr);
}

std::string to_address_literal(std::string_view addr)
{
  if (IP4::is_address(addr))
    return IP4::to_address_literal(addr);
  if (IP6::is_address(addr))
    return IP6::to_address_literal(addr);
  LOG(FATAL) << "not a valid IP address " << addr;
}

std::string_view to_address(std::string_view addr)
{
  if (IP4::is_address_literal(addr))
    return IP4::to_address(addr);
  if (IP6::is_address_literal(addr))
    return IP6::to_address(addr);
  LOG(FATAL) << "not a valid IP address literal " << addr;
}

std::string reverse(std::string_view addr)
{
  if (IP4::is_address(addr))
    return IP4::reverse(addr);
  if (IP6::is_address(addr))
    return IP6::reverse(addr);
  LOG(FATAL) << "not a valid IP address " << addr;
}

std::string fcrdns(std::string_view addr)
{
  // <https://en.wikipedia.org/wiki/Forward-confirmed_reverse_DNS>

  if (IP4::is_address(addr))
    return IP4::fcrdns(addr);
  if (IP6::is_address(addr))
    return IP6::fcrdns(addr);
  LOG(FATAL) << "not a valid IP address " << addr;
}
} // namespace IP
