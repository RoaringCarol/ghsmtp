#include "Message.hpp"

#include <iostream>

#include <cstdlib>

using namespace std::string_literals;

int main(int argc, char* argv[])
{
  char env[] = "MAILDIR=/tmp/Maildir";
  PCHECK(putenv(env) == 0);

  Message msg;
  msg.open("example.com", 4096, Message::SpamStatus::ham);

  auto ms = "foo bar baz"s;
  msg.write(ms.data(), ms.size());
  msg.save();

  Message msg2;
  msg2.open("example.com", 4096, Message::SpamStatus::spam);

  CHECK(msg.id() != msg2.id());

  std::stringstream msg_str, msg2_str;

  msg_str << msg.id();
  msg2_str << msg2.id();

  CHECK_NE(msg_str.str(), msg2_str.str());

  msg2.trash();

  std::cout << "sizeof(Message) == " << sizeof(Message) << '\n';
}
