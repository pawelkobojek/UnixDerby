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
#define MAX_PLAYERS 128

#define STATE_NOT_RACING 101
#define STATE_RACING 102

#define SERVER_CONF_FILE "conf"

#define ENTER_LOGIN_MSG "[SERVER MESSAGE] Enter login please:\n"
#define CANT_WITHDRAW_MSG "[SERVER MESSAGE] Cannot withdraw that amount!\n"
#define UNWN_CMD_MSG "[SERVER MESSAGE] Unkown command!\n"
#define NO_SUCH_HORSE_MSG "[SERVER MESSAGE] There's no such horse!\n"
#define CANT_BET_MSG "[SERVER MESSAGE] Not enough money to bet!\n"
#define CANT_BET_NEGATIVE_MSG "[SERVER MESSAGE] Your bet must be more than zero!\n"
#define CANT_DEP_NEGATIVE_MSG "[SERVER MESSAGE] Cannot deposit negative amount!\n"

#define ERR(source) (perror(source),\
		fprintf(stderr,"%s:%d\n",__FILE__,__LINE__),\
		exit(EXIT_FAILURE))

volatile sig_atomic_t exit_flag = 0;

typedef struct {
	pthread_t tid;			/* Horse thread's id */
	pthread_mutex_t* mutex;		/* Pointer to mutex uesd to simulate race turns */
	pthread_cond_t* cond;		/* Pointer to conditional variable used to simulate race turns */
	pthread_barrier_t* barrier;	/* Barrier used to ensure that every horse ends his turn before next */
	char name[MAX_NAME_LEN];	/* Name of the horse */
	short running;			/* Tells wheter horse is running (==1 if so)  or not (==0) */
	unsigned int distance_run;	/* Distance run by horse in current race */
	float rest_factor;		/* Horse rest factor */
} horse;

typedef struct {
	char name[MAX_NAME_LEN];	/* Player's name */
	int money;			/* Player's deposited money */
	horse* horse_bet;		/* Pointer to betted horse */
	int money_bet;			/* Money betted on horse */
	int* bank;			/* Pointer to bank */
} player;

typedef struct {
	horse* horse_data;		/* Horse owned by thread */
	horse** winner;			/* Pointer to winner of the race */
} horse_args;

typedef struct {
	player** players;		/* Array of all players */
	int socket;			/* Socket of player's connection */
	int* state;			/* Indicates state of the server (either accepting bets or handling the race */
	int* bank;			/* Pointer to bank */
	int horse_count;		/* Number of all horses */
	time_t* time;			/* Time of interval between races start */
	int* interval;			/* Interval of time between races */
	horse* horses;			/* Array of all horses */
	pthread_mutex_t* state_mutex;	/* Mutex used in state changes */
	pthread_mutex_t* mutex;		/* Pointer to mutex used to simulate race turns */
	pthread_mutex_t* bank_mutex;	/* Mutex for bank access */
	pthread_cond_t* state_cond;	/* Conditional variable used to signal state changes */
	pthread_cond_t* cond;		/* Conditional variable used to signal race turns */
	horse*** curr_running_horses;	/* Pointer to array of horses running in current/upcoming race */
	horse** winner;			/* Pointer to winner of the race */
} player_th_data;

typedef struct {
	int socket;			/* Socket used to accept new connections */
	horse* horses;			/* Array of all horses */
	int horse_count;		/* Number of horses */
	int* state;			/* Indicates state of the server (either accepting bets or handling the race */
	int* bank;			/* Pointer to the bank */
	time_t* time;			/* Time of interval between races start */
	int* interval;			/* Interval of time between races */
	player** players;		/* Array of all players */
	horse** winner;			/* Pointer to winner of the race */
	pthread_cond_t* state_cond;	/* Conditional variable used to signal state changes */
	pthread_cond_t* cond;		/* Conditional variable used to signal race turns */
	pthread_mutex_t* state_mutex;	/* Mutex used in state changes */
	pthread_mutex_t* mutex;		/* Pointer to mutex used to simulate race turns */
	pthread_mutex_t* bank_mutex;	/* Mutex for bank access */
	horse*** curr_running_horses;	/* Pointer to array of horses running in current/upcoming race */
} acc_clients_args;

