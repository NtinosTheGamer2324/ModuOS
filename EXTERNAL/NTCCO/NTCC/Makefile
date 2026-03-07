CC=gcc
CFLAGS=-Wall -Wextra -std=c99 -g

OBJS=ntcc.o lexer.o parser.o codegen.o

all: ntcc

ntcc: $(OBJS)
	$(CC) $(CFLAGS) -o ntcc $(OBJS)

ntcc.o: ntcc.c lexer.h parser.h codegen.h
	$(CC) $(CFLAGS) -c ntcc.c

lexer.o: lexer.c lexer.h
	$(CC) $(CFLAGS) -c lexer.c

parser.o: parser.c parser.h lexer.h
	$(CC) $(CFLAGS) -c parser.c

codegen.o: codegen.c parser.h codegen.h
	$(CC) $(CFLAGS) -c codegen.c

clean:
	rm -f *.o ntcc
