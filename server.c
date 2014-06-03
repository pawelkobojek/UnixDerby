#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <signal.h>
#include <pthread.h>
#include <limits.h>
#include <time.h>

#define BACKLOG 5
#define BUF_SIZE 32
#define MAX_NAME_LEN 16
#define LINE_BUF 256
#define MAX_HORSES_PER_RACE 8
#define MAX_HORSE_SPEED 14
#define RACE_DISTANCE 100
#define MAX_PLAYERS 8

#define STATE_NOT_RACING 101
#define STATE_RACING 102

#define SERVER_CONF_FILE "conf"

#define ENTER_LOGIN_MSG "[SERVER MESSAGE] Enter login please:\n"
#define CANT_WITHDRAW_MSG "[SERVER MESSAGE] Cannot withdraw that amount.\n"
#define UNWN_CMD_MSG "[SERVER MESSAGE] Unkown command.\n"
#define NO_SUCH_HORSE_MSG "[SERVER MESSAGE] There's no such horse.\n"
#define CANT_BET_MSG "[SERVER MESSAGE] Not enough money to bet.\n"

#define ERR(source) (perror(source),\
		fprintf(stderr,"%s:%d\n",__FILE__,__LINE__),\
		exit(EXIT_FAILURE))

volatile sig_atomic_t exit_flag = 0;

typedef struct {
	pthread_t tid;			/* Horse thread's id */
	pthread_mutex_t* mutex;		/* Pointer to mutex uesd to simulate race turns */
	pthread_cond_t* cond;		/* Pointer to conditional variable used to simulate race turns */
	char name[MAX_NAME_LEN];	/* Name of the horse */
	short running;			/* Tells wheter horse is running (==1 if so)  or not (==0) */
	unsigned int distance_run;	/* Distance run by horse in current race */
	float rest_factor;		/* Horse rest factor */
} horse;

typedef struct {
	char name[MAX_NAME_LEN];
	int money;
	horse* horse_bet;
	int money_bet;
	int* bank;
	pthread_mutex_t* bank_mutex;
} player;

typedef struct {
	horse* horse_data;
	horse** winner;
} horse_args;

typedef struct {
	player** players;
	int socket;
	int* state;
	int* bank;
	int horse_count;
	horse* horses;
	pthread_mutex_t* state_mutex;
	pthread_mutex_t* mutex;
	pthread_mutex_t* bank_mutex;
	pthread_cond_t* state_cond;
	pthread_cond_t* cond;
	horse*** curr_running_horses;
	horse** winner;
} player_th_data;

typedef struct {
	int socket;
	horse* horses;
	int horse_count;
	int* state;
	int* bank;
	player** players;
	horse** winner;
	pthread_cond_t* state_cond;
	pthread_cond_t* cond;
	pthread_mutex_t* state_mutex;
	pthread_mutex_t* mutex;
	pthread_mutex_t* bank_mutex;
	horse*** curr_running_horses;
} acc_clients_args;

typedef struct {
	horse* horses;
	horse** winner;
	player** players;
	int horse_count;
	int* state;
	int* bank;
	pthread_mutex_t* state_mutex;
	pthread_cond_t* state_cond;
	pthread_mutex_t* bank_mutex;
	pthread_mutex_t* mutex;
	pthread_cond_t* cond;
	horse*** curr_running_horses;
} race_args;

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