typedef struct {
	horse* horses;			/* Array of all horses */
	horse** winner;			/* Pointer to winner of the race */
	player** players;		/* Array of all players */
	int horse_count;		/* Number of horses */
	int* state;			/* Indicates state of the server (either accepting bets or handling the race */
	int* bank;			/* Pointer to the bank */
	pthread_mutex_t* state_mutex;	/* Mutex used in state changes */
	pthread_cond_t* state_cond;	/* Conditional variable used to signal state changes */
	pthread_mutex_t* bank_mutex;	/* Mutex for bank access */
	pthread_mutex_t* mutex;		/* Mutex used in state changes */
	pthread_cond_t* cond;		/* Conditional variable used to signal race turns */
	pthread_barrier_t* barrier;	/* Barrier used to ensure that every horse ends his turn before next */
	horse*** curr_running_horses;	/* Pointer to array of horses running in current/upcoming race */
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

/*
* Sets action for signal inside the thread.
*
* @action: action to be performed on given signal
* @signal: signal to be blocked/ublocked
*/
void single_pthread_sigmask(int action, int signal) {
	sigset_t sigmask;
	sigemptyset(&sigmask);
	sigaddset(&sigmask, SIGUSR1);
	
	pthread_sigmask(SIG_UNBLOCK, &sigmask, NULL);
}	

int sethandler(void (*f)(int), int sigNo) {
	struct sigaction act;
	memset(&act, 0, sizeof(struct sigaction));
	act.sa_handler = f;
	
	return sigaction(sigNo, &act, NULL);
}

void sigint_handler(int sigNo) {
	exit_flag = 1;
}

/*
* Registers player in the system.
* LF and CR characters are chopped from name.
*
* @players: array of all players
* @pl_len:  players' array length
* @name:    name of player being registered
* @len:	    name length
*
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

	fprintf(stderr, "Player %s registered.\n", buf);
	
	return empty_slot;
}

void deposit(int socket, player* pl, int deposit) {
	if(deposit < 0) {
		if(bulk_write(socket, CANT_DEP_NEGATIVE_MSG, strlen(CANT_DEP_NEGATIVE_MSG)) < 0 && errno != EPIPE) {
			ERR("write");
		}
	}
	pl->money += deposit;
}

void withdraw(int socket, player* pl, int amount) {
	if(pl->money - amount < 0) {
		if(bulk_write(socket, CANT_WITHDRAW_MSG, strlen(CANT_WITHDRAW_MSG)) < 0 && errno != EPIPE) {
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
			if(money_bet <= 0) {
				if(bulk_write(socket, CANT_BET_NEGATIVE_MSG, strlen(CANT_BET_NEGATIVE_MSG)) < 0 && errno != EPIPE) {
					ERR("write");
				}
			}
			if(money_bet > pl->money) {
				if(bulk_write(socket, CANT_BET_MSG, strlen(CANT_BET_MSG)) < 0 && errno != EPIPE) {
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

	if(bulk_write(socket, NO_SUCH_HORSE_MSG, strlen(NO_SUCH_HORSE_MSG)) < 0 && errno != EPIPE) {
		ERR("write");
	}
}

/*
* Sends player info to the client.
*
* @socket: socket for the client's connection
* @pl:     pointer to the player
*/
void print_info(int socket, player* pl) {
	char send_info[LINE_BUF];
	snprintf(send_info, LINE_BUF, "Player: %s, money: %d, Bet on horse: %s with %d money\n", pl->name, pl->money, (pl->horse_bet) ? pl->horse_bet->name : "none", pl->money_bet);
	if(bulk_write(socket, send_info, strlen(send_info)) < 0 && errno != EPIPE) {
		ERR("write");
	}
}

void next_race_time(int socket, player* pl, time_t* start_time, int* interval, horse*** curr_running_horses) {
	char send_info[LINE_BUF];
	char next_race_info[LINE_BUF * (MAX_HORSES_PER_RACE + 1)];
	int i;

	memset(send_info, 0, LINE_BUF);
	memset(next_race_info, 0, LINE_BUF * (MAX_HORSES_PER_RACE + 1));
	snprintf(send_info, LINE_BUF, "Next race in %d seconds...\nHorses running in the next race:\n", *interval - (int) (time(NULL) - (*start_time)));
	strcat(next_race_info, send_info);
	for(i = 0; i < MAX_HORSES_PER_RACE; ++i) {
		if((*curr_running_horses)[i]) {
			snprintf(send_info, LINE_BUF, "\t%s\n", (*curr_running_horses)[i]->name);
			strcat(next_race_info, send_info);
		}
	}
	strcat(next_race_info, "\n");
	if(bulk_write(socket, next_race_info, LINE_BUF * (MAX_HORSES_PER_RACE + 1)) < 0 && errno != EPIPE) {
		ERR("write");
	}
}

void last_race_info(int socket, player* pl, horse* winner) {
	char send_info[LINE_BUF];
	if(winner == NULL) {
		return;
	}

	snprintf(send_info, LINE_BUF, "Last race winner: %s\n", winner->name);
	if(bulk_write(socket, send_info, strlen(send_info)) < 0 && errno != EPIPE) {
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

void notify_race_info(player_th_data* data, int socket) {
	int i;
	char race_status[LINE_BUF * MAX_HORSES_PER_RACE];
	char one_line_status[LINE_BUF];
	memset(race_status, 0, LINE_BUF * MAX_HORSES_PER_RACE);
	memset(one_line_status, 0, LINE_BUF);
	while(*data->state != STATE_NOT_RACING && !exit_flag) {
		pthread_mutex_lock(data->mutex);
		if(pthread_cond_wait(data->cond, data->mutex) != 0) {
			ERR("pthread_mutex_unlock");
		}
		if(pthread_mutex_unlock(data->mutex) != 0) {
			ERR("pthread_mutex_unlock");
		}
		for(i = 0; i < MAX_HORSES_PER_RACE; ++i) {
			if(!(*data->winner) && (*data->curr_running_horses)[i]) {
				snprintf(one_line_status, LINE_BUF, "%s dinstance: %d\n", (*data->curr_running_horses)[i]->name, (*data->curr_running_horses)[i]->distance_run);
				strcat(race_status, one_line_status);
			}
		}
		strcat(race_status, "\n");
		if(bulk_write(socket, race_status, LINE_BUF * MAX_HORSES_PER_RACE) < 0 && errno != EPIPE) {
			ERR("write");
		}
		race_status[0]='\0';
	}
}

void route_cmd(int socket, int index, player_th_data* data, player** players) {
	int count;
	char buf[BUF_SIZE];

	count = read(socket, buf, BUF_SIZE);
		
	if(count < 0) {
		ERR("read");
	}

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
			break;
		case 'n':
			/* next */
			next_race_time(socket, players[index], data->time, data->interval, data->curr_running_horses);
			break;
		case 'l':
			/* last */
			last_race_info(socket, players[index], *data->winner);
			break;
		case 'b':
			/* bet */
			bet(socket, players[index], buf, data->horses, data->horse_count, data->bank, data->bank_mutex);
			break;
		default:
			if(bulk_write(socket, UNWN_CMD_MSG, strlen(UNWN_CMD_MSG)) < 0 && errno != EPIPE) {
				ERR("write");
			}
			break;
	}		
	memset(buf, 0, BUF_SIZE);
}

/**
* Handles one connection (always runs on separate thread)
* @th_data: thread argument, see: @player_th_data structure.
*/
void* handle_connection(void* th_data) {
	player_th_data* data = (player_th_data*) th_data;
	int socket = data->socket, count, index, result;
	char buf[BUF_SIZE];
	player** players = data->players;
	fd_set read_set;
	struct timeval tv, ttv = {0, 500000};

	memset(buf, 0, BUF_SIZE);

	if(bulk_write(socket, ENTER_LOGIN_MSG, strlen(ENTER_LOGIN_MSG)) < 0 && errno != EPIPE) {
		ERR("write");
	}

	if( (count = read(socket, buf, BUF_SIZE)) < 0) {
		ERR("read");
	}
	
	if(count == 0) {
		fprintf(stderr, "Connection closed.\n");
		pthread_exit(NULL);
	}

	index = register_player(players, MAX_PLAYERS,  buf, strlen(buf));
	
	notify_race_info(data, socket);

	FD_ZERO(&read_set);
	FD_SET(socket, &read_set);
	count = 1;
	tv = ttv;
	while(( (result = select(socket + 1, &read_set, NULL, NULL, &tv) || count)) && !exit_flag) {
		if(result < 0 && errno != EINTR) {
			ERR("select");
		}
		tv = ttv;
		if(FD_ISSET(socket, &read_set)) {
			route_cmd(socket, index, data, players);
		} else {
			notify_race_info(data, socket);
		}
		FD_ZERO(&read_set);
		FD_SET(socket, &read_set);
	}

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

void run_race(horse* data, horse_args* args) {
	float distance;
	while(data->running && !(*args->winner) && !exit_flag) {
		distance = data->rest_factor * MAX_HORSE_SPEED + (rand() % 5);
		data->distance_run += distance;
		data->rest_factor -= distance * 0.001;

		if(data->running && !(*args->winner)) {
			fprintf(stderr, "Horse: %s\tDistance run: %d, Rest Factor: %f\n", data->name, data->distance_run, data->rest_factor);
		}
		
		if(data->distance_run >= RACE_DISTANCE) {
			data->running = 0;
			if(pthread_mutex_lock(data->mutex) != 0) {
				ERR("pthread_mutex_lock");
			}
			if(!(*args->winner)) {
				*args->winner = data;
				fprintf(stderr, "Horse: %s won!\n", data->name);
			}
			
			fprintf(stderr, "Horse: %s waiting on cond...\n", data->name);
			if(pthread_cond_wait(data->cond, data->mutex) != 0) {
				ERR("pthread_cond_wait");
			}
			if(pthread_mutex_unlock(data->mutex) != 0) {
				ERR("pthread_mutex_unlock");
			}
		
			fprintf(stderr, "Horse: %s waiting on barrier...\n", data->name);
			pthread_barrier_wait(data->barrier);

			data->distance_run = 0;
			break;
		}
		if(pthread_mutex_lock(data->mutex) != 0) {
			ERR("pthread_mutex_lock");
		}
			fprintf(stderr, "Horse: %s waiting on cond...\n", data->name);
		if(pthread_cond_wait(data->cond, data->mutex) != 0) {
			ERR("pthread_cond_wait");
		}
		if(pthread_mutex_unlock(data->mutex) != 0) {
			ERR("pthread_mutex_unlock");
		}
		
		fprintf(stderr, "Horse: %s waiting on barrier...\n", data->name);
		pthread_barrier_wait(data->barrier);
	}
}

void* horse_thread(void* arg) {
	horse_args* args = (horse_args*) arg;
	horse* data = args->horse_data;
	time_t start, end, dif;
	
	while(!exit_flag) {
		start = time(NULL);
		if(pthread_mutex_lock(data->mutex) != 0) {
			ERR("pthread_mutex_lock");
		}
		while(!data->running && !exit_flag) {
			if(pthread_cond_wait(data->cond, data->mutex) != 0) {
				ERR("pthread_cond_wait");
			}
			fprintf(stderr, "horse: %s awaken!, running: %d\n", data->name, data->running);
		}
		if(pthread_mutex_unlock(data->mutex) != 0) {
			ERR("pthread_mutex_unlock");
		}
		
		end = time(NULL);
		
		dif = end - start;

		data->rest_factor += dif * 0.05;
		data->rest_factor = (data->rest_factor >= 1) ? 1 : data->rest_factor;
	
		run_race(data, args);
	}
	pthread_exit(NULL);
}

void* server_accept_connections(void* arg) {
	acc_clients_args* args = (acc_clients_args*) arg;
	int sock, i=0, socket = args->socket;
	pthread_t id[MAX_HORSES_PER_RACE];
	pthread_attr_t thattr;
	player** players = args->players;
	player_th_data* thread_data;

	single_pthread_sigmask(SIG_UNBLOCK, SIGUSR1);
	
	if(pthread_attr_init(&thattr) != 0) {
		ERR("pthread_attr_init");
	}
	if(pthread_attr_setdetachstate(&thattr, PTHREAD_CREATE_DETACHED) != 0) {
		ERR("pthread_attr_setdetachstate");
	}

	while(!exit_flag) {
		fprintf(stderr, "Waiting for connection...\n");
		if( (sock = accept(socket, NULL, NULL)) < 0) {
			if(errno == EINTR && exit_flag) break;
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
		thread_data->time = args->time;
		thread_data->interval = args->interval;
		if(pthread_create(&id[i++], &thattr, handle_connection, (void*) thread_data) != 0) {
			ERR("pthread_create");
		}
	}

	if(pthread_attr_destroy(&thattr) != 0) {
		ERR("pthread_attr_destroy");
	}

	pthread_exit(NULL);
}

void read_configuration(horse** horses, horse** race_winner, int* horse_count, int* frequency, pthread_mutex_t* race_mutex, pthread_cond_t* race_cond, pthread_barrier_t* race_barrier, horse_args** hargs) {
	FILE* file;
	char buf[LINE_BUF];
	int i;
	char* b;
	pthread_attr_t thattr;

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

	if( (*hargs = (horse_args*) calloc(*horse_count, sizeof(horse_args))) == NULL) {
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
		(*horses)[i].barrier = race_barrier;

		(*hargs)[i].winner = race_winner;
		(*hargs)[i].horse_data = &(*horses)[i];
	
		if(pthread_create(&(*horses)[i].tid, &thattr, horse_thread, (void*) &((*hargs)[i])) != 0) {
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

int init_race(horse* horses, race_args* args) {
	int count = 0, in_running_index = 0, racing_horses = (MAX_HORSES_PER_RACE > args->horse_count) ? args->horse_count : MAX_HORSES_PER_RACE, i, index;
	unsigned int seed = time(NULL);

	for(i = 0; i < racing_horses; ++i) {
		index = rand_r(&seed) % racing_horses;
		if(horses[index].running == 0) {
			horses[index].running = 1;
			(*args->curr_running_horses)[in_running_index++] = &horses[index];
			++count;
		}
	}
	pthread_barrier_init(args->barrier, NULL, count);

	for(i = 0; i < MAX_HORSES_PER_RACE; ++i) {
		if( (*args->curr_running_horses)[i]) {
			printf("%s \t", (*args->curr_running_horses)[i]->name);
		}
	}

	return count;
}

void wait_for_race(int* state, pthread_mutex_t* state_mutex, pthread_cond_t* state_cond) {

	if(pthread_mutex_lock(state_mutex) != 0) {
		ERR("pthread_mutex_lock");
	}
	while(*state != STATE_RACING && !exit_flag) {
		printf("State: NOT_RACING\n");
		if(pthread_cond_wait(state_cond, state_mutex) != 0) {
			ERR("pthread_cond_wait");
		}
	}
	printf("State: RACING\n");
	if(pthread_mutex_unlock(state_mutex) != 0) {
		ERR("pthread_mutex_unlock");
	}
}

void manage_prizes(int* bank, player** players, horse** winner) {
	int total_win_bet = 0, i;
	double percent;
	for(i = 0; i < MAX_PLAYERS; ++i) {
		if(players[i] && players[i]->horse_bet && !strcmp(players[i]->horse_bet->name, (*winner)->name)) {
			total_win_bet += players[i]->money_bet;
		}
	}

	for(i = 0; i < MAX_PLAYERS; ++i) {
		if(players[i] && players[i]->horse_bet) {
			if(!strcmp(players[i]->horse_bet->name, (*winner)->name)) {
				percent = ((double) players[i]->money_bet / (double) total_win_bet);
				players[i]->money += percent * (*bank);

				players[i]->horse_bet = NULL;
				players[i]->money_bet = 0;
			} else {
				players[i]->horse_bet = NULL;
				players[i]->money_bet = 0;
			}
		}
	}
	if(total_win_bet != 0) {
		*bank = 0;
	}
}

void* server_handle_race(void* arg) {
	race_args* args = (race_args*) arg;
	int horse_count = args->horse_count, i;
	horse* horses = args->horses;
	int* bank = args->bank;
	player** players = args->players;
	
	single_pthread_sigmask(SIG_UNBLOCK, SIGUSR1);

	while(!exit_flag) {
		init_race(horses, args);
		wait_for_race(args->state, args->state_mutex, args->state_cond);
		(*(args->winner)) = NULL;
		if(pthread_cond_broadcast(args->cond) != 0) {
			ERR("pthread_cond_broadcast");
		}
		
		while(!(*(args->winner)) && !exit_flag) {
			sleep(1);
			if(pthread_cond_broadcast(args->cond) != 0) {
				ERR("pthread_cond_broadcast");
			}
			printf("\n");
		}

		for(i = 0; i < horse_count; ++i) {
			horses[i].running = 0;
			horses[i].distance_run = 0;
		}
		
		fprintf(stderr, "cond broadcast after end\n");
		if(pthread_cond_broadcast(args->cond) != 0) {
			ERR("pthread_cond_broadcast");
		}
		
		manage_prizes(bank, players, args->winner);
		for(i = 0; i < MAX_HORSES_PER_RACE; ++i) {
			(*args->curr_running_horses)[i] = NULL;
		}
		if(pthread_cond_broadcast(args->cond) != 0) {
			ERR("pthread_cond_broadcast");
		}
		
		*args->state = STATE_NOT_RACING;
		pthread_cond_broadcast(args->state_cond);

		pthread_barrier_destroy(args->barrier);

	}
	pthread_exit(NULL);
}

void initialize_syncs(pthread_mutex_t* race_mutex, pthread_mutex_t* state_mutex, pthread_mutex_t* bank_mutex, pthread_cond_t* race_cond, pthread_cond_t* state_cond) {

	if(pthread_mutex_init(race_mutex, NULL) != 0) {
		ERR("pthread_mutex_init");
	}
	if(pthread_mutex_init(state_mutex, NULL) != 0) {
		ERR("pthread_mutex_init");
	}
	if(pthread_mutex_init(bank_mutex, NULL) != 0) {
		ERR("pthread_mutex_init");
	}
	if(pthread_cond_init(race_cond, NULL) != 0) {
		ERR("pthread_cond_init");
	}
	if(pthread_cond_init(state_cond, NULL) != 0) {
		ERR("pthread_cond_init");
	}
}

void destroy_syncs(pthread_mutex_t* race_mutex, pthread_mutex_t* state_mutex, pthread_mutex_t* bank_mutex, pthread_cond_t* race_cond, pthread_cond_t* state_cond) {
	if(pthread_mutex_destroy(race_mutex) != 0) {
		ERR("pthread_mutex_destroy");
	}
	if(pthread_mutex_destroy(state_mutex) != 0) {
		ERR("pthread_mutex_destroy");
	}
	if(pthread_mutex_destroy(bank_mutex) != 0) {
		ERR("pthread_mutex_destroy");
	}
	if(pthread_cond_destroy(race_cond) != 0) {
		ERR("pthread_cond_destroy");
	}
	if(pthread_cond_destroy(state_cond) != 0) {
		ERR("pthread_cond_destroy");
	}
}

void manage_state(int frequency, time_t* count_start, int* state_value, pthread_cond_t* state_cond, pthread_mutex_t* state_mutex) {

	while(!exit_flag) {
		fprintf(stdout, "Next race in %d seconds...\n", frequency);
		*count_start = time(NULL);
		sleep(frequency);
		printf("Woke up!\n");
		*state_value = STATE_RACING;
		pthread_cond_broadcast(state_cond);
		pthread_mutex_lock(state_mutex);
		while(*state_value != STATE_NOT_RACING && !exit_flag) {
			pthread_cond_wait(state_cond, state_mutex);
		}
		pthread_mutex_unlock(state_mutex);
	} 
	
}

void cleaning(pthread_t* tid, int socket, player** players, horse** curr_running, horse_args* hargs, horse* horses) {
	int i;
	if(pthread_kill(tid[0], SIGUSR1) != 0) {
		ERR("pthread_kill");
	}
	if(pthread_kill(tid[1], SIGUSR1) != 0) {
		ERR("pthread_kill");
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

	for(i = 0; i < MAX_PLAYERS; ++i) {
		free(players[i]);
	}
	free(players);
	free(curr_running);
	free(hargs);
	free(horses);
}

void set_signal_handling(sigset_t* sigmask) {
	if(sethandler(SIG_IGN, SIGPIPE) < 0) {
		ERR("sigaction");
	}
	
	if(sethandler(sigint_handler, SIGUSR1) < 0) {
		ERR("sigaction");
	}
	
	if(sethandler(sigint_handler, SIGINT) != 0) {
		ERR("sethandler");
	}

	sigemptyset(sigmask);
	sigaddset(sigmask, SIGINT);
	sigaddset(sigmask, SIGUSR1);
	pthread_sigmask(SIG_BLOCK, sigmask, NULL);
}

int main(int argc, char** argv) {
	int socket, horse_count, frequency, state_value = STATE_NOT_RACING, i, bank = 0;
	uint16_t port;
	time_t count_start;
	horse* horses;
	horse* race_winner;
	horse** curr_running;
	player** players;
	pthread_t tid[2];
	pthread_mutex_t race_mutex, state_mutex, bank_mutex;
	pthread_cond_t race_cond, state_cond;
	pthread_barrier_t race_barrier;
	acc_clients_args arguments1;
	race_args race_arg;
	sigset_t sigmask;
	horse_args* hargs;
	
	if(argc != 2) {
		usage();
		exit(EXIT_FAILURE);
	}

	set_signal_handling(&sigmask);

	if( (curr_running = (horse**) calloc(MAX_HORSES_PER_RACE, sizeof(horse*))) == NULL ) {
		ERR("calloc");
	}
	if( (players = (player**) calloc(MAX_PLAYERS, sizeof(player*))) == NULL ) {
		ERR("calloc");
	}
	for(i = 0; i < MAX_HORSES_PER_RACE; ++i) {
		curr_running[i] = NULL;
	}
	initialize_syncs(&race_mutex, &state_mutex, &bank_mutex, &race_cond, &state_cond);
	port = atoi(argv[1]);

	race_winner = NULL;

	read_configuration(&horses, &race_winner, &horse_count, &frequency, &race_mutex, &race_cond, &race_barrier, &hargs);

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
	arguments1.time = &count_start;
	arguments1.interval = &frequency;
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
	race_arg.barrier = &race_barrier;
	if( pthread_create(&tid[1], NULL, server_handle_race, (void*) &race_arg) != 0) {
		ERR("pthread_create");
	}

	pthread_sigmask(SIG_UNBLOCK, &sigmask, NULL);


	manage_state(frequency, &count_start, &state_value, &state_cond, &state_mutex);

	cleaning(tid, socket, players, curr_running, hargs, horses); 

	return EXIT_SUCCESS;
}
