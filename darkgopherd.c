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

#define _GNU_SOURCE

#include <arpa/inet.h>   // inet_ntoa
#include <assert.h>      // assert
#include <dirent.h>      // opendir
#include <err.h>         // err
#include <errno.h>       // errno
#include <fcntl.h>       // fcntl
#include <limits.h>      // LLONG_MAX
#include <netinet/in.h>  // sockaddr_in
#include <signal.h>      // signal
#include <stdarg.h>      // va_start
#include <stdint.h>      // uint16_t
#include <stdio.h>       // printf
#include <stdlib.h>      // free
#include <string.h>      // strdup
#include <sys/select.h>  // select
#include <sys/socket.h>  // socket
#include <sys/stat.h>    // stat
#include <time.h>        // time
#include <unistd.h>      // close

#if defined(__GNUC__)
#define unused __attribute__((__unused__))
/* Borrowed from FreeBSD's src/sys/sys/cdefs.h,v 1.102.2.2.2.1 */
#define __printflike(fmtarg, firstvararg) \
    __attribute__((__format__(__printf__, fmtarg, firstvararg)))
#else
#define unused
#define __printflike(fmtarg, firstvararg)
#endif

#define llu(x) ((unsigned long long)(x))

#ifndef HOST_NAME_MAX
#define HOST_NAME_MAX 64
#endif

#define DOMAIN_NAME_MAX 256

#define MAX_CONNECTIONS 64
#define MAX_REQUEST_LENGTH 1024

static const size_t max_size_t = ~((size_t)0);

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

static volatile int running = 0; /* Set by signal handler to stop running. */
static int listen_fd = -1;
static uint16_t port = 7070;
static char *fqdn = NULL;
static struct connection **connections;
static int num_conn = 0;
static uint64_t num_requests = 0, total_in = 0, total_out = 0;
static char *gopher_root = NULL;

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

/* strdup() that dies if it can't allocate. */
static char *xstrdup(const char *src) {
    char *out = strdup(src);
    if (out == NULL) errx(1, "can't strdup, out of memory");
    return out;
}

/* vasprintf() that dies if it fails. */
static unsigned int __printflike(2, 0)
    xvasprintf(char **ret, const char *format, va_list ap) {
    int len = vasprintf(ret, format, ap);
    if (ret == NULL || len == -1) errx(1, "out of memory in vasprintf()");
    return (unsigned int)len;
}

/* asprintf() that dies if it fails. */
static unsigned int __printflike(2, 3)
    xasprintf(char **ret, const char *format, ...) {
    va_list va;
    unsigned int len;

    va_start(va, format);
    len = xvasprintf(ret, format, va);
    va_end(va);
    return len;
}

/* Append buffer code. A somewhat efficient string buffer with pool-based
 * reallocation. */
#define APBUF_INIT 4096
#define APBUF_GROW APBUF_INIT
struct apbuf {
    size_t length, pool;
    char *str;
};

static struct apbuf *make_apbuf(void) {
    struct apbuf *buf = xmalloc(sizeof(struct apbuf));
    buf->length = 0;
    buf->pool = APBUF_INIT;
    buf->str = xmalloc(buf->pool);
    return buf;
}

/* Append s (of length len) to buf. */
static void appendl(struct apbuf *buf, const char *s, const size_t len) {
    size_t need = buf->length + len;
    if (buf->pool < need) {
        /* pool has dried up */
        while (buf->pool < need) buf->pool += APBUF_GROW;
        buf->str = xrealloc(buf->str, buf->pool);
    }
    memcpy(buf->str + buf->length, s, len);
    buf->length += len;
}

#ifdef __GNUC__
#define append(buf, s) \
    appendl(buf, s, (__builtin_constant_p(s) ? sizeof(s) - 1 : strlen(s)))
#else
static void append(struct apbuf *buf, const char *s) {
    appendl(buf, s, strlen(s));
}
#endif

