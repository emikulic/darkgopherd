/* darkgopherd - a simple, single-threaded gopher server.
 * Copyright (c) 2024 Emil Mikulic <emikulic@gmail.com>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the
 * above copyright notice and this permission notice appear in all
 * copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL
 * WARRANTIES WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE
 * AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL
 * DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR
 * PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER
 * TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
 * PERFORMANCE OF THIS SOFTWARE.
 */

static const char pkgname[] = "darkgopherd/0.0.from.git",
                  copyright[] = "copyright (c) 2024 Emil Mikulic";

#include <arpa/inet.h>   // inet_ntoa
#include <assert.h>      // assert
#include <err.h>         // err
#include <errno.h>       // errno
#include <fcntl.h>       // fcntl
#include <netinet/in.h>  // sockaddr_in
#include <signal.h>      // signal
#include <stdint.h>      // uint16_t
#include <stdio.h>       // printf
#include <stdlib.h>      // free
#include <string.h>      // strdup
#include <sys/select.h>  // select
#include <sys/socket.h>  // socket
#include <time.h>        // time
#include <unistd.h>      // close

#if defined(__GNUC__)
#define unused __attribute__((__unused__))
#else
#define unused
#endif

#ifndef HOST_NAME_MAX
#define HOST_NAME_MAX 64
#endif

#define DOMAIN_NAME_MAX 256

#define MAX_CONNECTIONS 64
#define MAX_REQUEST_LENGTH 1024

struct connection {
    int socket;
    struct sockaddr_in client;
    // time_t last_active;
    enum {
        RECV_REQUEST, /* Receiving request. */
        SEND_REPLY,   /* Sending reply. */
        FINISHED      /* Finished, needs to be closed and cleaned up. */
    } state;

    char *request; /* Null-terminated. */
    size_t request_length;

    enum { REPLY_GENERATED, REPLY_FROMFILE } reply_type;
    char *reply;
    off_t reply_length, reply_sent;
    int reply_fd;
};

static volatile int running = 0; /* Set by signal handler. */
static int listen_fd = -1;
static uint16_t port = 7070;  // TODO: Make this configurable.
static char *fqdn = NULL;
static struct connection **connections;
static int num_conn = 0;
static uint64_t num_requests = 0, total_in = 0, total_out = 0;

/* close() that dies on error. */
static void xclose(const int fd) {
    if (close(fd) == -1) err(1, "close()");
}

/* malloc() that dies if it can't allocate. */
static void *xmalloc(const size_t size) {
    void *ptr = malloc(size);
    if (ptr == NULL) errx(1, "can't allocate %zu bytes", size);
    return ptr;
}

/* realloc() that dies if it can't reallocate. */
static void *xrealloc(void *original, const size_t size) {
    void *ptr = realloc(original, size);
    if (ptr == NULL) errx(1, "can't reallocate %zu bytes", size);
    return ptr;
}

/* Signal handler for SIGTERM etc. */
static void stop_running(int sig unused) { running = 0; }

/* Return the FQDN of the host. Caller must free the result. */
static char *get_fqdn() {
    char hostname[HOST_NAME_MAX + 1];
    char domainname[DOMAIN_NAME_MAX + 1];
    char fqdn[HOST_NAME_MAX + DOMAIN_NAME_MAX + 3];

    if (gethostname(hostname, HOST_NAME_MAX) == -1) err(1, "gethostname()");
    if (getdomainname(domainname, DOMAIN_NAME_MAX) == -1)
        err(1, "getdomainname()");
    if (domainname[0] == '(') return strdup(hostname);
    snprintf(fqdn, sizeof(fqdn) - 1, "%s.%s", hostname, domainname);
    return strdup(fqdn);
}

/* Format [when] as a CLF date format, stored in the specified buffer. The same
 * buffer is returned for convenience. */
#define CLF_DATE_LEN 29 /* strlen("[10/Oct/2000:13:55:36 -0700]")+1 */
static char *clf_date(char *dest, const time_t when) {
    time_t when_copy = when;
    struct tm tm;
    localtime_r(&when_copy, &tm);
    if (strftime(dest, CLF_DATE_LEN, "[%d/%b/%Y:%H:%M:%S %z]", &tm) == 0) {
        dest[0] = 0;
    }
    return dest;
}

/* Make the specified socket non-blocking. */
static void nonblock_socket(const int sock) {
    int flags = fcntl(sock, F_GETFL);
    if (flags == -1) err(1, "fcntl(F_GETFL)");
    flags |= O_NONBLOCK;
    if (fcntl(sock, F_SETFL, flags) == -1) err(1, "fcntl() to set O_NONBLOCK");
}

