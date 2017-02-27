/*
    This file is part of ghsmtp - Gene's simple SMTP server.
    Copyright © 2013-2017 Gene Hightower <gene@digilicious.com>

    This program is free software: you can redistribute it and/or
    modify it under the terms of the GNU Affero General Public License
    as published by the Free Software Foundation, version 3.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
    Affero General Public License for more details.

    You should have received a copy of the GNU Affero General Public
    License along with this program.  See the file COPYING.  If not,
    see <http://www.gnu.org/licenses/>.

    Additional permission under GNU AGPL version 3 section 7

    If you modify this program, or any covered work, by linking or
    combining it with the OpenSSL project's OpenSSL library (or a
    modified version of that library), containing parts covered by the
    terms of the OpenSSL or SSLeay licenses, I, Gene Hightower grant
    you additional permission to convey the resulting work.
    Corresponding Source for a non-source form of such a combination
    shall include the source code for the parts of OpenSSL used as
    well as that of the covered work.
*/

#include "Pill.hpp"

#include <iostream>
#include <string.h>

int main(int arcv, char* argv[])
{
  google::InitGoogleLogging(argv[0]);

  Pill red, blue;
  CHECK(red != blue);

  std::stringstream red_str, blue_str;

  red_str << red;
  blue_str << blue;

  CHECK_NE(red_str.str(), blue_str.str());

  CHECK_EQ(13U, red_str.str().length());
  CHECK_EQ(13U, blue_str.str().length());

  Pill red2(red);
  CHECK_EQ(red, red2);

  std::cout << "sizeof(Pill) == " << sizeof(Pill) << '\n';
}
