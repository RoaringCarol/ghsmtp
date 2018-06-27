#include "DNS-fcrdns.hpp"
#include "DNS.hpp"
#include "Domain.hpp"

#include <algorithm>
#include <random>

#include <glog/logging.h>

int main(int argc, char const* argv[])
{
  DNS::Resolver res;

  DNS::Query q_dee(res, DNS::RR_type::A, "dee.test.digilicious.com");
  CHECK(!q_dee.nx_domain());
  CHECK(q_dee.bogus_or_indeterminate());

  auto goog_a{"google-public-dns-a.google.com"};
  auto goog_b{"google-public-dns-b.google.com"};

  auto addrs_a = res.get_records(DNS::RR_type::A, goog_a);

  CHECK_EQ(addrs_a.size(), 1U);
  CHECK_EQ(strcmp(std::get<DNS::RR_A>(addrs_a[0]).c_str(), "8.8.8.8"), 0);

  auto addrs_b = res.get_records(DNS::RR_type::A, goog_b);
  CHECK_EQ(addrs_b.size(), 1U);
  CHECK_EQ(strcmp(std::get<DNS::RR_A>(addrs_b[0]).c_str(), "8.8.4.4"), 0);

  auto aaaaddrs_a = res.get_records(DNS::RR_type::AAAA, goog_a);
  CHECK_EQ(aaaaddrs_a.size(), 1U);
  CHECK_EQ(strcmp(std::get<DNS::RR_AAAA>(aaaaddrs_a[0]).c_str(),
                  "2001:4860:4860::8888"),
           0);

  auto aaaaddrs_b = res.get_records(DNS::RR_type::AAAA, goog_b);
  CHECK_EQ(aaaaddrs_b.size(), 1U);
  CHECK_EQ(strcmp(std::get<DNS::RR_AAAA>(aaaaddrs_b[0]).c_str(),
                  "2001:4860:4860::8844"),
           0);

  auto mxes = res.get_records(DNS::RR_type::MX, "anyold.host");

  // RFC 5321 section 5.1 “Locating the Target Host”
  std::shuffle(mxes.begin(), mxes.end(), std::random_device());
  std::sort(mxes.begin(), mxes.end(), [](DNS::RR const& a, DNS::RR const& b) {
    if (std::holds_alternative<DNS::RR_MX>(a)
        && std::holds_alternative<DNS::RR_MX>(b)) {
      return std::get<DNS::RR_MX>(a).preference()
             < std::get<DNS::RR_MX>(b).preference();
    }
    LOG(WARNING) << "non MX records in answer section";
    return false;
  });

  CHECK_EQ(std::get<DNS::RR_MX>(mxes[0]).exchange(), "digilicious.com");
  CHECK_EQ(std::get<DNS::RR_MX>(mxes[0]).preference(), 1);

  auto as = res.get_records(DNS::RR_type::A, "amazon.com");
  CHECK_GE(as.size(), 1);

  DNS::Query q(res, DNS::RR_type::TLSA, "_25._tcp.digilicious.com");
  CHECK(q.authentic_data()) << "TLSA records must be authenticated";

  DNS::Query q_noexist(res, DNS::RR_type::A,
                       "does-not-exist.test.digilicious.com");
  CHECK(q_noexist.nx_domain());
  CHECK(!q_noexist.bogus_or_indeterminate());

  auto cmxes = res.get_records(DNS::RR_type::MX, "cname.test.digilicious.com");
  for (auto const& cmx : cmxes) {
    if (std::holds_alternative<DNS::RR_CNAME>(cmx)) {
      CHECK_EQ(std::get<DNS::RR_CNAME>(cmx).str(), "test.digilicious.com");
    }
    else {
      CHECK_EQ(std::get<DNS::RR_MX>(cmx).preference(), 10);
      CHECK_EQ(std::get<DNS::RR_MX>(cmx).exchange(), "digilicious.com");
    }
  }

  auto txts = res.get_records(DNS::RR_type::TXT, "digilicious.com");
  CHECK_GE(txts.size(), 1);

  // These IP addresses might be stable for a while.

  auto fcrdnses4 = fcrdns4(res, "1.1.1.1");
  CHECK_EQ(fcrdnses4.size(), 1);
  CHECK(Domain::match(fcrdnses4.front(), "1dot1dot1dot1.cloudflare-dns.com."))
      << "no match for " << fcrdnses4.front();

  auto fcrdnses6 = fcrdns6(res, "2606:4700:4700::1111");
  CHECK_EQ(fcrdnses6.size(), 1);
  CHECK(Domain::match(fcrdnses6.front(), "1dot1dot1dot1.cloudflare-dns.com."))
      << "no match for " << fcrdnses6.front();

  auto quad9 = fcrdns4(res, "9.9.9.9");
  CHECK(Domain::match(quad9.front(), "dns.quad9.net"))
      << "no match for " << quad9.front();
}
