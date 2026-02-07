// server.c - Concurrent Networked Word Guessing Game (3 players)
// Architecture:
// - Parent: accept() loop, forks 3 children (1 per client), runs 2 threads:
//   (1) scheduler thread (RR turns for guessers) (2) logger thread (non-blocking queue -> game.log)
// - Shared state: POSIX shared memory, process-shared mutexes + semaphores
// - Communication: TCP IPv4 sockets
//
// Build: gcc -O2 -Wall -Wextra -pedantic -pthread server.c -o server
//
// Notes:
// - This is a skeleton meant to satisfy OS-core requirements first.
// - Game: 5-letter word. Positions 0..4. Each position: guesser1 turn then guesser2 turn.
//   Score +1 if guessed letter matches the secret word at that position.
//   After position 4 completes, game ends; winner is higher score; tie = draw.
//   Server then requests a new word from wordmaster (multi-game without restart).

#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <pthread.h>
#include <semaphore.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define MAX_PLAYERS 3
#define WORD_LEN 5
#define NAME_LEN 32

#define SHM_NAME "/csn6214_wordgame_shm_v1"

#define LOG_MSG_LEN 256
#define LOGQ_CAP 1024

#define OUTQ_CAP 256
#define OUT_MSG_LEN 256

typedef enum {
    PHASE_WAITING_PLAYERS = 0,
    PHASE_WAITING_WORD    = 1,
    PHASE_IN_PROGRESS     = 2,
    PHASE_GAME_OVER       = 3
} game_phase_t;

typedef struct {
    char name[NAME_LEN];
    int wins;
} score_entry_t;

typedef struct {
    // --- Global protection for game state ---
    pthread_mutex_t game_mtx;      // process-shared
    pthread_mutex_t score_mtx;     // process-shared

    // --- Turn control ---
    sem_t turn_sem[MAX_PLAYERS];   // process-shared semaphores (child waits, scheduler posts)

    // --- Logger queue (shared across processes; logger thread drains) ---
    pthread_mutex_t log_mtx;       // process-shared
    sem_t log_items;               // counts queued log messages
    sem_t log_spaces;              // counts free slots

    // --- Game state ---
    game_phase_t phase;

    int connected[MAX_PLAYERS];    // 1 if connected, 0 if disconnected
    int current_turn;              // player id whose turn (1 or 2 for guessers); 0 for wordmaster when prompting word
    int position_idx;              // 0..4
    int guess_count_for_pos;       // 0,1,2 for each position (how many guessers have guessed)
    int score[MAX_PLAYERS];        // score[1], score[2] used
    int pass_num;        // 0..4 (each pass = one full sweep over positions 0..4)

    char secret_word[WORD_LEN + 1];
    char display[WORD_LEN + 1];    // '_' placeholders to show progress

    char player_name[MAX_PLAYERS][NAME_LEN];  // from client NAME message

    // Persistent score table in memory (simple: index by player id 1/2, plus optional names)
    score_entry_t score_table[MAX_PLAYERS];   // score_table[1] and [2] = guessers' lifetime wins

    // Multi-game counter
    int game_number;

    // Shutdown flag set by SIGINT in parent (best-effort)
    int shutting_down;

    // --- Logger ring buffer ---
    int log_head;
    int log_tail;
    char logq[LOGQ_CAP][LOG_MSG_LEN];

    // --- Per-player outgoing broadcast queues ---
    pthread_mutex_t out_mtx[MAX_PLAYERS];   // process-shared
    sem_t out_items[MAX_PLAYERS];           // number of queued messages
    sem_t out_spaces[MAX_PLAYERS];          // free slots
    int out_head[MAX_PLAYERS];
    int out_tail[MAX_PLAYERS];
    char outq[MAX_PLAYERS][OUTQ_CAP][OUT_MSG_LEN];
} shared_t;

// Global pointers in parent process
static int g_listen_fd = -1;
static shared_t *g_sh = NULL;

// ---------- Utility: time string ----------
static void now_str(char *buf, size_t n) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tm;
    localtime_r(&ts.tv_sec, &tm);
    snprintf(buf, n, "%04d-%02d-%02d %02d:%02d:%02d",
             tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
             tm.tm_hour, tm.tm_min, tm.tm_sec);
}

