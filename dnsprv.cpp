#include <iostream>

#include "Sock.hpp"
#include "osutil.hpp"

#include <glog/logging.h>

#include <netdb.h>
#include <sys/socket.h>
#include <sys/types.h>

auto constexpr addr6 = "2606:4700:4700::1111";
auto constexpr srv = "domain-s";

auto constexpr client_name = "digilicious.com";
auto constexpr server_name = "1dot1dot1dot1.cloudflare-dns.com";

int main(int argc, char* argv[])
{
  uint16_t port = osutil::get_port(srv);

  auto const fd{socket(AF_INET6, SOCK_STREAM, 0)};
  PCHECK(fd >= 0) << "socket() failed";

  auto in6{sockaddr_in6{}};
  in6.sin6_family = AF_INET6;
  in6.sin6_port = htons(port);
  CHECK_EQ(inet_pton(AF_INET6, addr6, reinterpret_cast<void*>(&in6.sin6_addr)),
           1);

  if (connect(fd, reinterpret_cast<const sockaddr*>(&in6), sizeof(in6))) {
    PLOG(FATAL) << "connect failed [" << addr6 << "]:" << port;
  }

  Sock sock(fd, fd);

  DNS::RR_set tlsa_rrs; // empty

  sock.starttls_client(client_name, server_name, tlsa_rrs, false);
  CHECK(sock.verified());

  
}
