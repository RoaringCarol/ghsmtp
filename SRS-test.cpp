#include "SRS.hpp"

#include <gflags/gflags.h>

#include <glog/logging.h>

#include <fmt/format.h>
#include <fmt/ostream.h>

int main(int argc, char* argv[])
{
  std::ios::sync_with_stdio(false);
  google::ParseCommandLineFlags(&argc, &argv, true);

  SRS srs;

  char const* sender = "gene@digilicious.com";
  char const* alias  = "♥.digilicious.com";
  char const* alias2 = "xn--g6h.digilicious.com";

  auto const fwd  = srs.forward(sender, alias);
  auto const fwd2 = srs.forward(fwd.c_str(), alias2);
  auto const rev  = srs.reverse(fwd.c_str());

  LOG(INFO) << "sender == " << sender;
  LOG(INFO) << "alias  == " << alias;
  LOG(INFO) << "  fwd  == " << fwd;
  LOG(INFO) << "  fwd2 == " << fwd2;
  LOG(INFO) << "   rev == " << rev;

  CHECK_EQ(rev, sender);
}
