// server.c (annotated)
// Listens on TCP port 12345, accepts many connections via epoll,
// each client sends a 4‑byte size (network order), and the server
// streams back that many 'A's, then waits for the next size.

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <stdint.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/epoll.h>
#include <signal.h>

// Add global flag
static volatile int running = 1;

// Add signal handler
static void sig_handler(int signo) {
    printf("[server] Received signal %d, shutting down...\n", signo);
    running = 0;
}

// #define PORT 12345
#define MAX_EVENTS  64
#define BUF_SIZE    (1024 * 1024 * 128)    // send in up to 128 MB chunks

// Per-client state
struct client {
    int    fd;           // socket fd
    int hdr;      // buffer to accumulate 4-byte size header
    int    hdr_read;     // how many header bytes read so far
    size_t to_send;      // total payload bytes to send (decoded from header)
    size_t sent;         // how many payload bytes already sent
};

static char *send_buf;

// make a socket non-blocking
static int make_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

// clean up a client: remove from epoll, close FD, free state
static void cleanup_client(int epfd, struct client *c) {
    epoll_ctl(epfd, EPOLL_CTL_DEL, c->fd, NULL);
    close(c->fd);
    free(c);
}

int main(int argc, char *argv[]) {
    if (argc != 3) {
        fprintf(stderr, "Usage: %s <bind_ip> <port>\n", argv[0]);
        fprintf(stderr, "Example: %s 0.0.0.0 12345    # Listen on all interfaces\n", argv[0]);
        fprintf(stderr, "         %s 127.0.0.1 12345  # Listen only on localhost\n", argv[0]);
        exit(1);
    }

    const char* bind_ip = argv[1];
    int port = atoi(argv[2]);

    if (port <= 0 || port > 65535) {
        fprintf(stderr, "Invalid port number (must be between 1-65535)\n");
        exit(1);
    }

    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);
    
    // Pre‑fill a big buffer of 'A's
    send_buf = malloc(BUF_SIZE);
    if (!send_buf) { perror("malloc"); exit(1); }
    memset(send_buf, 'A', BUF_SIZE);

    // 1) Create and bind listening socket
    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) { perror("socket"); exit(1); }
    int opt = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    if (inet_pton(AF_INET, bind_ip, &addr.sin_addr) <= 0) {
        fprintf(stderr, "Invalid IP address: %s\n", bind_ip);
        exit(1);
    }
    addr.sin_port = htons(port);
    if (bind(listen_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("bind"); exit(1);
    }
    if (listen(listen_fd, SOMAXCONN) < 0) {
        perror("listen"); exit(1);
    }

    // 2) Make listening socket non-blocking
    if (make_nonblocking(listen_fd) < 0) {
        perror("fcntl"); exit(1);
    }

    // 3) Create epoll instance
    int epfd = epoll_create1(0);
    if (epfd < 0) { perror("epoll_create1"); exit(1); }

    // 4) Register listening socket with epoll for incoming connections
    struct epoll_event ev;
    ev.events = EPOLLIN | EPOLLET;   // edge-triggered read events
    ev.data.fd = listen_fd;
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, listen_fd, &ev) < 0) {
        perror("epoll_ctl: listen_fd"); exit(1);
    }

    printf("[server] listening on %s:%d\n", bind_ip, port);

    // 5) Main event loop
    struct epoll_event events[MAX_EVENTS];
    while (running) {
        int n = epoll_wait(epfd, events, MAX_EVENTS, -1);
        if (n < 0) {
            if (errno == EINTR) {
                fprintf(stderr, "[server] epoll_wait interrupted by signal %s\n", strerror(errno));
                continue;  // interrupted by signal, retry
            }
            fprintf(stderr, "[server] epoll_wait error: %s\n", strerror(errno));
            perror("epoll_wait");
            break;
        }

        // Process each ready file descriptor
        for (int i = 0; i < n; i++) {
            // --------------------------------------------
            // A) Is this the listening socket? (new client)
            // --------------------------------------------
            if ((events[i].events & EPOLLIN) && events[i].data.fd == listen_fd){
                // accept() in a loop until EAGAIN (edge‑triggered)
                while (1) {
                    struct sockaddr_in cli;
                    socklen_t len = sizeof(cli);
                    int conn_fd = accept(listen_fd, (struct sockaddr*)&cli, &len);
                    if (conn_fd < 0) {
                        // no more pending connections
                        if (errno == EAGAIN || errno == EWOULDBLOCK)
                            break;
                        perror("accept");
                        break;
                    }

                    // set non‑blocking on new client
                    make_nonblocking(conn_fd);

                    // allocate and initialize per‑client state
                    struct client *c = calloc(1, sizeof(*c));
                    c->fd       = conn_fd;
                    c->hdr_read = 0;
                    c->to_send  = 0;
                    c->sent     = 0;

                    // register client socket for EPOLLIN (header reads)
                    struct epoll_event cev = {
                        .events   = EPOLLIN | EPOLLET,
                        .data.ptr = c
                    };
                    epoll_ctl(epfd, EPOLL_CTL_ADD, conn_fd, &cev);

                    char ip[INET_ADDRSTRLEN];
                    inet_ntop(AF_INET, &cli.sin_addr, ip, sizeof(ip));
                    printf("[server] new client %s:%d (fd=%d)\n",
                           ip, ntohs(cli.sin_port), conn_fd);
                }
            }

            // --------------------------------------------------
            // B) Otherwise, it's an event on an existing client
            // --------------------------------------------------
            else {
                struct client *c = events[i].data.ptr;

                // B1) Hang up or error on the socket?
                if (events[i].events & (EPOLLHUP | EPOLLERR)) {
                    // client closed or error occurred → clean up
                    cleanup_client(epfd, c);
                    continue;
                }

                // B2) Ready to read header? (we need 4 bytes)
                if ((events[i].events & EPOLLIN) && c->hdr_read < 4) {
                    // keep reading until we've got all 4 bytes or hit EAGAIN
                    while (c->hdr_read < 4) {
                        ssize_t r = recv(c->fd,
                                         &c->hdr + c->hdr_read,
                                         4 - c->hdr_read,
                                         0);
                        printf("[server] fd=%d read %zd bytes\n", c->fd, r);
                        if (r > 0) {
                            c->hdr_read += r;
                        }
                        else if (r == 0 || (r < 0 && errno != EAGAIN)) {
                            // client closed or real error
                            printf("[server] fd=%d closed\n", c->fd);
                            cleanup_client(epfd, c);
                            goto next_event;
                        }
                        else {
                            // EAGAIN: no more data right now
                            printf("[server] fd=%d EAGAIN\n", c->fd);
                            break;
                        }
                    }
                    // If we've now got all 4 header bytes:
                    if (c->hdr_read == 4) {
                        
                        // decode network-order uint32_t
                        c->to_send = ntohl(c->hdr);
                        c->sent    = 0;

                        // switch this FD to EPOLLOUT to send payload
                        struct epoll_event mev = {
                            .events   = EPOLLOUT | EPOLLET,
                            .data.ptr = c
                        };
                        epoll_ctl(epfd, EPOLL_CTL_MOD, c->fd, &mev);

                        printf("[server] fd=%d requested %zu bytes\n",
                               c->fd, c->to_send);
                    }
                }
                // B3) Ready to write payload?
                else if (events[i].events & EPOLLOUT) {
                    // send in chunks until we've sent everything or hit EAGAIN
                    while (c->sent < c->to_send) {
                        size_t left  = c->to_send - c->sent;
                        size_t chunk = left > BUF_SIZE ? BUF_SIZE : left;
                        ssize_t s = send(c->fd, send_buf, chunk, 0);
                        if (s > 0) {
                            c->sent += s;
                        }
                        else if (s < 0 && errno != EAGAIN) {
                            // real error
                            cleanup_client(epfd, c);
                            goto next_event;
                        }
                        else {
                            // EAGAIN: can't send more right now
                            break;
                        }
                    }
                    // If done sending this request:
                    if (c->sent >= c->to_send) {
                        printf("[server] finished fd=%d sent=%ld bytes\n", c->fd, c->sent);
                        // reset header state for next cycle
                        c->hdr_read = 0;
                        c->to_send  = 0;
                        c->sent     = 0;
                        // switch back to EPOLLIN to read a new header
                        struct epoll_event rev = {
                            .events   = EPOLLIN | EPOLLET,
                            .data.ptr = c
                        };
                        epoll_ctl(epfd, EPOLL_CTL_MOD, c->fd, &rev);
                    }
                }
            }

        next_event:;  // jump here to skip remaining checks after cleanup
        }  // for each event
    }  // while(1)
    printf("[server] exiting\n");
    // teardown (never reached in this example)
    close(listen_fd);
    close(epfd);
    free(send_buf);
    return 0;
}