/* Bind, and listen on listen_fd. */
static void init_listen_fd() {
    int sockopt;
    struct sockaddr_in addrin;
    socklen_t addrin_len;

    /* Create listening socket. */
    listen_fd = socket(PF_INET, SOCK_STREAM, 0);
    if (listen_fd == -1) err(1, "socket()");

    /* "Reuse address" so we can start the server even if there are
     * connections in TIME_WAIT state. */
    sockopt = 1;
    if (setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &sockopt,
                   sizeof(sockopt)) == -1)
        err(1, "setsockopt(SO_REUSEADDR)");

    /* Bind to port. */
    memset(&addrin, 0, sizeof(addrin));
    addrin.sin_family = AF_INET;
    addrin.sin_port = htons(port);
    addrin.sin_addr.s_addr = INADDR_ANY;
    if (bind(listen_fd, (struct sockaddr *)&addrin,
             sizeof(struct sockaddr_in)) == -1)
        err(1, "bind(port=%d)", port);

    /* Get IP address. */
    addrin_len = sizeof(addrin);
    if (getsockname(listen_fd, (struct sockaddr *)&addrin, &addrin_len) == -1)
        err(1, "getsockname()");
    printf("Listening on gopher://%s:%u/ bound to IP %s\n", fqdn,
           ntohs(addrin.sin_port), inet_ntoa(addrin.sin_addr));

    if (listen(listen_fd, /*backlog=*/128) == -1) err(1, "listen()");
}

static void accept_connection() {
    socklen_t sin_size;
    struct sockaddr_in addrin;
    struct connection *conn;
    int fd;

    sin_size = sizeof(addrin);
    memset(&addrin, 0, sin_size);
    fd = accept(listen_fd, (struct sockaddr *)&addrin, &sin_size);
    if (fd == -1) {
        warn("accept()");
        return;
    }
    nonblock_socket(fd);
    conn = xmalloc(sizeof(*conn));
    conn->socket = fd;
    conn->client = addrin;
    conn->state = RECV_REQUEST;
    conn->request = NULL;
    conn->request_length = 0;
    conn->reply = NULL;
    conn->reply_fd = -1;

    assert(num_conn < MAX_CONNECTIONS);
    connections[num_conn++] = conn;
}

static void log_connection(const struct connection *conn) {
    char dest[CLF_DATE_LEN];
    time_t now = time(NULL);
#define llu(x) ((unsigned long long)(x))

    printf("%s - - %s \"%s\" %llu\n", inet_ntoa(conn->client.sin_addr),
           clf_date(dest, now),
           conn->request == NULL ? "(null)" : conn->request,
           llu(conn->reply_sent));
    fflush(stdout);
}

/* Destructor for connection. */
static void finish_connection(struct connection *conn) {
    log_connection(conn);
    xclose(conn->socket);
    assert(conn->state == FINISHED);
    free(conn->request);
    if (conn->reply) free(conn->reply);
    free(conn);
}

static void process_request(struct connection *conn) {
    // TODO: Actually serve requests.
    conn->reply = strdup("iHello, world\tfake\t(NULL)\t0\n");
    conn->reply_type = REPLY_GENERATED;
    conn->reply_length = strlen(conn->reply);
    conn->reply_sent = 0;
    conn->state = SEND_REPLY;
    num_requests++;
}

/* Remove \r\n or \n suffix. */
static void chomp(char *s, size_t *len) {
    if (*len == 0) return;
    if (s[*len - 1] == '\n') s[--*len] = 0;
    if (*len == 0) return;
    if (s[*len - 1] == '\r') s[--*len] = 0;
}

static void recv_request(struct connection *conn) {
    char buf[MAX_REQUEST_LENGTH];
    ssize_t recvd;

    assert(conn->state == RECV_REQUEST);
    recvd = recv(conn->socket, buf, sizeof(buf), 0);
    if (recvd < 1) {
        if (recvd == -1) {
            if (errno == EAGAIN) {
                // read() would have blocked.
                return;
            }
            warn("recv(fd=%d)", conn->socket);
        }
        conn->state = FINISHED;
        return;
    }
    // TODO: Timeouts: conn->last_active = now;

    /* Append to conn->request. */
    conn->request =
        xrealloc(conn->request, conn->request_length + (size_t)recvd + 1);
    memcpy(conn->request + conn->request_length, buf, (size_t)recvd);
    conn->request_length += (size_t)recvd;
    conn->request[conn->request_length] = 0;
    total_in += (size_t)recvd;

    /* Process request if we have all of it. */
    if (conn->request_length > 0 &&
        conn->request[conn->request_length - 1] == '\n') {
        chomp(conn->request, &conn->request_length);
        process_request(conn);
        return;
    }

    /* Die if it's too large. */
    if (conn->request_length > MAX_REQUEST_LENGTH) conn->state = FINISHED;
}

