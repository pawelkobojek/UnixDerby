#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <signal.h>
#include <pthread.h>
#include <limits.h>

#define BACKLOG 5
#define BUF_SIZE 32
#define MAX_NAME_LEN 16
#define LINE_BUF 256
#define MAX_HORSES 8

#define SERVER_CONF_FILE "conf"

#define ENTER_LOGIN_MSG "[SERVER MESSAGE] Enter login please:\n"
#define CANT_WITHDRAW_MSG "[SERVER MESSAGE] Cannot withdraw that amount.\n"
#define UNWN_CMD_MSG "[SERVER MESSAGE] Unkown command.\n"

#define ERR(source) (perror(source),\
		fprintf(stderr,"%s:%d\n",__FILE__,__LINE__),\
		exit(EXIT_FAILURE))

typedef struct {
	char name[MAX_NAME_LEN];
	int money;
} player;

typedef struct {
	player** players;
	int socket;
} player_th_data;

typedef struct {
	pthread_t tid;			/* Horse thread's id */
	pthread_mutex_t* mutex;		/* Pointer to mutex uesd to simulate race turns */
	pthread_cond_t* cond;		/* Pointer to conditional variable used to simulate race turns */
	pthread_barrier_t* barrier;	/* Pointer to barrier used to simulate race turns */
	char name[MAX_NAME_LEN];	/* Name of the horse */
	short running;			/* Tells wheter horse is running (==1 if so)  or not (==0) */
} horse;

void usage(void) {
	fprintf(stderr, "USAGE: server port\n");
}

ssize_t bulk_read(int fd, char* buf, size_t count) {
	int c;
	size_t len = 0;
	do {
		c = TEMP_FAILURE_RETRY(read(fd, buf, count));
		if(c < 0) return c;
		if(0 == c) return len;
		buf += c;
		len += c;
		count -= c;
	} while(count > 0);
	return len;
}

ssize_t bulk_write(int fd, char* buf, size_t count) {
	int c;
	size_t len = 0;
	do {
		c = TEMP_FAILURE_RETRY(write(fd, buf, count));
		if( c < 0) return c;
		buf += c;
		len += c;
		count -= c;
	} while(count > 0);
	return len;
}

int sethandler(void (*f)(int), int sigNo) {
	struct sigaction act;
	memset(&act, 0, sizeof(struct sigaction));
	act.sa_handler = f;
	
	return sigaction(sigNo, &act, NULL);
}

/*
* Registers player in the system.
* Returns registered player's index.
*/
int register_player(player** players, int pl_len, char* name, int len) {
	int i, empty_slot;
	char buf[MAX_NAME_LEN];

	for(i = 0; i < pl_len; ++i) {
		if(players[i] == NULL) {
			empty_slot = i;
			break;
		}
	}

	if(i == pl_len) {
		fprintf(stderr, "Cannot register more players.\n");
		return -1;
	}

	if( (players[i] = (player*) malloc(sizeof(player))) == NULL) {
		ERR("malloc");
	}

	/* Copy name without CR and LF characters*/
	memcpy(buf, name, (len - 2 < MAX_NAME_LEN) ? len - 2 : MAX_NAME_LEN);

	players[empty_slot]->money = 0;
	strcpy(players[empty_slot]->name, buf);

	fprintf(stderr, "Player %s registered, n=%d\n", buf, len);
	
	return empty_slot;
}

void deposit(int socket, player* pl, int deposit) {
	pl->money += deposit;
	fprintf(stderr, "%s, %d\n", pl->name, pl->money);
	
}

void withdraw(int socket, player* pl, int amount) {
	if(pl->money - amount < 0) {
		if(bulk_write(socket, CANT_WITHDRAW_MSG, strlen(CANT_WITHDRAW_MSG)) < 0) {
			ERR("write");
		}
		
		return;
	}

	pl->money -= amount;

}

int get_value(char* buf) {
	char* second;
	
	second = strchr(buf, ' ');

	if(second && *(second + 1)) {
		printf("%s\n", second + 1);
		return atoi(second+1);
	}
	return 0;
}

