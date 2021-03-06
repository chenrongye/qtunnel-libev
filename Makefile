CC = gcc
CFLAGS = -g -w
SRCS = qtunnel.c qtunnel.h

all: qtunnel-c

qtunnel-c: $(SRCS)
	$(CC) $(CFLAGS) $(SRCS) -o qtunnel-c -lcrypto -lpthread -lev

clean:
	rm -f qtunnel-c
