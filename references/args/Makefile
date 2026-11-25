# the compiler: gcc for C program, define as g++ for C++
CC = gcc

# compiler flags:
#  -g    adds debugging information to the executable file
#  -Wall turns on most, but not all, compiler warnings
CFLAGS  = -g -Wall

all: args cos

args: args.c
	$(CC) $(CFLAGS) -o args args.c

cos: cos.c
	$(CC) $(CFLAGS) -o cos cos.c -lm

clean:
	$(RM) args cos