/**
* 
*/
void* handle_connection(void* th_data) {
	player_th_data* data = (player_th_data*) th_data;
	int socket = data->socket, count, index;
	char buf[BUF_SIZE];
	player** players = data->players;

	memset(buf, 0, BUF_SIZE);

	if(bulk_write(socket, ENTER_LOGIN_MSG, strlen(ENTER_LOGIN_MSG)) < 0) {
		ERR("write");
	}

	if( (count = read(socket, buf, BUF_SIZE)) < 0) {
		ERR("read");
	}
	
	if(count == 0) {
		fprintf(stderr, "Connection closed.\n");
		pthread_exit(NULL);
	}

	index = register_player(players, 3,  buf, strlen(buf));

	while( (count = read(socket, buf, BUF_SIZE)) ) {
		if(count < 0) {
			ERR("read");
		}

		fprintf(stderr, "Data: %s", buf);

		switch(buf[0]) {
			case 'd':
				/* deposit */
				deposit(socket, players[index], get_value(buf));
				break;
			case 'w':
				/* withdraw */
				withdraw(socket, players[index], get_value(buf));
				break;
			case 'n':
				/* next */
				break;
			case 'l':
				/* last */
				break;
			default:
				if(bulk_write(socket, UNWN_CMD_MSG, strlen(UNWN_CMD_MSG)) < 0) {
	ERR("write");
}
				break;
		}		

		/* if(strcmp(buf, "deposit\n") == 0) {
			
		} else if(strcmp(buf, "withdraw\n") == 0) {
			
		} else if(strcmp(buf, "next\n") == 0) {
		 TODO 
		* Oddzielna komenda na czas i sklad? 
		
		
		} else if(strcmp(buf, "last\n") == 0) {
			
		} */
		
		memset(buf, 0, BUF_SIZE);
	}

	fprintf(stderr, "Connection ended.\n");
	free(th_data);
	pthread_exit(NULL);
}

void* horse_thread(void* horse_data) {
	horse* data = (horse*) horse_data;

	while(!data->running) {
		pthread_cond_wait(data->cond, data->mutex);
		fprintf(stderr, "Horse: %s awaken!\n", data->name);
	}

	fprintf(stderr, "Horse: %s waiting on barrier...\n", data->name);
	pthread_barrier_wait(data->barrier);
	fprintf(stderr, "Horse: %s done!\n", data->name);
	
	pthread_exit(NULL);
}

int make_socket(uint16_t port) {
	struct sockaddr_in name;
	int sock, t = 1;
	sock = socket(PF_INET, SOCK_STREAM, 0);
	if(sock < 0) {
		ERR("socket");
	}

	name.sin_family = AF_INET;
	name.sin_port = htons(port);
	name.sin_addr.s_addr = htonl(INADDR_ANY);

	if(setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &t, sizeof(t)) < 0) {
		ERR("setsockopt");
	}
	if(bind(sock, (struct sockaddr*) &name, sizeof(name)) < 0) {
		ERR("bind");
	}
	if(listen(sock, BACKLOG) < 0) {
		ERR("listen");
	}
	
	return sock;
}

int add_new_client(int sfd, fd_set* fdset, int* fdmax) {
	int fd;
	
	if( (fd = TEMP_FAILURE_RETRY(accept(sfd, NULL, NULL))) < 0) {
		ERR("accept");
	}

	FD_SET(fd, fdset);
	*fdmax = (*fdmax < fd ? fd : *fdmax);

	return fd;
}

void read_from_client(int socket, fd_set* active_fds) {
	int read_bytes;
	char buf[BUF_SIZE];
	if( (read_bytes = bulk_read(socket, buf, BUF_SIZE)) < 0) {
		ERR("read");
	}
	if(read_bytes == 0) {
		/* EOF from i-th socket */
		fprintf(stderr, "Connection ended\n");
		FD_CLR(socket, active_fds);
	} else {
		fprintf(stderr, "Recieved message: %s\n", buf);
	}
}

