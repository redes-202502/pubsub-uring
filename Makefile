CC=gcc
CFLAGS=-Wall -Wextra -O2 -std=c11

all: broker_tcp publisher_tcp subscriber_tcp broker_udp publisher_udp subscriber_udp

broker_tcp: broker_tcp.c
	$(CC) $(CFLAGS) -o $@ $<

publisher_tcp: publisher_tcp.c
	$(CC) $(CFLAGS) -o $@ $<

subscriber_tcp: subscriber_tcp.c
	$(CC) $(CFLAGS) -o $@ $<

broker_udp: broker_udp.c
	$(CC) $(CFLAGS) -o $@ $<

publisher_udp: publisher_udp.c
	$(CC) $(CFLAGS) -o $@ $<

subscriber_udp: subscriber_udp.c
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f broker_tcp publisher_tcp subscriber_tcp broker_udp publisher_udp subscriber_udp