static void __printflike(2, 3)
    appendf(struct apbuf *buf, const char *format, ...) {
    char *tmp;
    va_list va;
    size_t len;

    va_start(va, format);
    len = xvasprintf(&tmp, format, va);
    va_end(va);
    appendl(buf, tmp, len);
    free(tmp);
}

/* Returns 1 if string is a number, 0 otherwise.  Set num to NULL if
 * disinterested in value.
 */
static int str_to_num(const char *str, long long *num) {
    char *endptr;
    long long n;

    errno = 0;
    n = strtoll(str, &endptr, 10);
    if (*endptr != '\0') return 0;
    if (n == LLONG_MIN && errno == ERANGE) return 0;
    if (n == LLONG_MAX && errno == ERANGE) return 0;
    if (num != NULL) *num = n;
    return 1;
}

/* Returns a valid number or dies. */
static long long xstr_to_num(const char *str) {
    long long ret;

    if (!str_to_num(str, &ret)) {
        errx(1, "number \"%s\" is invalid", str);
    }
    return ret;
}

static void usage(const char *argv0) {
    printf("usage:\t%s /path/to/gopher_root [flags]\n\n", argv0);
    printf(
        "flags:\t--port number (default: %u, or 70 if running as root)\n"
        "\t\tSpecifies which port to listen on for connections.\n"
        "\t\tPass 0 to let the system choose a free port for you.\n\n",
        port);
    printf(
        "\t--host some.domain.com (default: current hostname)\n"
        "\t\tThe DNS name of this Gopher server.\n\n");
}

static void parse_commandline(const int argc, char *argv[]) {
    int i;
    size_t len;

    if ((argc < 2) || (argc == 2 && strcmp(argv[1], "--help") == 0)) {
        usage(argv[0]); /* No gopher_root given. */
        exit(EXIT_SUCCESS);
    }

    if (getuid() == 0) port = 70;
    gopher_root = xstrdup(argv[1]);

    /* Strip ending slash. */
    len = strlen(gopher_root);
    if (len == 0) errx(1, "/path/to/gopher_root cannot be empty");
    if (len > 1 && gopher_root[len - 1] == '/') gopher_root[len - 1] = '\0';

    /* Walk through the remaining arguments. */
    for (i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--port") == 0) {
            if (++i >= argc) errx(1, "missing number after --port");
            port = (uint16_t)xstr_to_num(argv[i]);
        } else if (strcmp(argv[i], "--host") == 0) {
            if (++i >= argc) errx(1, "missing argument after --host");
            fqdn = xstrdup(argv[i]);
        } else
            errx(1, "unknown argument `%s'", argv[i]);
    }
}

/* Signal handler for SIGTERM etc. */
static void stop_running(int sig unused) { running = 0; }

/* Return the FQDN of the host. Caller must free the result. */
static char *get_fqdn() {
    char hostname[HOST_NAME_MAX + 1];
    char domainname[DOMAIN_NAME_MAX + 1];
    char *fqdn;

    if (gethostname(hostname, HOST_NAME_MAX) == -1) err(1, "gethostname()");
    if (getdomainname(domainname, DOMAIN_NAME_MAX) == -1)
        err(1, "getdomainname()");
    /* Missing domainname. */
    if (domainname[0] == '(') return xstrdup(hostname);
    xasprintf(&fqdn, "%s.%s", hostname, domainname);
    return fqdn;
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
    conn->reply_sent = 0;
    conn->reply_fd = -1;

    assert(num_conn < MAX_CONNECTIONS);
    connections[num_conn++] = conn;
}

static void log_connection(const struct connection *conn) {
    char dest[CLF_DATE_LEN];
    time_t now = time(NULL);

    printf("%s - - %s \"%s\" %llu\n", inet_ntoa(conn->client.sin_addr),
           clf_date(dest, now),
           conn->request == NULL ? "(null)" : conn->request,
           llu(conn->reply_sent));
    fflush(stdout);
}

