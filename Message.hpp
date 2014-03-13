/*
    This file is part of ghsmtp - Gene's simple SMTP server.
    Copyright (C) 2013  Gene Hightower <gene@digilicious.com>

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU Affero General Public License as
    published by the Free Software Foundation, either version 3 of the
    License, or (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU Affero General Public License for more details.

    You should have received a copy of the GNU Affero General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#ifndef MESSAGE_DOT_HPP
#define MESSAGE_DOT_HPP

#include <cstring>
#include <fstream>
#include <iomanip>
#include <random>
#include <sstream>

#include <sys/types.h>
#include <pwd.h>

#include "Now.hpp"
#include "Pill.hpp"

class Message {
public:
  Message(Message const&) = delete;
  Message& operator=(Message const&) = delete;

  Message(std::string const& fqdn, std::random_device& rd) : s_(rd)
  {
    std::string maildir;

    char const* ev = getenv("MAILDIR");
    if (ev) {
      maildir = ev;
    } else {
      ev = getenv("HOME");
      if (ev) {
        maildir = ev;
      } else {
        long bufsize = sysconf(_SC_GETPW_R_SIZE_MAX);
        if (bufsize == -1)    // Value was indeterminate.
          bufsize = 4 * 1024; // Generous size.

        passwd pwd;
        passwd* pw;

      fresh_vector:
        std::vector<char> buf(bufsize);

        int e = getpwuid_r(getuid(), &pwd, &buf[0], bufsize, &pw);
        if (nullptr == pw) {
          CHECK_EQ(ERANGE, e) << "getpwuid_r() error errno==" << e << ": "
                              << std::strerror(e);
          bufsize *= 2;
          goto fresh_vector;
        }
        maildir = pw->pw_dir;
      }
      maildir += "/Maildir";
    }

    // Unique name, see: <http://cr.yp.to/proto/maildir.html>
    std::ostringstream uniq;
    uniq << then_.sec() << "." << "R" << s_ << "." << fqdn;

    tmpfn_ = maildir + "/tmp/" + uniq.str();
    newfn_ = maildir + "/new/" + uniq.str();

    // open
    ofs_.exceptions(std::ifstream::failbit | std::ifstream::badbit);
    ofs_.open(tmpfn_.c_str());
  }
  Pill const& id() const
  {
    return s_;
  }
  Now const& when() const
  {
    return then_;
  }
  std::ostream& out()
  {
    return ofs_;
  }
  void save()
  {
    ofs_.close();
    PCHECK(rename(tmpfn_.c_str(), newfn_.c_str()) == 0);
  }
  void trash()
  {
    ofs_.close();
    PCHECK(remove(tmpfn_.c_str()) == 0);
  }

private:
  Pill s_;
  Now then_;

  std::ofstream ofs_;

  std::string tmpfn_;
  std::string newfn_;
};

#endif // MESSAGE_DOT_HPP
