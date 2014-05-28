server: server.c
	gcc -Wall -pthread -lpthread -pedantic -g -o server server.c

.PHONY: clean

clean:
	rm server
