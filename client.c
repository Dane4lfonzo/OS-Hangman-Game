// client.c - TCP client for the 3-player word guessing game
// Build: gcc -O2 -Wall -Wextra -pedantic client.c -o client
//
// Usage:
//   ./client <server_ip> <port> <name>
// Example:
//   ./client 127.0.0.1 5000 Alice

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static int game_active = 0;

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
    char buf[512];
    snprintf(buf, sizeof(buf), "%s\n", line);
    return (send_all(fd, buf, strlen(buf)) < 0) ? -1 : 0;
}

static ssize_t recv_line(int fd, char *out, size_t cap) {
    size_t n = 0;
    while (n + 1 < cap) {
        char c;
        ssize_t r = recv(fd, &c, 1, 0);
        if (r < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (r == 0) return 0;
        if (c == '\n') break;
        if (c == '\r') continue;
        out[n++] = c;
    }
    out[n] = '\0';
    return (ssize_t)n;
}

static int connect_to(const char *ip, uint16_t port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); exit(1); }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);

    if (inet_pton(AF_INET, ip, &addr.sin_addr) != 1) {
        fprintf(stderr, "Invalid IP: %s\n", ip);
        exit(1);
    }
    if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("connect");
        exit(1);
    }
    return fd;
}

// --- UI state ---
static int my_player_id = 0;   // 0 = wordmaster, 1/2 = guesser
static int current_pass = 1;   // 1..5
static int current_turn = 0;   // 0/1/2
static int cursor_pos0  = 0;   // 0..4
static char row[6] = "_____";  // feedback row for current pass

static void reset_row(void) {
    for (int i = 0; i < 5; i++) row[i] = '_';
    row[5] = '\0';
}

static void render_screen(int pass, int pos0) {
    // Always redraw from the top cleanly
    // 1) go home
    // 2) clear from cursor to end (removes leftover blocks below)
    printf("\033[H\033[J");

    // If you still want one-time full clear on "new game start", keep this:
    // (optional) but NOT necessary if you use H+J every time
    if (!game_active) {
        game_active = 1;
    }

    // Helper macro: print line + clear to end of line
    // \033[K clears from cursor to end-of-line, prevents trailing ghosts
    #define PRINT_LINE(fmt, ...) do { \
        printf(fmt, ##__VA_ARGS__);   \
        printf("\033[K\n");           \
    } while (0)

    PRINT_LINE("               Round %d", pass);
    PRINT_LINE("");

    if (my_player_id == 0) PRINT_LINE("----------Wordmaster view----------");
    else PRINT_LINE("----------Player%d view----------", my_player_id);

    PRINT_LINE("");

    if (current_turn == 1 || current_turn == 2) {
        if (my_player_id == current_turn) PRINT_LINE("Turn: player%d (YOU)", current_turn);
        else PRINT_LINE("Turn: player%d", current_turn);
    } else {
        PRINT_LINE("Turn: -");
    }

    PRINT_LINE("");

    // Row line
    printf("               ");
    for (int i = 0; i < 5; i++) {
        char c = row[i] ? row[i] : '_';
        printf("%c", c);
        if (i != 4) printf(" ");
    }
    printf("\033[K\n"); // clear rest of that line

    // Caret line
    if (pos0 < 0) pos0 = 0;
    if (pos0 > 4) pos0 = 4;
    int caret_offset = pos0 * 2;

    printf("               ");
    for (int i = 0; i < caret_offset; i++) printf(" ");
    printf("^\033[K\n");

    PRINT_LINE("");

    fflush(stdout);

    #undef PRINT_LINE
}

static void handle_state_line(const char *line) {
    // STATE from=1 pass=1/5 pos=2 guess=A result=PRESENT display=_A___ scoreA=0 scoreB=0 next_pass=1/5 next_pos=3 turn=2
    int pass=1, pos=1, next_pass=1, next_pos=1, turn=0;
    char guess='?';
    char result[16] = "";
    const char *p;
    char disp[6] = "_____";

    p = strstr(line, "result=");
    if (p) {
        p += 7;
        int i = 0;
        while (p[i] && p[i] != ' ' && i < 15) {
            result[i] = p[i];
            i++;
        }
        result[i] = '\0';
    }

    p = strstr(line, "display=");
    if (p) {
        p += 8; // skip "display="
        int i = 0;
        while (p[i] && p[i] != ' ' && i < 5) {
            disp[i] = p[i];
            i++;
        }
        for (; i < 5; i++) disp[i] = '_';
        disp[5] = '\0';
    }

    p = strstr(line, "pass=");
    if (p) pass = atoi(p + 5);

    p = strstr(line, "pos=");
    if (p) pos = atoi(p + 4);

    p = strstr(line, "guess=");
    if (p) guess = p[6];

    p = strstr(line, "result=");
    if (p) {
        p += 7;
        int i = 0;
        while (p[i] && p[i] != ' ' && i < 15) { result[i] = p[i]; i++; }
        result[i] = '\0';
    }

    p = strstr(line, "next_pass=");
    if (p) next_pass = atoi(p + 10);

    p = strstr(line, "next_pos=");
    if (p) next_pos = atoi(p + 9);

    p = strstr(line, "turn=");
    if (p) turn = atoi(p + 5);

    // NEW GAME START: server reset display to _____ at pass 1
    if (pass == 1 && pos == 1 && strcmp(disp, "_____") == 0) {
        current_pass = 1;
        reset_row();    // <-- THIS resets the screen to _ _ _ _ _
    }

    // If pass advanced, reset feedback row
    if (next_pass != current_pass) {
        current_pass = next_pass;
        reset_row();
    } else if (pass != current_pass) {
        current_pass = next_pass;
        reset_row();
    }

    // Update feedback at the position that was just guessed
    int idx = pos - 1;
    if (idx >= 0 && idx < 5) {
        char up = guess;
        if (up >= 'a' && up <= 'z') up = (char)(up - 'a' + 'A');
        if (strcmp(result, "CORRECT") == 0) row[idx] = up;
        else if (strcmp(result, "PRESENT") == 0) row[idx] = '*';
        else if (strcmp(result, "ABSENT") == 0) row[idx] = '-';
        else row[idx] = '_';
    }

    current_turn = turn;
    cursor_pos0 = (next_pos > 0) ? (next_pos - 1) : 0;

    render_screen(current_pass, cursor_pos0);
}

