CC=g++
CFLAGS=-ggdb -Wall -Wextra -pedantic -Werror
DEPS = httpd.h
SRCS = httpd.cpp
MAIN_SRCS = main.c $(SRCS)

default: httpd

httpd: httpd.o main.o threadpool.o
	$(CC) $(CFLAGS) -o httpd main.o httpd.o threadpool.o -lpthread

main.o: main.cpp $(DEPS)
	$(CC) -c main.cpp -I include

httpd.o: httpd.cpp $(DEPS)
	$(CC) -c httpd.cpp -I include

threadpool.o: threadpool.cpp threadpool.h
	$(CC) -c threadpool.cpp -I include

clean:
	rm -f httpd *.o
