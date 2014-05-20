server: server.c
	gcc -Wall -pthread -lpthread -pedantic -o server server.c

.PHONY: clean

clean:
	rm server