int main(int argc, char **argv) {
    if (argc != 4) {
        fprintf(stderr, "Usage: %s <server_ip> <port> <name>\n", argv[0]);
        return 1;
    }

    const char *ip = argv[1];
    uint16_t port = (uint16_t)atoi(argv[2]);
    const char *name = argv[3];

    int fd = connect_to(ip, port);

    char line[512];
    if (recv_line(fd, line, sizeof(line)) <= 0) {
        fprintf(stderr, "Server closed.\n");
        close(fd);
        return 1;
    }
    printf("%s\n", line);

    char msg[128];
    snprintf(msg, sizeof(msg), "NAME %s", name);
    send_line(fd, msg);

    while (1) {
        ssize_t r = recv_line(fd, line, sizeof(line));
        if (r <= 0) {
            printf("Disconnected.\n");
            break;
        }

        // STATE updates redraw everyone
        if (strncmp(line, "STATE", 5) == 0) {
            handle_state_line(line);
            continue;
        }

        // Role assignment
        if (strncmp(line, "ROLE GUESSER", 11) == 0) {
            my_player_id = atoi(line + 12);
            reset_row();
            current_pass = 1;
            current_turn = 0;
            cursor_pos0 = 0;
            render_screen(current_pass, cursor_pos0);
            continue;
        }
        if (strncmp(line, "ROLE WORDMASTER", 15) == 0) {
            my_player_id = 0;
            reset_row();
            current_pass = 1;
            current_turn = 0;
            cursor_pos0 = 0;
            render_screen(current_pass, cursor_pos0);
            continue;
        }

        // Wordmaster prompt
        if (strncmp(line, "ENTER_WORD", 10) == 0) {
            printf("%s\n", line);
            char word[64];
            printf("Input (WORD ABCDE): ");
            if (!fgets(word, sizeof(word), stdin)) break;
            word[strcspn(word, "\r\n")] = 0;
            send_line(fd, word);
            continue;
        }

        if (strncmp(line, "GAME_OVER", 9) == 0) {
            printf("\n=== GAME OVER ===\n");
            printf("%s\n", line);

            game_active = 0;   // <-- THIS IS STEP 3

            continue;
        }

        // Your turn prompt
        if (strncmp(line, "YOUR_TURN", 8) == 0) {
            int pass = current_pass;
            int pos = cursor_pos0 + 1;

            char *p_pass = strstr(line, "pass=");
            if (p_pass) pass = atoi(p_pass + 5);
            char *p_pos = strstr(line, "pos=");
            if (p_pos) pos = atoi(p_pos + 4);

            current_pass = pass;
            cursor_pos0 = (pos > 0) ? (pos - 1) : 0;
            current_turn = my_player_id;

            render_screen(current_pass, cursor_pos0);
            printf("Input letter: \033[K");
            fflush(stdout);

            printf("Input letter: ");
            fflush(stdout);

            char guess[64];
            if (!fgets(guess, sizeof(guess), stdin)) break;
            guess[strcspn(guess, "\r\n")] = 0;

            if (strlen(guess) == 1 &&
                ((guess[0] >= 'A' && guess[0] <= 'Z') || (guess[0] >= 'a' && guess[0] <= 'z'))) {
                char out[64];
                snprintf(out, sizeof(out), "GUESS %c", guess[0]);
                send_line(fd, out);
            } else {
                send_line(fd, guess);
            }
            continue;
        }

        // Game over: print message (screen will already show last STATE)
        // if (strncmp(line, "GAME_OVER", 9) == 0) {
        //     printf("%s\n", line);
        //     continue;
        // }

        // Default: print other messages
        printf("%s\n", line);
    }

    close(fd);
    return 0;
}