void bet(int socket, player* pl, char* cmd, horse* horses, int horse_count, int* bank, pthread_mutex_t* bank_mutex) {
	int i, money_bet;
	char* second, *third, *save_ptr;
	printf("cmd: %s\n", cmd);
	second = strtok_r(cmd, " ", &save_ptr);
	second = strtok_r(NULL, " ", &save_ptr);

	printf("second: %s\n", second);

	if(second) {
		third = strtok_r(NULL, " ", &save_ptr);
		if( third ) {	
			printf("third: %s\n", third);
			money_bet = atoi(third);
			if(money_bet > pl->money) {
				if(bulk_write(socket, CANT_BET_MSG, strlen(CANT_BET_MSG)) < 0) {
					ERR("write");
				}
				return;
			}
			for(i = 0; i < horse_count; ++i) {
				if( !strcmp(second, horses[i].name) ) {
					pl->horse_bet = &horses[i];
					pl->money_bet = money_bet;
					pl->money -= money_bet;
					pthread_mutex_lock(bank_mutex);
					*bank += money_bet;
					pthread_mutex_unlock(bank_mutex);
					return;
				}
			}
		}
	}

	if(bulk_write(socket, NO_SUCH_HORSE_MSG, strlen(NO_SUCH_HORSE_MSG)) < 0) {
		ERR("write");
	}
}