// ---------- Logger queue API (safe across processes) ----------
static void log_enqueuef(const char *fmt, ...) {
    if (!g_sh) return;

    // Try to reserve a slot. If full, we will block briefly (queue is large to avoid this).
    if (sem_wait(&g_sh->log_spaces) == -1) return;

    pthread_mutex_lock(&g_sh->log_mtx);

    int idx = g_sh->log_tail;
    g_sh->log_tail = (g_sh->log_tail + 1) % LOGQ_CAP;

    char ts[64];
    now_str(ts, sizeof(ts));

    char msg[LOG_MSG_LEN];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    snprintf(g_sh->logq[idx], LOG_MSG_LEN, "%s | %s", ts, msg);

    pthread_mutex_unlock(&g_sh->log_mtx);
    sem_post(&g_sh->log_items);
}

// ---------- TCP line-based I/O ----------
static ssize_t send_all(int fd, const void *buf, size_t len) {
    const char *p = (const char*)buf;
    size_t off = 0;
    while (off < len) {
        ssize_t w = send(fd, p + off, len - off, 0);
        if (w < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (w == 0) return -1;
        off += (size_t)w;
    }
    return (ssize_t)off;
}

static int send_line(int fd, const char *line) {
    // sends line plus '\n'
    char buf[512];
    snprintf(buf, sizeof(buf), "%s\n", line);
    return (send_all(fd, buf, strlen(buf)) < 0) ? -1 : 0;
}

static ssize_t recv_line(int fd, char *out, size_t cap) {
    // reads until '\n' or cap-1 bytes
    size_t n = 0;
    while (n + 1 < cap) {
        char c;
        ssize_t r = recv(fd, &c, 1, 0);
        if (r < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (r == 0) return 0; // closed
        if (c == '\n') break;
        if (c == '\r') continue;
        out[n++] = c;
    }
    out[n] = '\0';
    return (ssize_t)n;
}

// ---------- scores.txt persistence ----------
static void scores_load(const char *path) {
    pthread_mutex_lock(&g_sh->score_mtx);

    // initialize defaults
    for (int i = 0; i < MAX_PLAYERS; i++) {
        g_sh->score_table[i].name[0] = '\0';
        g_sh->score_table[i].wins = 0;
    }
    strncpy(g_sh->score_table[1].name, "GuesserA", NAME_LEN - 1);
    strncpy(g_sh->score_table[2].name, "GuesserB", NAME_LEN - 1);

    FILE *f = fopen(path, "r");
    if (!f) {
        // create if missing
        f = fopen(path, "w");
        if (f) fclose(f);
        pthread_mutex_unlock(&g_sh->score_mtx);
        return;
    }

    // File format (simple):
    // player_id wins name
    // e.g.: 1 3 Alice
    //       2 1 Bob
    int pid, wins;
    char name[NAME_LEN];
    while (fscanf(f, "%d %d %31s", &pid, &wins, name) == 3) {
        if (pid >= 0 && pid < MAX_PLAYERS) {
            g_sh->score_table[pid].wins = wins;
            strncpy(g_sh->score_table[pid].name, name, NAME_LEN - 1);
            g_sh->score_table[pid].name[NAME_LEN - 1] = '\0';
        }
    }
    fclose(f);

    pthread_mutex_unlock(&g_sh->score_mtx);
}

static void scores_save(const char *path) {
    pthread_mutex_lock(&g_sh->score_mtx);

    FILE *f = fopen(path, "w");
    if (!f) {
        pthread_mutex_unlock(&g_sh->score_mtx);
        return;
    }

    for (int pid = 1; pid <= 2; pid++) {
        fprintf(f, "%d %d %s\n", pid, g_sh->score_table[pid].wins,
                g_sh->score_table[pid].name[0] ? g_sh->score_table[pid].name : (pid == 1 ? "GuesserA" : "GuesserB"));
    }
    fclose(f);

    pthread_mutex_unlock(&g_sh->score_mtx);
}

// ---------- Shared memory init ----------
static void init_process_shared_mutex(pthread_mutex_t *mtx) {
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED);
    pthread_mutex_init(mtx, &attr);
    pthread_mutexattr_destroy(&attr);
}

static void shm_init_or_attach(bool create) {
    int fd;
    if (create) {
        fd = shm_open(SHM_NAME, O_CREAT | O_EXCL | O_RDWR, 0666);
        if (fd < 0) {
            perror("shm_open(create)");
            exit(1);
        }
        if (ftruncate(fd, (off_t)sizeof(shared_t)) != 0) {
            perror("ftruncate");
            exit(1);
        }
    } else {
        fd = shm_open(SHM_NAME, O_RDWR, 0666);
        if (fd < 0) {
            perror("shm_open(open)");
            exit(1);
        }
    }

    void *mem = mmap(NULL, sizeof(shared_t), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (mem == MAP_FAILED) {
        perror("mmap");
        exit(1);
    }
    close(fd);

    g_sh = (shared_t*)mem;

    if (create) {
        memset(g_sh, 0, sizeof(*g_sh));

        init_process_shared_mutex(&g_sh->game_mtx);
        init_process_shared_mutex(&g_sh->score_mtx);
        init_process_shared_mutex(&g_sh->log_mtx);

        for (int i = 0; i < MAX_PLAYERS; i++) {
            sem_init(&g_sh->turn_sem[i], 1, 0); // pshared=1
            g_sh->connected[i] = 0;
            g_sh->score[i] = 0;
            g_sh->player_name[i][0] = '\0';
        }

        sem_init(&g_sh->log_items,  1, 0);
        sem_init(&g_sh->log_spaces, 1, LOGQ_CAP);
        g_sh->log_head = 0;
        g_sh->log_tail = 0;

        for (int i = 0; i < MAX_PLAYERS; i++) {
            init_process_shared_mutex(&g_sh->out_mtx[i]);
            sem_init(&g_sh->out_items[i], 1, 0);
            sem_init(&g_sh->out_spaces[i], 1, OUTQ_CAP);
            g_sh->out_head[i] = 0;
            g_sh->out_tail[i] = 0;
        }

        g_sh->phase = PHASE_WAITING_PLAYERS;
        g_sh->current_turn = 0;
        g_sh->position_idx = 0;
        g_sh->guess_count_for_pos = 0;
        g_sh->secret_word[0] = '\0';
        for (int i = 0; i < WORD_LEN; i++) g_sh->display[i] = '_';
        g_sh->display[WORD_LEN] = '\0';

        g_sh->game_number = 0;
        g_sh->shutting_down = 0;
    }
}

// ---------- SIGCHLD reaper ----------
static void sigchld_handler(int signo) {
    (void)signo;
    while (waitpid(-1, NULL, WNOHANG) > 0) { }
}

// ---------- SIGINT for graceful shutdown ----------
static volatile sig_atomic_t g_sigint = 0;
static void sigint_handler(int signo) {
    (void)signo;
    g_sigint = 1;
    if (g_sh) g_sh->shutting_down = 1;
}

static void out_enqueue(int target_player, const char *msg) {
    if (target_player < 0 || target_player >= MAX_PLAYERS) return;

    // If queue is full, drop the message to avoid blocking gameplay
    if (sem_trywait(&g_sh->out_spaces[target_player]) != 0) return;

    pthread_mutex_lock(&g_sh->out_mtx[target_player]);
    int idx = g_sh->out_tail[target_player];
    g_sh->out_tail[target_player] = (g_sh->out_tail[target_player] + 1) % OUTQ_CAP;

    snprintf(g_sh->outq[target_player][idx], OUT_MSG_LEN, "%s", msg);

    pthread_mutex_unlock(&g_sh->out_mtx[target_player]);
    sem_post(&g_sh->out_items[target_player]);
}

static void out_drain_to_socket(int my_id, int client_fd) {
    // Drain everything currently queued for this player
    while (sem_trywait(&g_sh->out_items[my_id]) == 0) {
        pthread_mutex_lock(&g_sh->out_mtx[my_id]);

        int idx = g_sh->out_head[my_id];
        g_sh->out_head[my_id] = (g_sh->out_head[my_id] + 1) % OUTQ_CAP;

        char msg[OUT_MSG_LEN];
        snprintf(msg, sizeof(msg), "%s", g_sh->outq[my_id][idx]);

        pthread_mutex_unlock(&g_sh->out_mtx[my_id]);
        sem_post(&g_sh->out_spaces[my_id]);

        // send as a line so client receives it cleanly
        send_line(client_fd, msg);
    }
}

// ---------- Logger thread ----------
static void *logger_thread_main(void *arg) {
    (void)arg;
    FILE *f = fopen("game.log", "a");
    if (!f) {
        perror("fopen(game.log)");
        return NULL;
    }

    while (1) {
        // If shutting down and no more items, exit
        if (g_sh->shutting_down) {
            int sval = 0;
            sem_getvalue(&g_sh->log_items, &sval);
            if (sval <= 0) break;
        }

        if (sem_wait(&g_sh->log_items) == -1) {
            if (errno == EINTR) continue;
            break;
        }

        pthread_mutex_lock(&g_sh->log_mtx);
        int idx = g_sh->log_head;
        g_sh->log_head = (g_sh->log_head + 1) % LOGQ_CAP;

        char line[LOG_MSG_LEN];
        strncpy(line, g_sh->logq[idx], sizeof(line) - 1);
        line[sizeof(line) - 1] = '\0';
        pthread_mutex_unlock(&g_sh->log_mtx);

        sem_post(&g_sh->log_spaces);

        fprintf(f, "%s\n", line);
        fflush(f);
    }

    fclose(f);
    return NULL;
}

// ---------- Scheduler thread (Round Robin turns for guessers) ----------
static void reset_game_state_locked(void) {
    // game_mtx must be held
    g_sh->position_idx = 0;
    g_sh->guess_count_for_pos = 0;
    g_sh->score[1] = 0;
    g_sh->score[2] = 0;
    for (int i = 0; i < WORD_LEN; i++) g_sh->display[i] = '_';
    g_sh->display[WORD_LEN] = '\0';
    g_sh->current_turn = 0; // will be set when starting
    g_sh->pass_num = 0;
}

static void *scheduler_thread_main(void *arg) {
    (void)arg;

    while (!g_sh->shutting_down) {
        pthread_mutex_lock(&g_sh->game_mtx);

        // Wait until 3 players connected
        if (g_sh->phase == PHASE_WAITING_PLAYERS) {
            if (g_sh->connected[0] && g_sh->connected[1] && g_sh->connected[2]) {
                g_sh->phase = PHASE_WAITING_WORD;
                g_sh->game_number++;
                log_enqueuef("All players connected. Starting game #%d. Waiting for wordmaster.", g_sh->game_number);
                g_sh->current_turn = 0;
                g_sh->guess_count_for_pos = 0; // scheduler gate
                sem_post(&g_sh->turn_sem[0]);  // wake wordmaster
            }
            pthread_mutex_unlock(&g_sh->game_mtx);
            usleep(10 * 1000);
            continue;
        }

        // Waiting for wordmaster to set secret word
        if (g_sh->phase == PHASE_WAITING_WORD) {
            pthread_mutex_unlock(&g_sh->game_mtx);
            usleep(10 * 1000);
            continue;
        }

        // In progress: one guess per position, alternating turns
        if (g_sh->phase == PHASE_IN_PROGRESS) {
            if (!g_sh->connected[1] || !g_sh->connected[2]) {
                g_sh->phase = PHASE_GAME_OVER;
                log_enqueuef("A guesser disconnected. Ending game #%d.", g_sh->game_number);
                pthread_mutex_unlock(&g_sh->game_mtx);
                usleep(10 * 1000);
                continue;
            }

            // gate: post exactly once per turn
            if (g_sh->guess_count_for_pos == 0) {
                int next = g_sh->current_turn;
                if (next != 1 && next != 2) next = 1;
                g_sh->current_turn = next;
                g_sh->guess_count_for_pos = 1;

                log_enqueuef("Turn: player %d (pass=%d/5 pos=%d display=%s scoreA=%d scoreB=%d)",
                             next, g_sh->pass_num + 1, g_sh->position_idx + 1,
                             g_sh->display, g_sh->score[1], g_sh->score[2]);

                sem_post(&g_sh->turn_sem[next]);
            }

            pthread_mutex_unlock(&g_sh->game_mtx);
            usleep(10 * 1000);
            continue;
        }

        // Game over: reset and ask wordmaster for next game
        if (g_sh->phase == PHASE_GAME_OVER) {
            reset_game_state_locked();
            g_sh->secret_word[0] = '\0';
            g_sh->phase = PHASE_WAITING_WORD;
            g_sh->current_turn = 0;
            g_sh->guess_count_for_pos = 0;
            log_enqueuef("Reset complete. Waiting for wordmaster for game #%d.", g_sh->game_number + 1);
            sem_post(&g_sh->turn_sem[0]);
            pthread_mutex_unlock(&g_sh->game_mtx);
            usleep(10 * 1000);
            continue;
        }

        pthread_mutex_unlock(&g_sh->game_mtx);
        usleep(10 * 1000);
    }

    return NULL;
}

// ---------- Child session handlers ----------
static int is_valid_word(const char *w) {
    if ((int)strlen(w) != WORD_LEN) return 0;
    for (int i = 0; i < WORD_LEN; i++) {
        if (w[i] < 'A' || w[i] > 'Z') return 0;
    }
    return 1;
}

static int parse_name(const char *line, char *out, size_t cap) {
    // expects: "NAME <token>"
    if (strncmp(line, "NAME ", 5) != 0) return -1;
    const char *p = line + 5;
    if (!*p) return -1;
    snprintf(out, cap, "%.*s", (int)(cap - 1), p);
    return 0;
}

static int is_word_revealed_locked(void) {
    // game_mtx must be held
    for (int i = 0; i < WORD_LEN; i++) {
        if (g_sh->display[i] == '_') return 0;
    }
    return 1;
}

static void child_wordmaster_loop(int client_fd, int player_id) {
    (void)player_id;

    send_line(client_fd, "ROLE WORDMASTER");
    send_line(client_fd, "INFO You will enter a 5-letter secret word (A-Z).");

    while (1) {
        // Block until scheduler signals it's time to enter word
        // sem_wait(&g_sh->turn_sem[0]);
        // if (g_sh->shutting_down) break;

        while (1) {
            if (g_sh->shutting_down) return;

            out_drain_to_socket(0, client_fd);

            if (sem_trywait(&g_sh->turn_sem[0]) == 0) {
                break;
            }

            usleep(20 * 1000);
        }

        pthread_mutex_lock(&g_sh->game_mtx);
        if (!g_sh->connected[0]) {
            pthread_mutex_unlock(&g_sh->game_mtx);
            break;
        }
        if (g_sh->phase != PHASE_WAITING_WORD) {
            pthread_mutex_unlock(&g_sh->game_mtx);
            continue;
        }
        pthread_mutex_unlock(&g_sh->game_mtx);

        send_line(client_fd, "ENTER_WORD Please send: WORD ABCDE");

        // Receive until valid WORD
        while (1) {
            char line[256];
            ssize_t r = recv_line(client_fd, line, sizeof(line));
            if (r <= 0) {
                pthread_mutex_lock(&g_sh->game_mtx);
                g_sh->connected[0] = 0;
                pthread_mutex_unlock(&g_sh->game_mtx);
                log_enqueuef("Wordmaster disconnected.");
                return;
            }

            if (strncmp(line, "WORD ", 5) == 0) {
                char w[WORD_LEN + 1];
                snprintf(w, sizeof(w), "%s", line + 5);

                // Uppercase normalize
                for (int i = 0; i < WORD_LEN; i++) {
                    if (w[i] >= 'a' && w[i] <= 'z') w[i] = (char)(w[i] - 'a' + 'A');
                }

                if (!is_valid_word(w)) {
                    send_line(client_fd, "ERR Word must be exactly 5 letters A-Z. Try again.");
                    continue;
                }

                pthread_mutex_lock(&g_sh->game_mtx);
                strncpy(g_sh->secret_word, w, WORD_LEN);
                g_sh->secret_word[WORD_LEN] = '\0';
                g_sh->position_idx = 0;
                g_sh->pass_num = 0;
                g_sh->current_turn = 1;
                g_sh->guess_count_for_pos = 0;
                g_sh->phase = PHASE_IN_PROGRESS;
                log_enqueuef("Wordmaster set secret word for game #%d.", g_sh->game_number);
                pthread_mutex_unlock(&g_sh->game_mtx);

                send_line(client_fd, "OK Word accepted. Game started.");
                break;
            } else {
                send_line(client_fd, "ERR Expected: WORD ABCDE");
            }
        }
    }
}

static void broadcast_state_to_guessers(int fd1, int fd2) {
    // In this skeleton, children send updates only to their own client.
    // If you want full broadcast, you can store client fds in shared memory (not recommended),
    // or implement parent-managed broadcast. We'll keep it simple here.
    (void)fd1; (void)fd2;
}

static void child_guesser_loop(int client_fd, int player_id) {
    char role_msg[64];
    snprintf(role_msg, sizeof(role_msg), "ROLE GUESSER %d", player_id);
    send_line(client_fd, role_msg);
    send_line(client_fd, "INFO You will guess letters (A-Z) for each position 1..5 when prompted: GUESS X");

    while (1) {
        // Wait for our turn, but keep flushing broadcast messages while waiting
        while (1) {
            if (g_sh->shutting_down) return;

            out_drain_to_socket(player_id, client_fd);

            if (sem_trywait(&g_sh->turn_sem[player_id]) == 0) {
                break; // our turn
            }
            usleep(20 * 1000);
        }

        pthread_mutex_lock(&g_sh->game_mtx);
        if (!g_sh->connected[player_id]) {
            pthread_mutex_unlock(&g_sh->game_mtx);
            break;
        }
        if (g_sh->phase != PHASE_IN_PROGRESS || g_sh->current_turn != player_id) {
            pthread_mutex_unlock(&g_sh->game_mtx);
            usleep(10 * 1000);
            continue;
        }

        int pos = g_sh->position_idx;
        int pass = g_sh->pass_num;
        char disp[WORD_LEN + 1];
        strncpy(disp, g_sh->display, WORD_LEN);
        disp[WORD_LEN] = '\0';
        pthread_mutex_unlock(&g_sh->game_mtx);

        char prompt[256];
        snprintf(prompt, sizeof(prompt),
                 "YOUR_TURN pass=%d/5 pos=%d display=%s (send: GUESS X)", pass + 1, pos + 1, disp);
        if (send_line(client_fd, prompt) < 0) {
            pthread_mutex_lock(&g_sh->game_mtx);
            g_sh->connected[player_id] = 0;
            // release scheduler gate
            g_sh->guess_count_for_pos = 0;
            pthread_mutex_unlock(&g_sh->game_mtx);
            log_enqueuef("Player %d disconnected during prompt.", player_id);
            return;
        }

        // Read until valid GUESS line (so scheduler doesn't deadlock)
        char line[256];
        char ch = '\0';
        while (1) {
            ssize_t r = recv_line(client_fd, line, sizeof(line));
            if (r <= 0) {
                pthread_mutex_lock(&g_sh->game_mtx);
                g_sh->connected[player_id] = 0;
                g_sh->guess_count_for_pos = 0;
                pthread_mutex_unlock(&g_sh->game_mtx);
                log_enqueuef("Player %d disconnected.", player_id);
                return;
            }

            if (strncmp(line, "GUESS ", 6) == 0 && strlen(line + 6) >= 1) {
                ch = line[6];
                if (ch >= 'a' && ch <= 'z') ch = (char)(ch - 'a' + 'A');
                if (ch >= 'A' && ch <= 'Z') break;
                send_line(client_fd, "ERR Guess must be a single letter A-Z.");
                continue;
            }

            send_line(client_fd, "ERR Expected: GUESS X");
        }

        // Apply guess to shared state (one guess per position)
        pthread_mutex_lock(&g_sh->game_mtx);

        // Re-check still valid
        if (g_sh->phase != PHASE_IN_PROGRESS || g_sh->current_turn != player_id) {
            pthread_mutex_unlock(&g_sh->game_mtx);
            send_line(client_fd, "ERR Not your turn (race).");
            // allow scheduler to proceed
            pthread_mutex_lock(&g_sh->game_mtx);
            g_sh->guess_count_for_pos = 0;
            pthread_mutex_unlock(&g_sh->game_mtx);
            continue;
        }

        int pass_before = g_sh->pass_num;
        int pos_before  = g_sh->position_idx;

        int correct = (ch == g_sh->secret_word[pos_before]) ? 1 : 0;
        int present = 0;
        if (!correct) {
            for (int k = 0; k < WORD_LEN; k++) {
                if (g_sh->secret_word[k] == ch) { present = 1; break; }
            }
        }
        const char *result = correct ? "CORRECT" : (present ? "PRESENT" : "ABSENT");

        if (correct) {
            g_sh->score[player_id] += 1;
            g_sh->display[pos_before] = g_sh->secret_word[pos_before];
        }

        // Advance immediately (one guess per position)
        g_sh->position_idx += 1;
        if (g_sh->position_idx >= WORD_LEN) {
            g_sh->position_idx = 0;
            g_sh->pass_num += 1;
        }

        // Determine end of game
        if (is_word_revealed_locked() || g_sh->pass_num >= 5) {
            g_sh->phase = PHASE_GAME_OVER;
        } else {
            // Swap turn
            g_sh->current_turn = (player_id == 1) ? 2 : 1;
        }

        // Release scheduler gate so it can post next turn (or proceed to reset)
        g_sh->guess_count_for_pos = 0;

        // Snapshot state for UI sync
        char state[256];
        snprintf(state, sizeof(state),
                 "STATE from=%d pass=%d/5 pos=%d guess=%c result=%s display=%s scoreA=%d scoreB=%d next_pass=%d/5 next_pos=%d turn=%d",
                 player_id,
                 pass_before + 1,
                 pos_before + 1,
                 ch,
                 result,
                 g_sh->display,
                 g_sh->score[1],
                 g_sh->score[2],
                 (g_sh->pass_num + 1),
                 (g_sh->position_idx + 1),
                 (g_sh->phase == PHASE_IN_PROGRESS ? g_sh->current_turn : 0));

        int is_game_over = (g_sh->phase == PHASE_GAME_OVER);
        int s1 = g_sh->score[1];
        int s2 = g_sh->score[2];
        char secret[WORD_LEN + 1];
        strncpy(secret, g_sh->secret_word, WORD_LEN);
        secret[WORD_LEN] = '\0';

        pthread_mutex_unlock(&g_sh->game_mtx);

        // Send state to everyone: self directly, others via queue
        send_line(client_fd, state);
        out_enqueue(0, state);
        out_enqueue((player_id == 1) ? 2 : 1, state);

        log_enqueuef("Player %d guessed '%c' for pos %d -> %s (scoreA=%d scoreB=%d)",
                     player_id, ch, pos_before + 1, result, s1, s2);

        if (is_game_over) {
            int winner = 0;
            if (s1 > s2) winner = 1;
            else if (s2 > s1) winner = 2;

            // Update persistent wins
            pthread_mutex_lock(&g_sh->score_mtx);
            if (winner == 1 || winner == 2) {
                g_sh->score_table[winner].wins += 1;
                if (g_sh->player_name[winner][0]) {
                    strncpy(g_sh->score_table[winner].name, g_sh->player_name[winner], NAME_LEN - 1);
                    g_sh->score_table[winner].name[NAME_LEN - 1] = '\0';
                }
            }
            pthread_mutex_unlock(&g_sh->score_mtx);

            scores_save("scores.txt");

            char endmsg[256];
            snprintf(endmsg, sizeof(endmsg),
                     "GAME_OVER word=%s display=%s passes=%d scoreA=%d scoreB=%d winner=%s",
                     secret,
                     g_sh->display,   // safe: display is stable now
                     g_sh->pass_num,
                     s1, s2,
                     (winner == 0 ? "DRAW" : (winner == 1 ? "PLAYER1" : "PLAYER2")));

            // Notify everyone of game end
            send_line(client_fd, endmsg);
            out_enqueue(0, endmsg);
            out_enqueue((player_id == 1) ? 2 : 1, endmsg);
        }
    }
}

static void child_session(int client_fd, int player_id) {
    // Ask for name first
    send_line(client_fd, "WELCOME Please identify: NAME yourname");

    char line[256];
    ssize_t r = recv_line(client_fd, line, sizeof(line));
    if (r <= 0) {
        close(client_fd);
        return;
    }

    char name[NAME_LEN];
    if (parse_name(line, name, sizeof(name)) != 0) {
        send_line(client_fd, "ERR Expected: NAME yourname");
        close(client_fd);
        return;
    }

    pthread_mutex_lock(&g_sh->game_mtx);
    g_sh->connected[player_id] = 1;
    strncpy(g_sh->player_name[player_id], name, NAME_LEN - 1);
    g_sh->player_name[player_id][NAME_LEN - 1] = '\0';
    pthread_mutex_unlock(&g_sh->game_mtx);

    log_enqueuef("Player %d connected as '%s'.", player_id, name);

    if (player_id == 0) child_wordmaster_loop(client_fd, player_id);
    else child_guesser_loop(client_fd, player_id);

    pthread_mutex_lock(&g_sh->game_mtx);
    g_sh->connected[player_id] = 0;
    pthread_mutex_unlock(&g_sh->game_mtx);
    log_enqueuef("Player %d disconnected.", player_id);

    close(client_fd);
}

// ---------- Server socket ----------
static int make_listen_socket(uint16_t port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); exit(1); }

    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);

    if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("bind");
        exit(1);
    }
    if (listen(fd, 16) < 0) {
        perror("listen");
        exit(1);
    }
    return fd;
}