void server_work(int socket, horse* horses, int horse_count) {
	int sock, i=0;
	pthread_t id[MAX_HORSES];
	pthread_attr_t thattr;
	pthread_mutex_t race_mutex;
	pthread_cond_t race_cond;
	pthread_barrier_t race_barrier;
	player** players;
	player_th_data* thread_data;

	pthread_attr_init(&thattr);
	pthread_attr_setdetachstate(&thattr, PTHREAD_CREATE_DETACHED);

	pthread_mutex_init(&race_mutex, NULL);
	pthread_cond_init(&race_cond, NULL);

	if( (players = (player**) malloc(2 * sizeof(player*))) == NULL) {
		ERR("malloc");
	}

	for(i = 0; i < horse_count; ++i) {
		horses[i].mutex = &race_mutex;
		horses[i].cond = &race_cond;
		horses[i].barrier = &race_barrier;
		horses[i].running = 1;
		pthread_create(&horses[i].tid, &thattr, horse_thread, (void*) &horses[i]);
	}

	pthread_barrier_init(&race_barrier, NULL, horse_count);

	pthread_cond_broadcast(&race_cond);

	while(1) {
		fprintf(stderr, "Waiting for connection...\n");
		if( (sock = TEMP_FAILURE_RETRY(accept(socket, NULL, NULL))) < 0) {
			ERR("accept");
		}
		fprintf(stderr, "Accepted socket %d.\n", sock);
		if( (thread_data = (player_th_data*) malloc(sizeof(player_th_data))) == NULL) {
			ERR("malloc");
		}
		thread_data->socket = sock;
		thread_data->players = players;
		pthread_create(&id[i++], &thattr, handle_connection, (void*) thread_data);
	}

	for(i = 0; i < 2; ++i) {
		free(players[i]);
	}
	free(players);

	pthread_attr_destroy(&thattr);
	pthread_mutex_destroy(&race_mutex);
	pthread_cond_destroy(&race_cond);
	pthread_barrier_destroy(&race_barrier);
}

void read_configuration(horse** horses, int* horse_count, int* frequency) {
	FILE* file;
	char buf[LINE_BUF];
	int i;
	char* b;
	
	memset(buf, 0, LINE_BUF);

	if( (file = fopen(SERVER_CONF_FILE, "r")) == NULL) {
		ERR("fopen");
	}

	/* Read frequency */
	if(fgets(buf, LINE_BUF, file) == NULL) {
		goto closed;
	}

	*frequency = atoi(buf + strlen("FREQUENCY:"));

	/* Read horse count */
	if(fgets(buf, LINE_BUF, file) == NULL) {
		goto closed;
	}

	*horse_count = atoi(buf + strlen("HORSE_COUNT:"));

	if( (*horses = (horse*) calloc(*horse_count, sizeof(horse))) == NULL) {
		fclose(file);
		ERR("calloc");
	}
	
	for(i = 0; i < *horse_count; ++i) {
		if(fgets(buf, LINE_BUF, file) == NULL) {
			goto closed;
		}
		b = strchr(buf, ' ') + 1;
		strncpy((*horses)[i].name, b, MAX_NAME_LEN);
		(*horses)[i].name[strlen(b) - 1] = '\0';
		(*horses)[i].running = 0;
	}
closed:
	if(fclose(file) == EOF) {
		ERR("fclose");
	}
}

int main(int argc, char** argv) {
	int socket, i, horse_count, frequency;
	uint16_t port;
	horse* horses;

	if(argc != 2) {
		usage();
		exit(EXIT_FAILURE);
	}

	port = atoi(argv[1]);

	read_configuration(&horses, &horse_count, &frequency);

	for(i = 0; i < horse_count; ++i) {
		fprintf(stderr, "Horse#%d: %s\n", i, horses[i].name);
	}

	if(sethandler(SIG_IGN, SIGPIPE) < 0) {
		ERR("sigaction");
	}

	socket = make_socket(port);
	server_work(socket, horses, horse_count);
	
	if(TEMP_FAILURE_RETRY(close(socket)) < 0) {
		ERR("close");
	}

	return EXIT_SUCCESS;
}
