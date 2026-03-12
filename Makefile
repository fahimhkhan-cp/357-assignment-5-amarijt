CC=gcc
CFLAGS=-Wall -std=c99 -pedantic -g -O0

all: httpd

httpd: httpd.c
	$(CC) $(CFLAGS) httpd.c -o httpd

clean:
	rm -f httpd