static void send_reply(struct connection *conn) {
    ssize_t sent;
    /* off_t can be wider than size_t, avoid overflow in send_len. */
    const size_t max_size_t = ~((size_t)0);
    off_t send_len = conn->reply_length - conn->reply_sent;
    if (send_len > max_size_t) send_len = max_size_t;

    assert(conn->state == SEND_REPLY);
    if (conn->reply_type == REPLY_GENERATED) {
        sent = send(conn->socket, conn->reply + conn->reply_sent,
                    (size_t)send_len, 0);
    } else {
        // TODO
        return;
    }
    // conn->last_active = now;

    /* Handle any errors (-1) or closure (0) in send(). */
    if (sent < 1) {
        if (sent == -1) {
            if (errno == EAGAIN) return;  // send() would have blocked.
            warn("send(fd=%d)", conn->socket);
        }
        // Else sent == 0 because remote end closed connection.
        conn->state = FINISHED;
        return;
    }
    conn->reply_sent += sent;
    total_out += (size_t)sent;

    /* Check if we're finished sending. */
    if (conn->reply_sent == conn->reply_length) conn->state = FINISHED;
}

/* Main loop. */
static void serve() {
    fd_set recv_set, send_set;
    int max_fd = 0;
    int select_ret;
    int need_timeout = 0;

    /* Populate FD sets. */
#define FD_SET_MAX(sock, fdset)                   \
    do {                                          \
        FD_SET(sock, fdset);                      \
        max_fd = (max_fd < sock) ? sock : max_fd; \
    } while (0)

    FD_ZERO(&recv_set);
    FD_ZERO(&send_set);
    if (num_conn < MAX_CONNECTIONS) FD_SET_MAX(listen_fd, &recv_set);

    for (int i = 0; i < num_conn; i++) {
        struct connection *c = connections[i];
        switch (c->state) {
            case RECV_REQUEST:
                FD_SET_MAX(c->socket, &recv_set);
                // need_timeout = 1;
                break;

            case SEND_REPLY:
                FD_SET_MAX(c->socket, &send_set);
                // need_timeout = 1;
                break;

            case FINISHED:
                break; /* Handled later. */
        }
    }

    /* Select. */
    select_ret = select(max_fd + 1, &recv_set, &send_set, NULL, NULL);
    if (select_ret == 0 && !need_timeout) errx(1, "select() timed out");
    if (select_ret == -1) {
        if (errno == EINTR)
            return; /* Interrupted by signal, retry. */
        else
            err(1, "select() failed");
    }

    /* Act on select. */
    if (FD_ISSET(listen_fd, &recv_set)) accept_connection();

    for (int i = 0; i < num_conn; i++) {
        struct connection *conn = connections[i];
        switch (conn->state) {
            case RECV_REQUEST:
                if (FD_ISSET(conn->socket, &recv_set)) recv_request(conn);
                break;

            case SEND_REPLY:
                if (FD_ISSET(conn->socket, &send_set)) send_reply(conn);
                break;

            case FINISHED:
                break; /* Handled later. */
        }
    }

    /* Clean up finished connections. */
    for (int i = 0; i < num_conn; i++) {
        struct connection *conn = connections[i];
        if (conn->state == FINISHED) {
            finish_connection(conn);
            assert(num_conn > 0);
            connections[i] = connections[--num_conn];
        }
    }
}

int main(int argc, char **argv) {
    printf("%s, %s.\n", pkgname, copyright);

    if (signal(SIGPIPE, SIG_IGN) == SIG_ERR) err(1, "signal(ignore SIGPIPE)");
    if (signal(SIGINT, stop_running) == SIG_ERR) err(1, "signal(SIGINT)");
    if (signal(SIGTERM, stop_running) == SIG_ERR) err(1, "signal(SIGTERM)");

    fqdn = get_fqdn();
    init_listen_fd();
    connections = xmalloc(MAX_CONNECTIONS * sizeof(*connections));
    running = 1;
    while (running) serve();

    /* Shutdown. */
    xclose(listen_fd);
    free(connections);
    free(fqdn);
    return 0;
}
