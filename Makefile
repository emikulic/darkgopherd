CC=gcc
CFLAGS=-pedantic -Wall -O2 -g -fsanitize=address

darkgopherd: darkgopherd.c
	$(CC) $(CFLAGS) -o $@ $<
