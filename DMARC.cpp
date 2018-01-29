#include "DMARC.hpp"

#include "osutil.hpp"

namespace {
u_char* uc(char const* cp)
{
  return reinterpret_cast<u_char*>(const_cast<char*>((cp)));
}
} // namespace

namespace OpenDMARC {
Lib::Lib()
{
  lib_.tld_type = OPENDMARC_TLD_TYPE_MOZILLA;
  auto const path = osutil::get_config_dir() / "public_suffix_list.dat";
  CHECK(fs::exists(path)) << "can't find " << path;
  auto native = path.string();
  strcpy(reinterpret_cast<char*>(lib_.tld_source_file), native.c_str());
  auto const status = opendmarc_policy_library_init(&lib_);
  CHECK_EQ(status, DMARC_PARSE_OKAY) << opendmarc_policy_status_to_str(status);
}

Lib::~Lib() { opendmarc_policy_library_shutdown(&lib_); }

Policy::~Policy()
{
  if (pctx_) {
    opendmarc_policy_connect_shutdown(pctx_);
    pctx_ = nullptr;
  }
}

void Policy::init(char const* ip)
{
  auto const is_ipv6 = IP6::is_address(ip);
  pctx_ = CHECK_NOTNULL(opendmarc_policy_connect_init(uc(ip), is_ipv6));
}

bool Policy::store_from_domain(char const* from_domain)
{
  auto const status
      = opendmarc_policy_store_from_domain(pctx_, uc(from_domain));
  if (status != DMARC_PARSE_OKAY) {
    LOG(WARNING) << "from_domain == " << from_domain;
    LOG(WARNING) << opendmarc_policy_status_to_str(status);
    return false;
  }
  return true;
}

bool Policy::store_dkim(char const* d_equal_domain,
                        int dkim_result,
                        char const* human_result)
{
  auto const status = opendmarc_policy_store_dkim(
      pctx_, uc(d_equal_domain), dkim_result, uc(human_result));
  if (status != DMARC_PARSE_OKAY) {
    LOG(WARNING) << "d_equal_domain == " << d_equal_domain;
    LOG(WARNING) << opendmarc_policy_status_to_str(status);
    return false;
  }
  return true;
}

bool Policy::store_spf(char const* domain,
                       int result,
                       int origin,
                       char const* human_readable)
{
  auto const status = opendmarc_policy_store_spf(pctx_, uc(domain), result,
                                                 origin, uc(human_readable));
  if (status != DMARC_PARSE_OKAY) {
    LOG(WARNING) << "domain == " << domain;
    LOG(WARNING) << opendmarc_policy_status_to_str(status);
    return false;
  }
  return true;
}

bool Policy::query_dmarc(char const* domain)
{
  auto const status = opendmarc_policy_query_dmarc(pctx_, uc(domain));
  if (status != DMARC_PARSE_OKAY) {
    LOG(WARNING) << domain << ": " << opendmarc_policy_status_to_str(status);
    return false;
  }
  return true;
}

Advice Policy::get_advice()
{
  auto const status = opendmarc_get_policy_to_enforce(pctx_);

  switch (status) {
  case DMARC_PARSE_ERROR_NULL_CTX:
    LOG(WARNING) << "NULL pctx value";
    return Advice::NONE;

  case DMARC_FROM_DOMAIN_ABSENT:
    LOG(WARNING) << "no From: domain";

  case DMARC_POLICY_ABSENT:
    return Advice::NONE;

  case DMARC_POLICY_PASS:
    return Advice::ACCEPT;

  case DMARC_POLICY_REJECT:
    return Advice::REJECT;

  case DMARC_POLICY_QUARANTINE:
    return Advice::QUARANTINE;

  case DMARC_POLICY_NONE:
    return Advice::NONE;
  }

  LOG(FATAL) << "unknown status";
}

} // namespace OpenDMARC
