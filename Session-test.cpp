#include "Session.hpp"

#include <iostream>

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>

struct Session_test {
  static void test()
  {
    std::cout << "sizeof(Session)       == " << sizeof(Session) << '\n';
    std::cout << "sizeof(sock_)         == " << sizeof(Session::sock_) << '\n';
    std::cout << "sizeof(client_)       == " << sizeof(Session::client_)
              << '\n';
    std::cout << "sizeof(reverse_path_) == " << sizeof(Session::reverse_path_)
              << '\n';
    std::cout << "sizeof(forward_path_) == " << sizeof(Session::forward_path_)
              << '\n';
    std::cout << "sizeof(rd_)           == " << sizeof(Session::rd_) << '\n';
    std::cout << "sizeof(binarymime_)   == " << sizeof(Session::binarymime_)
              << '\n';

    setenv("GHSMTP_SERVER_ID", "example.com", 1);

    int fd_null = open("/dev/null", O_WRONLY);
    PCHECK(fd_null >= 0) << " can't open /dev/null";

    auto read_hook = []() { std::cout << "Session-test read_hook\n"; };
    Session sess(read_hook, STDIN_FILENO, fd_null);

    auto sender{Domain{"example.er"}};
    sess.verify_sender_domain_(sender);

    LOG(ERROR) << "Expect: 3 invalid sender domains:";
    CHECK(!sess.verify_sender_domain_(Domain("com")));
    CHECK(!sess.verify_sender_domain_(Domain("zzux.com")));
    CHECK(!sess.verify_sender_domain_(Domain("blogspot.com.ar")));
  }
};

int main(int argc, char* argv[]) { Session_test::test(); }
