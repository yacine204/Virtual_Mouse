#include <fcntl.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <string.h>
#include <poll.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <stdint.h>
#include <stddef.h>

#include "include/body.h"
#include "include/header.h"
#include "include/request.h"
#include "include/response.h"
#include "include/response_status_line.h"
#include "include/routing.h"

void UPDATE_MOUSE_STATE(int fd, int dx, int dy, bool left_pressed, bool right_pressed) {
    struct input_event events[5] = {
        { .type = EV_REL, .code = REL_X, .value = dx },
        { .type = EV_REL, .code = REL_Y, .value = dy },
        { .type = EV_KEY, .code = BTN_LEFT, .value = left_pressed ? 1 : 0 },
        { .type = EV_KEY, .code = BTN_RIGHT, .value = right_pressed ? 1 : 0 },
        { .type = EV_SYN, .code = SYN_REPORT, .value = 0 }
    };

    if (write(fd, events, sizeof(events)) < 0) {
        perror("write");
    }
}

typedef struct {
    int x;
    int y;
    bool left_btn;
    bool right_btn;
} STATE;

void parse_response_state(struct REQUEST *res, STATE *state) {
    if (!res || !res->body.data || !state) {
        return;
    }

    int x = 0, y = 0, left = 0, right = 0;

    int parsed = sscanf(
        res->body.data,
        "x:%d,y:%d,left_btn:%d,right_button:%d",
        &x, &y, &left, &right
    );

    if (parsed == 4) {
        state->x = x;
        state->y = y;
        state->left_btn = (left != 0);
        state->right_btn = (right != 0);
    }
}


static void send_simple_ok(int fd) {
    static const char resp[] =
        "HTTP/1.1 200 OK\r\n"
        "Content-Length: 2\r\n"
        "Content-Type: text/plain\r\n"
        "Connection: keep-alive\r\n"
        "\r\n"
        "ok";
    ssize_t n = send(fd, resp, sizeof(resp) - 1, MSG_NOSIGNAL);
    (void)n;
}

int main(void) {
    int ret = 1;

    int input_fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
    if (input_fd < 0) {
        perror("open /dev/uinput");
        return 1;
    }

    ioctl(input_fd, UI_SET_EVBIT, EV_REL);
    ioctl(input_fd, UI_SET_RELBIT, REL_X);
    ioctl(input_fd, UI_SET_RELBIT, REL_Y);

    ioctl(input_fd, UI_SET_EVBIT, EV_KEY);
    ioctl(input_fd, UI_SET_KEYBIT, BTN_LEFT);
    ioctl(input_fd, UI_SET_KEYBIT, BTN_RIGHT);

    struct uinput_setup usetup = {0};
    usetup.id.bustype = BUS_USB;
    usetup.id.vendor  = 0;
    usetup.id.product = 0;
    snprintf(usetup.name, UINPUT_MAX_NAME_SIZE, "remote-mouse");

    if (ioctl(input_fd, UI_DEV_SETUP, &usetup) < 0) {
        perror("UI_DEV_SETUP");
        goto out_input;
    }

    if (ioctl(input_fd, UI_DEV_CREATE) < 0) {
        perror("UI_DEV_CREATE");
        goto out_input;
    }

    sleep(1);

    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        perror("socket");
        goto out_uinput;
    }

    int opt = 1;
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof opt) < 0) {
        perror("setsockopt");
        goto out_server;
    }

    struct sockaddr_in server = {0};
    server.sin_addr.s_addr   = INADDR_ANY;
    server.sin_family = AF_INET;
    server.sin_port   = htons(8080);

    if (bind(server_fd, (struct sockaddr *)&server, sizeof(server)) < 0) {
        perror("bind");
        goto out_server;
    }

    if (listen(server_fd, 5) < 0) {
        perror("listen");
        goto out_server;
    }


    enum { MAX_CLIENTS = 1};
    struct pollfd polls[1 + MAX_CLIENTS];
    bool index_html_served[1 + MAX_CLIENTS] = {0};

    for (int i = 0; i < 1 + MAX_CLIENTS; i++) {
        polls[i].fd = -1;
        polls[i].events = 0;
        polls[i].revents = 0;
    }
    polls[0].fd = server_fd;
    polls[0].events = POLLIN;
    nfds_t nfds = 1;

    char buffer[65536];
    STATE state = {0}; 

    for (;;) {
        int ready = poll(polls, nfds, -1);
        if (ready < 0) {
            if (errno == EINTR) continue;
            perror("poll");
            break;
        }

        if (polls[0].revents & POLLIN) {
            struct sockaddr_in client;
            socklen_t client_len = sizeof(client);
            int client_fd = accept(server_fd, (struct sockaddr *)&client, &client_len);
            if (client_fd >= 0) {
                char ip[INET_ADDRSTRLEN];
                printf("[%s] connected.\n",
                       inet_ntop(AF_INET, &client.sin_addr, ip, sizeof(ip)));
                fflush(stdout);

                int slot = -1;
                for (int i = 1; i < 1 + MAX_CLIENTS; i++) {
                    if (polls[i].fd == -1) { slot = i; break; }
                }
                if (slot == -1) {
                    fprintf(stderr, "too many clients, rejecting\n");
                    close(client_fd);
                } else {
                    polls[slot].fd = client_fd;
                    polls[slot].events = POLLIN;
                    polls[slot].revents = 0;
                    index_html_served[slot] = false;
                    if ((nfds_t)slot >= nfds) nfds = slot + 1;
                }
            }
        }

        for (int i = 1; i < 1 + MAX_CLIENTS; i++) {
            if (polls[i].fd < 0) continue;
            if (!(polls[i].revents & (POLLIN | POLLERR | POLLHUP))) continue;

            ssize_t bytes = recv(polls[i].fd, buffer, sizeof(buffer) - 1, 0);
            if (bytes <= 0) {
                printf("Disconnected (slot %d).\n", i);
                fflush(stdout);
                close(polls[i].fd);
                polls[i].fd = -1;
                polls[i].events = 0;
                polls[i].revents = 0;
                index_html_served[i] = false;
                continue;
            }

            buffer[bytes] = '\0';

            struct REQUEST req = parse_request(buffer);

            if (!index_html_served[i]) {
                
                struct RESPONSE res = handle_response(req);
                size_t response_len = 0;
                char *raw_response = serialize_response(res, &response_len);
                if (raw_response != NULL) {
                    send(polls[i].fd, raw_response, response_len, MSG_NOSIGNAL);
                    free(raw_response);
                    index_html_served[i] = true;
                }
            } else {
                
                parse_response_state(&req, &state);
                UPDATE_MOUSE_STATE(input_fd, state.x, state.y, state.left_btn, state.right_btn);
                send_simple_ok(polls[i].fd);
            }
        }

       
        while (nfds > 1 && polls[nfds - 1].fd == -1) nfds--;
    }

out_server:
    close(server_fd);
out_uinput:
    ioctl(input_fd, UI_DEV_DESTROY);
out_input:
    close(input_fd);
    return ret;
}