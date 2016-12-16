/*
    This file is part of ghsmtp - Gene's simple SMTP server.
    Copyright (C) 2013  Gene Hightower <gene@digilicious.com>
*/

#include "Message.hpp"

#include <iostream>

#include <cstdlib>
#include <sys/utsname.h>

int main(int argc, char* argv[])
{
  google::InitGoogleLogging(argv[0]);

  char env[] = "MAILDIR=/tmp/Maildir";
  PCHECK(putenv(env) == 0);

  Message msg;
  msg.open("example.com", Message::SpamStatus::ham);

  msg.out() << "foo bar baz";
  msg.save();

  Message msg2;
  msg2.open("example.com", Message::SpamStatus::spam);

  CHECK(msg.id() != msg2.id());

  std::stringstream msg_str, msg2_str;

  msg_str << msg.id();
  msg2_str << msg2.id();

  CHECK_NE(msg_str.str(), msg2_str.str());

  msg2.trash();

  std::cout << "sizeof(Message) == " << sizeof(Message) << '\n';
}