void print_info(int socket, player* pl) {
	char send_info[256];
	snprintf(send_info, 256, "Player: %s, money: %d, Bet on horse: %s with %d money\n", pl->name, pl->money, (pl->horse_bet) ? pl->horse_bet->name : "none", pl->money_bet);
	if(bulk_write(socket, send_info, strlen(send_info)) < 0) {
		ERR("write");
	}
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
	int socket = data->socket, count, index, i, result;
	char buf[BUF_SIZE];
	char race_status[256 * 8];
	char one_line_status[256];
	player** players = data->players;
	fd_set read_set;
	struct timeval tv, ttv = {0, 100};

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

	while(*data->state != STATE_NOT_RACING) {
		pthread_mutex_lock(data->mutex);
		if(pthread_cond_wait(data->cond, data->mutex) != 0) {
			ERR("pthread_mutex_unlock");
		}
		if(pthread_mutex_unlock(data->mutex) != 0) {
			ERR("pthread_mutex_unlock");
		}
		for(i = 0; i < data->horse_count; ++i) {
			if((*data->curr_running_horses)[i]) {
				snprintf(one_line_status, 256, "%s dinstance: %d\n", (*data->curr_running_horses)[i]->name, (*data->curr_running_horses)[i]->distance_run);
/*				snprintf(one_line_status, 256, "%s distance: %d\n", data->horses[i].name, data->horses[i].distance_run); */
				strcat(race_status, one_line_status);
			}
		}
		strcat(race_status, "\n");
		if(bulk_write(socket, race_status, 256*8) < 0) {
			ERR("write");
		}
		race_status[0]='\0';
	}

	if(players[index]->horse_bet && *data->winner && !strcmp(players[index]->horse_bet->name, (*data->winner)->name)) {
		players[index]->money += players[index]->money_bet * 2;
	}

	FD_ZERO(&read_set);
	FD_SET(socket, &read_set);
	count = 1;
	tv = ttv;
	while( count || (result = select(socket + 1, &read_set, NULL, NULL, &tv))) {
		if(result < 0 && errno != EINTR) {
			ERR("select");
		}
		tv = ttv;
		printf("About to check FD_ISSET...\n");
		if(FD_ISSET(socket, &read_set)) {
			printf("Reading\n");
			count = read(socket, buf, BUF_SIZE);
			
			if(count < 0) {
				ERR("read");
			}

			while(*data->state != STATE_NOT_RACING) {
				pthread_mutex_lock(data->mutex);
				if(pthread_cond_wait(data->cond, data->mutex) != 0) {
					ERR("pthread_cond_wait");
				}
				if(pthread_mutex_unlock(data->mutex) != 0) {
					ERR("pthread_mutex_unlock");
				}
				for(i = 0; i < MAX_HORSES_PER_RACE; ++i) {
					if((*data->curr_running_horses)[i]) {
						snprintf(one_line_status, 256, "%s distance: %d\n", (*data->curr_running_horses)[i]->name, (*data->curr_running_horses)[i]->distance_run);
	/*					snprintf(one_line_status, 256, "%s distance: %d\n", data->horses[i].name, data->horses[i].distance_run); */
						strcat(race_status, one_line_status);
					}
				}
				strcat(race_status, "\n");
				if(bulk_write(socket, race_status, 256*8) < 0) {
					ERR("write");
				}
				race_status[0]='\0';
			}
			if(players[index]->horse_bet && *data->winner && !strcmp(players[index]->horse_bet->name, (*data->winner)->name)) {
				players[index]->money += players[index]->money_bet * 2;
				printf("SSSS\n");
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
				case 'i':
					/* info */
					print_info(socket, players[index]);
				case 'n':
					/* next */
					break;
				case 'l':
					/* last */
					break;
				case 'b':
					/* bet */
					bet(socket, players[index], buf, data->horses, data->horse_count, data->bank, data->bank_mutex);
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
		} else {
			printf("NOT Reading...\n");
			while(*data->state != STATE_NOT_RACING) {
				pthread_mutex_lock(data->mutex);
				if(pthread_cond_wait(data->cond, data->mutex) != 0) {
					ERR("pthread_cond_wait");
				}
				if(pthread_mutex_unlock(data->mutex) != 0) {
					ERR("pthread_mutex_unlock");
				}
				for(i = 0; i < MAX_HORSES_PER_RACE; ++i) {
					if((*data->curr_running_horses)[i]) {
						snprintf(one_line_status, 256, "%s distance: %d\n", (*data->curr_running_horses)[i]->name, (*data->curr_running_horses)[i]->distance_run);
	/*					snprintf(one_line_status, 256, "%s distance: %d\n", data->horses[i].name, data->horses[i].distance_run); */
						strcat(race_status, one_line_status);
					}
				}
				strcat(race_status, "\n");
				if(bulk_write(socket, race_status, 256*8) < 0) {
					ERR("write");
				}
				race_status[0]='\0';
			}
			if(players[index]->horse_bet && data->winner && !strcmp(players[index]->horse_bet->name, (*data->winner)->name)) {
				players[index]->money += players[index]->money_bet * 2;
				printf("SSSS\n");
			}		
		}/* if(FD_ISSET) */
	} /* while */

	fprintf(stderr, "Connection ended.\n");
	free(th_data);
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

void* horse_thread(void* arg) {
	horse_args* args = (horse_args*) arg;
	horse* data = args->horse_data;
	float distance;
	fprintf(stderr, "Horse: %s, running: %d\n", data->name, data->running);
	
	while(!exit_flag) {
		if(pthread_mutex_lock(data->mutex) != 0) {
			ERR("pthread_mutex_lock");
		}
		while(!data->running) {
			if(pthread_cond_wait(data->cond, data->mutex) != 0) {
				ERR("pthread_cond_wait");
			}
			fprintf(stderr, "Horse: %s awaken!\n", data->name);
		}
		if(pthread_mutex_unlock(data->mutex) != 0) {
			ERR("pthread_mutex_unlock");
		}
		
		while(data->running) {
			distance = data->rest_factor * MAX_HORSE_SPEED + (rand() % 5);
			data->distance_run += distance;
			data->rest_factor -= distance * 0.001;

			if(data->distance_run >= RACE_DISTANCE) {
				data->running = 0;
				if(pthread_mutex_lock(data->mutex) != 0) {
					ERR("pthread_mutex_lock");
				}
				if(!(*args->winner)) {
					*args->winner = data;
					fprintf(stderr, "Horse: %s ended\n", data->name);
				}
				if(pthread_mutex_unlock(data->mutex) != 0) {
					ERR("pthread_mutex_unlock");
				}
				break;
			}

			fprintf(stderr, "Horse: %s\tDistance run: %d, Rest Factor: %f\n", data->name, data->distance_run, data->rest_factor);


			if(pthread_mutex_lock(data->mutex) != 0) {
				ERR("pthread_mutex_lock");
			}
			if(pthread_cond_wait(data->cond, data->mutex) != 0) {
				ERR("pthread_cond_wait");
			}
			if(pthread_mutex_unlock(data->mutex) != 0) {
				ERR("pthread_mutex_unlock");
			}
		}
	}
	pthread_exit(NULL);
}

void* server_accept_connections(void* arg) {
	acc_clients_args* args = (acc_clients_args*) arg;
	int sock, i=0, socket = args->socket/*, horse_count = args->horse_count*/;
	/*horse* horses = args->horses;*/
	pthread_t id[MAX_HORSES_PER_RACE];
	pthread_attr_t thattr;
	player** players = args->players;
	player_th_data* thread_data;

	if(pthread_attr_init(&thattr) != 0) {
		ERR("pthread_attr_init");
	}
	if(pthread_attr_setdetachstate(&thattr, PTHREAD_CREATE_DETACHED) != 0) {
		ERR("pthread_attr_setdetachstate");
	}

	while(!exit_flag) {
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
		thread_data->state_mutex = args->state_mutex;
		thread_data->state_cond = args->state_cond;
		thread_data->cond = args->cond;
		thread_data->mutex = args->mutex;
		thread_data->bank_mutex = args->bank_mutex;
		thread_data->state = args->state;
		thread_data->horse_count = args->horse_count;
		thread_data->horses = args->horses;
		thread_data->curr_running_horses = args->curr_running_horses;
		thread_data->winner = args->winner;
		thread_data->players = args->players;
		thread_data->bank = args->bank;
		if(pthread_create(&id[i++], &thattr, handle_connection, (void*) thread_data) != 0) {
			ERR("pthread_create");
		}
	}

	if(pthread_attr_destroy(&thattr) != 0) {
		ERR("pthread_attr_destroy");
	}
	
	pthread_exit(NULL);
}

void read_configuration(horse** horses, horse** race_winner, int* horse_count, int* frequency, pthread_mutex_t* race_mutex, pthread_cond_t* race_cond) {
	FILE* file;
	char buf[LINE_BUF];
	int i;
	char* b;
	pthread_attr_t thattr;
	horse_args* hargs;

	if(pthread_attr_init(&thattr) != 0) {
		ERR("pthread_attr_init");
	}
	if(pthread_attr_setdetachstate(&thattr, PTHREAD_CREATE_DETACHED) != 0) {
		ERR("pthread_attr_setdetachstate");
	}
	
	memset(buf, 0, LINE_BUF);

	if( (file = fopen(SERVER_CONF_FILE, "r")) == NULL) {
		ERR("fopen");
	}

	/* Read frequency */
	if(fgets(buf, LINE_BUF, file) == NULL) {
		goto closed;
	}

	*frequency = atoi(buf + strlen("FREQUENCY:"));
	*frequency = 3600 / *frequency;

	/* Read horse count */
	if(fgets(buf, LINE_BUF, file) == NULL) {
		goto closed;
	}

	*horse_count = atoi(buf + strlen("HORSE_COUNT:"));

	if( (*horses = (horse*) calloc(*horse_count, sizeof(horse))) == NULL) {
		fclose(file);
		ERR("calloc");
	}

	if( (hargs = (horse_args*) calloc(*horse_count, sizeof(horse_args))) == NULL) {
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
		(*horses)[i].distance_run = 0;
		(*horses)[i].rest_factor = 1;
		(*horses)[i].mutex = race_mutex;
		(*horses)[i].cond = race_cond;

		hargs[i].winner = race_winner;
		hargs[i].horse_data = &(*horses)[i];
	
		if(pthread_create(&(*horses)[i].tid, &thattr, horse_thread, (void*) &hargs[i]) != 0) {
			ERR("pthread_create");
		}
	}

closed:
	if(fclose(file) == EOF) {
		ERR("fclose");
	}

	if(pthread_attr_destroy(&thattr) != 0) {
		ERR("pthread_attr_destroy");
	}
}

void* server_handle_race(void* arg) {
	race_args* args = (race_args*) arg;
	int horse_count = args->horse_count, i, racing_horses, count = 0, index, in_running_index=0, winners = 0, total_win_bet = 0;
	double percent;
	unsigned int seed = time(NULL);
	horse* horses = args->horses;
	int* bank = args->bank;
	player** players = args->players;
	/*pthread_mutex_t* mutex = args->mutex;(*/
	pthread_cond_t* cond = args->cond;
	
	while(!exit_flag) {

		if(pthread_mutex_lock(args->state_mutex) != 0) {
			ERR("pthread_mutex_lock");
		}
		while(*args->state != STATE_RACING) {
			printf("State: NOT_RACING\n");
			if(pthread_cond_wait(args->state_cond, args->state_mutex) != 0) {
				ERR("pthread_cond_wait");
			}
		}
		printf("State: RACING\n");
		if(pthread_mutex_unlock(args->state_mutex) != 0) {
			ERR("pthread_mutex_unlock");
		}
		printf("Here1\n");
		
		racing_horses = (MAX_HORSES_PER_RACE > horse_count) ? horse_count : MAX_HORSES_PER_RACE;

		count = 0;
		in_running_index = 0;
		for(i = 0; i < racing_horses; ++i) {
			index = rand_r(&seed) % racing_horses;
			if(horses[index].running == 0) {
				horses[index].running = 1;
				(*args->curr_running_horses)[in_running_index++] = &horses[index];
				++count;
			}
		}

		printf("here2\n");
		while(!(*(args->winner)) && !exit_flag) {
			sleep(1);
			printf("here3\n");
			if(pthread_cond_broadcast(cond) != 0) {
				ERR("pthread_cond_broadcast");
			}
		}

		for(i = 0; i < horse_count; ++i) {
			horses[index].running = 0;
			horses[index].distance_run = 0;
		}

		total_win_bet = 0;
		for(i = 0; i < MAX_PLAYERS; ++i) {
			if(players[i] && players[i]->horse_bet && !strcmp(players[i]->horse_bet->name, (*(args->winner))->name)) {
				total_win_bet += players[i]->money_bet;
			}
		}
	
		for(i = 0; i < MAX_PLAYERS; ++i) {
			if(players[i] && players[i]->horse_bet && !strcmp(players[i]->horse_bet->name, (*(args->winner))->name)) {
				percent = ((double) players[i]->money_bet / (double) total_win_bet);
				players[i]->money += percent * (*bank);

				players[i]->horse_bet = NULL;
				players[i]->money_bet = 0;
			}
		}
		*bank = 0;

		for(i = 0; i < MAX_HORSES_PER_RACE; ++i) {
			(*args->curr_running_horses)[i] = NULL;
		}

		if(pthread_cond_broadcast(cond) != 0) {
			ERR("pthread_cond_broadcast");
		}
		
		*args->state = STATE_NOT_RACING;
		pthread_cond_broadcast(args->state_cond);

		fprintf(stderr, "The Race has ended!\n");
		(*(args->winner)) = NULL;
	}
	pthread_exit(NULL);
}

int main(int argc, char** argv) {
	int socket, horse_count, frequency, state_value = STATE_NOT_RACING, i, bank = 0;
	uint16_t port;
	horse* horses;
	horse* race_winner;
	horse** curr_running;
	player** players;
	pthread_t tid[2];
	pthread_mutex_t race_mutex, state_mutex, bank_mutex;
	pthread_cond_t race_cond, state_cond;
	acc_clients_args arguments1;
	race_args race_arg;
	
	if(argc != 2) {
		usage();
		exit(EXIT_FAILURE);
	}

	if(sethandler(SIG_IGN, SIGPIPE) < 0) {
		ERR("sigaction");
	}

	if( (curr_running = (horse**) calloc(MAX_HORSES_PER_RACE, sizeof(horse*))) == NULL ) {
		ERR("calloc");
	}
	if( (players = (player**) calloc(MAX_PLAYERS, sizeof(player*))) == NULL ) {
		ERR("calloc");
	}
	for(i = 0; i < MAX_HORSES_PER_RACE; ++i) {
		curr_running[i] = NULL;
	}

	if(pthread_mutex_init(&race_mutex, NULL) != 0) {
		ERR("pthread_mutex_init");
	}
	if(pthread_mutex_init(&state_mutex, NULL) != 0) {
		ERR("pthread_mutex_init");
	}
	if(pthread_mutex_init(&bank_mutex, NULL) != 0) {
		ERR("pthread_mutex_init");
	}
	if(pthread_cond_init(&race_cond, NULL) != 0) {
		ERR("pthread_cond_init");
	}
	if(pthread_cond_init(&state_cond, NULL) != 0) {
		ERR("pthread_cond_init");
	}

	port = atoi(argv[1]);

	race_winner = NULL;

	read_configuration(&horses, &race_winner, &horse_count, &frequency, &race_mutex, &race_cond);

	socket = make_socket(port);
	
	arguments1.socket = socket;
	arguments1.horses = horses;
	arguments1.horse_count = horse_count;
	arguments1.state_mutex = &state_mutex;
	arguments1.state_cond = &state_cond;
	arguments1.state = &state_value;
	arguments1.cond = &race_cond;
	arguments1.mutex = &race_mutex;
	arguments1.bank_mutex = &bank_mutex;
	arguments1.curr_running_horses = &curr_running;
	arguments1.winner = &race_winner;
	arguments1.players = players;
	arguments1.bank = &bank;
	if(pthread_create(&tid[0], NULL, server_accept_connections, (void*) &arguments1) != 0) {
		ERR("pthread_create");
	}

	race_arg.horses = horses;
	race_arg.winner = &race_winner;
	race_arg.horse_count = horse_count;
	race_arg.state_mutex = &state_mutex;
	race_arg.state_cond = &state_cond;
	race_arg.mutex = &race_mutex;
	race_arg.bank_mutex = &bank_mutex;
	race_arg.cond = &race_cond;
	race_arg.state = &state_value;
	race_arg.curr_running_horses = &curr_running;
	race_arg.players = players;
	race_arg.bank = &bank;
	if( pthread_create(&tid[1], NULL, server_handle_race, (void*) &race_arg) != 0) {
		ERR("pthread_create");
	}

	while(!exit_flag) {
		printf("Frequency: %d.\n", frequency);
		printf("Sleeping...\n");
		sleep(frequency);
		printf("Woke up!\n");
		state_value = STATE_RACING;
		pthread_cond_broadcast(&state_cond);
		pthread_mutex_lock(&state_mutex);
		while(state_value != STATE_NOT_RACING) {
			pthread_cond_wait(&state_cond, &state_mutex);
		}
		pthread_mutex_unlock(&state_mutex);
	} 
	
	if(pthread_join(tid[0], NULL) != 0) {
		ERR("pthread_join");
	}
	
	if(pthread_join(tid[1], NULL) != 0) {
		ERR("pthread_join");
	}

	if(TEMP_FAILURE_RETRY(close(socket)) < 0) {
		ERR("close");
	}

	if(pthread_mutex_destroy(&race_mutex) != 0) {
		ERR("pthread_mutex_destroy");
	}
	if(pthread_mutex_destroy(&state_mutex) != 0) {
		ERR("pthread_mutex_destroy");
	}
	if(pthread_mutex_destroy(&bank_mutex) != 0) {
		ERR("pthread_mutex_destroy");
	}
	if(pthread_cond_destroy(&race_cond) != 0) {
		ERR("pthread_cond_destroy");
	}
	if(pthread_cond_destroy(&state_cond) != 0) {
		ERR("pthread_cond_destroy");
	}
	
	for(i = 0; i < MAX_PLAYERS; ++i) {
		free(players[i]);
	}
	free(players);

	return EXIT_SUCCESS;
}