/* Destructor for connection. */
static void finish_connection(struct connection *conn) {
    log_connection(conn);
    num_requests++;
    xclose(conn->socket);
    assert(conn->state == FINISHED);
    free(conn->request);
    if (conn->reply) free(conn->reply);
    if (conn->reply_fd != -1) xclose(conn->reply_fd);
    free(conn);
}

/* Return 0 if the request (as a path) is unsafe (contains /../) */
static int is_request_safe(const char *req, int len) {
    if (len == 0) return 1;
    if (req[0] != '/') return 0;
    /* Unsafe if it contains ".." */
    if (strstr(req, "/../") != NULL) return 0;
    /* Or ends with it. */
    if (memcmp(req + len - 3, "/..", 3) == 0) return 0;
    /* Or contains a NUL. */
    for (int i = 0; i < len; i++) {
        if (req[i] == '\0') return 0;
    }
    return 1;
}

/* Make the reply an error message. */
static void error_reply(struct connection *conn, const char *msg) {
    xasprintf(&conn->reply, "3%s\terror.host\t1\n", msg);
    conn->reply_type = REPLY_GENERATED;
    conn->reply_length = strlen(conn->reply);
    conn->state = SEND_REPLY;
}

struct dlent {
    char *name; /* The name/path of the entry. */
    int is_dir; /* If the entry is a directory and not a file. */
};

static int dlent_cmp(const void *a, const void *b) {
    if (strcmp((*((const struct dlent *const *)a))->name, "..") == 0) {
        return -1; /* Special-case ".." to come first. */
    }
    return strcmp((*((const struct dlent *const *)a))->name,
                  (*((const struct dlent *const *)b))->name);
}

/* Make sorted list of files in a directory. Returns number of entries, or -1
 * if error occurs. */
static ssize_t make_sorted_dirlist(const char *path, struct dlent ***output) {
    DIR *dir;
    struct dirent *ent;
    size_t entries = 0;
    size_t pool = 128;
    char *currname;
    struct dlent **list = NULL;

    dir = opendir(path);
    if (dir == NULL) {
        warn("opendir(\"%s\")", path);
        return -1;
    }

    currname = xmalloc(strlen(path) + MAXNAMLEN + 1);
    list = xmalloc(sizeof(struct dlent *) * pool);

    while ((ent = readdir(dir)) != NULL) {
        struct stat s;

        /* Skip. */
        if (strcmp(ent->d_name, ".") == 0) continue;
        if (strcmp(ent->d_name, "..") == 0) continue;
        assert(strlen(ent->d_name) <= MAXNAMLEN);
        sprintf(currname, "%s/%s", path, ent->d_name);
        if (stat(currname, &s) == -1) continue; /* Skip un-stat-able files. */
        if (entries == pool) {
            pool *= 2;
            list = xrealloc(list, sizeof(struct dlent *) * pool);
        }
        list[entries] = xmalloc(sizeof(struct dlent));
        list[entries]->name = xstrdup(ent->d_name);
        list[entries]->is_dir = S_ISDIR(s.st_mode);
        entries++;
    }
    closedir(dir);
    free(currname);
    qsort(list, entries, sizeof(struct dlent *), dlent_cmp);
    *output = list;
    return (ssize_t)entries;
}

/* Cleanly deallocate a list of directory files. */
static void cleanup_sorted_dirlist(struct dlent **list, const ssize_t size) {
    for (ssize_t i = 0; i < size; i++) {
        free(list[i]->name);
        free(list[i]);
    }
}

