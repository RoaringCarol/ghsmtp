#include "Message.hpp"

#include <sys/types.h>

#include <sys/stat.h>

#include <fmt/format.h>
#include <fmt/ostream.h>

namespace {
auto locate_homedir() -> fs::path
{
  auto const homedir_ev{getenv("HOME")};
  if (homedir_ev) {
    CHECK(strcmp(homedir_ev, "/root")) << "should not run as root";
    CHECK(strcmp(homedir_ev, "/")) << "should not run in root directory";
    return homedir_ev;
  }
  else {
    errno = 0; // See GETPWNAM(3)
    passwd* pw;
    PCHECK(pw = getpwuid(getuid()));
    return pw->pw_dir;
  }
}

auto locate_maildir() -> fs::path
{
  auto const maildir_ev{getenv("MAILDIR")};
  if (maildir_ev) {
    return maildir_ev;
  }
  else {
    return locate_homedir() / "Maildir";
  }
}
} // namespace

void Message::open(std::string_view fqdn,
                   std::streamsize  max_size,
                   std::string_view folder)
{
  max_size_ = max_size;

  auto maildir = locate_maildir();

  if (!folder.empty()) {
    maildir /= folder;
  }

  tmpfn_ = maildir / "tmp";
  newfn_ = maildir / "new";

  umask(077);

  error_code ec;
  create_directories(tmpfn_, ec);
  create_directories(newfn_, ec);

  // Unique name, see: <https://cr.yp.to/proto/maildir.html>
  auto const uniq{fmt::format("{}.R{}.{}", then_.sec(), s_, fqdn)};
  tmpfn_ /= uniq;
  newfn_ /= uniq;

  // open
  ofs_.exceptions(std::ifstream::failbit | std::ifstream::badbit);
  ofs_.open(tmpfn_);
}

std::ostream& Message::write(char const* s, std::streamsize count)
{
  if (!size_error_ && (size_ + count) <= max_size_) {
    size_ += count;
    return ofs_.write(s, count);
  }
  else {
    size_error_ = true;
    return ofs_;
  }
}

void Message::try_close_()
{
  try {
    ofs_.close();
  }
  catch (std::system_error const& e) {
    LOG(ERROR) << e.what() << "code: " << e.code();
  }
  catch (std::exception const& e) {
    LOG(ERROR) << e.what();
  }
}

void Message::save()
{
  if (size_error()) {
    LOG(WARNING) << "message size error: " << size() << " exceeds "
                 << max_size();
  }

  try_close_();

  error_code ec;
  rename(tmpfn_, newfn_, ec);
  if (ec) {
    LOG(ERROR) << "can't rename " << tmpfn_ << " to " << newfn_ << ": " << ec;
  }
}

void Message::trash()
{
  try_close_();

  error_code ec;
  fs::remove(tmpfn_, ec);
  if (ec) {
    LOG(ERROR) << "can't remove " << tmpfn_ << ": " << ec;
  }
}
