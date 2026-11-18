// The host side of the serial link: a TCP port on localhost that carries the
// same bytes the real UART carries. One client at a time; a new connection
// replaces the old one.
//
// Every time the client changes (it closes, a read or write on it fails, a
// new one connects or replaces it) the function given to link_on_change is
// called, so that whatever belonged to the old connection can be dropped.
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>

#include "emu.h"

static int g_listen = -1, g_client = -1;
static link_change_cb_t g_on_change;

void link_on_change(link_change_cb_t cb)
{
    g_on_change = cb;
}

static void changed(void)
{
    if (g_on_change)
        g_on_change(g_client >= 0);
}

// Without O_NONBLOCK an accept or a read would stop the simulation for good,
// so a socket that cannot be made non-blocking is not used.
static int set_nonblock(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    return flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0 ? -1 : 0;
}

int link_listen(int port)
{
    g_listen = socket(AF_INET, SOCK_STREAM, 0);
    if (g_listen < 0)
        return -1;
    int one = 1;
    // Not checked: without SO_REUSEADDR the only cost is that the port may be
    // refused for a moment after an earlier run, and bind() reports that.
    setsockopt(g_listen, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = htons((uint16_t) port);         // main() has checked 1..65535
    if (bind(g_listen, (struct sockaddr *) &a, sizeof(a)) < 0 || listen(g_listen, 4) < 0 ||
        set_nonblock(g_listen) < 0) {
        int e = errno;
        close(g_listen);
        g_listen = -1;
        errno = e;
        return -1;
    }
    return 0;
}

static void try_accept(void)
{
    if (g_listen < 0)
        return;
    int fd = accept(g_listen, NULL, NULL);
    if (fd < 0)
        return;                                  // nobody is waiting (or it gave up already)
    if (set_nonblock(fd) < 0) {
        close(fd);
        return;
    }
    if (g_client >= 0)
        close(g_client);
    g_client = fd;
    int one = 1;
    // Not checked: these only affect how promptly small writes leave and
    // whether a write to a closed client raises SIGPIPE, which main() ignores.
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
#ifdef SO_NOSIGPIPE
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
    changed();
}

int link_connected(void)
{
    return g_client >= 0;
}

int link_wait_client(int timeout_ms)
{
    if (g_listen < 0)
        return 0;
    try_accept();
    if (g_client < 0) {
        struct pollfd p = { .fd = g_listen, .events = POLLIN };
        if (poll(&p, 1, timeout_ms) > 0)
            try_accept();
    }
    return g_client >= 0;
}

static void drop_client(void)
{
    if (g_client < 0)
        return;
    close(g_client);
    g_client = -1;
    changed();
}

void link_send(uint8_t byte)
{
    if (g_client < 0)
        return;
    ssize_t n;
    do {
        n = send(g_client, &byte, 1, 0);
    } while (n < 0 && errno == EINTR);
    if (n == 1)
        return;
    // One byte that does not fit means the client has not read some 100 000
    // requests; like any other failure, that client is gone.
    drop_client();
}

size_t link_read(uint8_t *buf, size_t cap, int timeout_ms)
{
    try_accept();
    if (g_client < 0)
        return 0;
    if (timeout_ms > 0) {
        // The listening socket is watched too, so that a client that connects
        // while the clock is held for the old one replaces it at once.
        struct pollfd p[2] = { { .fd = g_client, .events = POLLIN },
                               { .fd = g_listen, .events = POLLIN } };
        if (poll(p, g_listen >= 0 ? 2 : 1, timeout_ms) <= 0)
            return 0;
        if (g_listen >= 0 && (p[1].revents & POLLIN)) {
            try_accept();
            if (g_client < 0)
                return 0;
        }
    }
    ssize_t n = recv(g_client, buf, cap, 0);
    if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) {
        drop_client();
        return 0;
    }
    return n > 0 ? (size_t) n : 0;
}

void link_close(void)
{
    if (g_client >= 0)
        close(g_client);
    g_client = -1;
    if (g_listen >= 0)
        close(g_listen);
    g_listen = -1;
}