/* Make the reply an index of a directory. */
static void index_reply(struct connection *conn, const char *path) {
    const char divider[] = "i\tfake\t(NULL)\t0\r\n";
    struct apbuf *buf;
    struct dlent **list;
    ssize_t listsize = make_sorted_dirlist(path, &list);
    if (listsize == -1) {
        error_reply(conn, "Internal server error.");
        return;
    }

    /* Heading. */
    buf = make_apbuf();
    append(buf, "i");
    append(buf, conn->request_length == 0 ? "/" : conn->request);
    append(buf, "\tfake\t(NULL)\t0\r\n");
    append(buf, divider);

    /* List of files. */
    for (int i = 0; i < listsize; i++) {
        /* Type. */
        append(buf, list[i]->is_dir ? "1" : "0");
        /* Display name. */
        append(buf, list[i]->name);
        append(buf, "\t");
        /* Path. */
        append(buf, conn->request);
        append(buf, "/");
        append(buf, list[i]->name);
        append(buf, "\t");
        /* Server. */
        append(buf, fqdn);
        append(buf, "\t");
        appendf(buf, "%u\t+\r\n", port);
    }
    cleanup_sorted_dirlist(list, listsize);
    free(list);

    conn->reply = buf->str;
    conn->reply_type = REPLY_GENERATED;
    conn->reply_length = buf->length;
    conn->state = SEND_REPLY;
    free(buf);
}

static void process_request(struct connection *conn) {
    char *path;

    /* Serving a path: check if it's safe. */
    if (!is_request_safe(conn->request, conn->request_length)) {
        conn->state = FINISHED;
        return;
    }

    /* Get path type. */
    xasprintf(&path, "%s%s", gopher_root, conn->request);
    struct stat filestat;
    if (stat(path, &filestat) == -1) {
        if (errno == ENOENT) {
            error_reply(conn, "File not found.");
        } else {
            warn("stat(\"%s\")", path);
            error_reply(conn, "Internal server error.");
        }
        free(path);
        return;
    }

    /* If it's a dir, serve an index. */
    if (S_ISDIR(filestat.st_mode)) {
        index_reply(conn, path);
        free(path);
        return;
    }

    /* Else it's a file. */
    conn->reply_fd = open(path, O_RDONLY | O_NONBLOCK);
    if (conn->reply_fd == -1) {
        warn("open(\"%s\")", path);
        error_reply(conn, "Internal server error.");
        free(path);
        return;
    }
    conn->reply_type = REPLY_FROMFILE;
    conn->state = SEND_REPLY;
    free(path);
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
    ssize_t sent = 0, numread;
    char buf[1 << 15];
    off_t send_len;

    assert(conn->state == SEND_REPLY);
    switch (conn->reply_type) {
        case REPLY_GENERATED:
            /* off_t can be wider than size_t, avoid overflow in send_len. */
            send_len = conn->reply_length - conn->reply_sent;
            if (send_len > max_size_t) send_len = max_size_t;
            sent = send(conn->socket, conn->reply + conn->reply_sent,
                        (size_t)send_len, 0);
            break;

        case REPLY_FROMFILE:
            numread = read(conn->reply_fd, buf, sizeof(buf));
            if (numread <= 0) {
                if (numread == -1) warn("read(fd=%d)", conn->reply_fd);
                /* EOF or read error. */
                conn->state = FINISHED;
                return;
            }
            sent = send(conn->socket, buf, numread, /*flags=*/0);
            break;
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
    assert(is_request_safe("/blah", 5));
    assert(is_request_safe("/", 1));
    assert(is_request_safe("/.", 2));
    assert(!is_request_safe("/..", 3));
    assert(!is_request_safe("/../", 4));

    printf("%s, %s.\n", pkgname, copyright);
    parse_commandline(argc, argv);
    if (fqdn == NULL) fqdn = get_fqdn();

    if (signal(SIGPIPE, SIG_IGN) == SIG_ERR) err(1, "signal(ignore SIGPIPE)");
    if (signal(SIGINT, stop_running) == SIG_ERR) err(1, "signal(SIGINT)");
    if (signal(SIGTERM, stop_running) == SIG_ERR) err(1, "signal(SIGTERM)");

    init_listen_fd();
    connections = xmalloc(MAX_CONNECTIONS * sizeof(*connections));
    running = 1;
    while (running) serve();

    /* Shutdown. */
    xclose(listen_fd);
    free(connections);
    free(fqdn);
    printf("Requests: %llu\n", llu(num_requests));
    printf("Bytes: %llu in, %llu out\n", llu(total_in), llu(total_out));
    return 0;
}
