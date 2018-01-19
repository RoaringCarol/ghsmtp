USES := ldns libglog libidn2 opendkim openssl

LDLIBS += \
	-lboost_filesystem \
	-lboost_iostreams \
	-lboost_system \
	-lcdb \
	-lgflags \
	-lmagic \
	-lopendmarc \
	-lregdom \
	-lspf2 \
	-lunistring

PROGRAMS := msg smtp sasl snd

msg_STEMS := msg \
	CDB \
	DKIM \
	DMARC \
	DNS \
	Domain \
	IP \
	IP4 \
	IP6 \
	SPF \
	esc \
	osutil

sasl_STEMS := sasl \
	Base64 \
	POSIX \
	SockBuffer \
	TLS-OpenSSL \
	esc

smtp_STEMS := smtp \
	CDB \
	DNS \
	Domain \
	IP \
	IP4 \
	IP6 \
	Message \
	POSIX \
	Pill \
	SPF \
	Session \
	Sock \
	SockBuffer \
	TLS-OpenSSL \
	esc \
	osutil

snd_STEMS := snd \
	Base64 \
	DKIM \
	DNS \
	Domain \
	IP \
	IP4 \
	IP6 \
	Magic \
	POSIX \
	Pill \
	SPF \
	Sock \
	SockBuffer \
	TLS-OpenSSL \
	esc \
	osutil

TESTS := \
	Base64-test \
	CDB-test \
	DNS-test \
	Domain-test \
	IP4-test \
	IP6-test \
	Magic-test \
	Mailbox-test \
	Message-test \
	Now-test \
	POSIX-test \
	Pill-test \
	SPF-test \
	Session-test \
	Sock-test \
	SockBuffer-test \
	TLD-test \
	TLS-OpenSSL-test \
	esc-test

Base64-test_STEMS := Base64
CDB-test_STEMS := CDB
DNS-test_STEMS := DNS
Domain-test_STEMS := DNS Domain IP IP4 IP6
IP4-test_STEMS := DNS IP4
IP6-test_STEMS := DNS IP6
Magic-test_STEMS := Magic
Mailbox-test_STEMS := DNS Domain IP IP4 IP6
Message-test_STEMS := DNS Domain IP IP4 IP6 Message Pill
POSIX-test_STEMS := POSIX
Pill-test_STEMS := Pill
SPF-test_STEMS := DNS IP4 IP6 SPF
Session-test_STEMS := CDB DNS Domain IP IP4 IP6 Message POSIX Pill SPF Session Sock SockBuffer TLS-OpenSSL esc osutil
Sock-test_STEMS := POSIX Sock SockBuffer TLS-OpenSSL esc
SockBuffer-test_STEMS := POSIX Sock SockBuffer TLS-OpenSSL esc
TLS-OpenSSL-test_STEMS := POSIX TLS-OpenSSL
esc-test_STEMS := esc

databases := \
	accept_domains.cdb \
	black.cdb \
	ip-black.cdb \
	ip-white.cdb \
	three-level-tlds.cdb \
	two-level-tlds.cdb \
	white.cdb \

all:: $(databases) public_suffix_list.dat

TMPDIR ?= /tmp
TEST_MAILDIR=$(TMPDIR)/Maildir

$(TEST_MAILDIR):
	mkdir -p $@

#smtp.cpp: smtp.rl
#	ragel -o smtp.cpp smtp.rl

clean::
	rm -rf $(TEST_MAILDIR)

%.cdb : %
	./cdb-gen < $< | cdb -c $@

clean::
	rm -f accept_domains.cdb
	rm -f black.cdb
	rm -f ip-black.cdb
	rm -f ip-white.cdb
	rm -f three-level-tlds.cdb
	rm -f two-level-tlds.cdb
	rm -f white.cdb cdb-gen

real-clean::
	rm -f two-level-tlds three-level-tlds public_suffix_list.dat

accept_domains.cdb: accept_domains cdb-gen
black.cdb: black cdb-gen
ip-black.cdb: ip-black cdb-gen
ip-white.cdb: ip-white cdb-gen
three-level-tlds.cdb: three-level-tlds cdb-gen
two-level-tlds.cdb: two-level-tlds cdb-gen
white.cdb: white cdb-gen

two-level-tlds three-level-tlds:
	wget --timestamping $(patsubst %,http://george.surbl.org/%,$@)

public_suffix_list.dat:
	wget --timestamping https://publicsuffix.org/list/public_suffix_list.dat

include MKUltra/rules

regression: $(programs) $(TEST_MAILDIR)
	MAILDIR=$(TEST_MAILDIR) valgrind ./smtp < input.txt
	ls -l smtp
	size smtp