// ---------- main ----------
int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <port>\nExample: %s 5000\n", argv[0], argv[0]);
        return 1;
    }
    uint16_t port = (uint16_t)atoi(argv[1]);

    // Signals
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sigchld_handler;
    sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;
    sigaction(SIGCHLD, &sa, NULL);

    struct sigaction si;
    memset(&si, 0, sizeof(si));
    si.sa_handler = sigint_handler;
    sigaction(SIGINT, &si, NULL);

    // Create shared memory (fresh run: remove if leftover)
    shm_unlink(SHM_NAME);
    shm_init_or_attach(true);

    // Load persistent scores
    scores_load("scores.txt");
    log_enqueuef("Server starting on port %u.", (unsigned)port);

    // Start threads (parent only)
    pthread_t logger_th, sched_th;
    if (pthread_create(&logger_th, NULL, logger_thread_main, NULL) != 0) {
        perror("pthread_create(logger)");
        return 1;
    }
    if (pthread_create(&sched_th, NULL, scheduler_thread_main, NULL) != 0) {
        perror("pthread_create(scheduler)");
        return 1;
    }

    // Create listening socket
    g_listen_fd = make_listen_socket(port);

    // Accept exactly 3 players; assign by connection order
    int next_player_id = 0;
    while (!g_sigint && next_player_id < MAX_PLAYERS) {
        struct sockaddr_in cli;
        socklen_t clen = sizeof(cli);
        int cfd = accept(g_listen_fd, (struct sockaddr*)&cli, &clen);
        if (cfd < 0) {
            if (errno == EINTR) continue;
            perror("accept");
            break;
        }

        int pid = fork();
        if (pid < 0) {
            perror("fork");
            close(cfd);
            continue;
        }
        if (pid == 0) {
            // child
            close(g_listen_fd);
            // Child attaches to shared memory (already mapped by fork, so g_sh is valid)
            child_session(cfd, next_player_id);
            _exit(0);
        } else {
            // parent
            close(cfd);
            log_enqueuef("Forked child %d for player slot %d.", pid, next_player_id);
            next_player_id++;
        }
    }

    // Parent: keep running to allow multiple games until SIGINT
    // In this skeleton we keep the accept loop closed after 3 players; you could extend to reconnect logic later.
    while (!g_sigint) {
        usleep(50 * 1000);
    }

    // Shutdown
    log_enqueuef("Server shutting down (SIGINT). Saving scores and cleaning up.");
    g_sh->shutting_down = 1;

    // Save scores
    scores_save("scores.txt");

    // Join threads
    pthread_join(sched_th, NULL);
    pthread_join(logger_th, NULL);

    if (g_listen_fd >= 0) close(g_listen_fd);

    munmap(g_sh, sizeof(shared_t));
    shm_unlink(SHM_NAME);

    return 0;
}
