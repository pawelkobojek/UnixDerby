server: server.c
	gcc -Wall -pthread -lpthread -pedantic -o server server.c
debug: server.c
	gcc -Wall -pthread -lpthread -pedantic -g -o server server.c
valrun: server
	valgrind --leak-check=full ./server 8080

.PHONY: clean

clean:
	rm server
