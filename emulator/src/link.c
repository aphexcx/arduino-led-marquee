// The host side of the serial link: a TCP port on localhost that carries the
// same bytes the real UART carries. One client at a time; a new connection
// replaces the old one.
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

static void set_nonblock(int fd)
{
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
}

int link_listen(int port)
{
    g_listen = socket(AF_INET, SOCK_STREAM, 0);
    if (g_listen < 0)
        return -1;
    int one = 1;
    setsockopt(g_listen, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = htons((uint16_t) port);
    if (bind(g_listen, (struct sockaddr *) &a, sizeof(a)) < 0 || listen(g_listen, 4) < 0) {
        close(g_listen);
        g_listen = -1;
        return -1;
    }
    set_nonblock(g_listen);
    return 0;
}

static void try_accept(void)
{
    if (g_listen < 0)
        return;
    int fd = accept(g_listen, NULL, NULL);
    if (fd < 0)
        return;
    if (g_client >= 0)
        close(g_client);
    g_client = fd;
    set_nonblock(fd);
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
#ifdef SO_NOSIGPIPE
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
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
    if (g_client >= 0)
        close(g_client);
    g_client = -1;
}

void link_send(uint8_t byte)
{
    if (g_client < 0)
        return;
    if (send(g_client, &byte, 1, 0) < 0 && errno != EAGAIN && errno != EWOULDBLOCK)
        drop_client();
}

size_t link_read(uint8_t *buf, size_t cap, int timeout_ms)
{
    try_accept();
    if (g_client < 0)
        return 0;
    if (timeout_ms > 0) {
        struct pollfd p = { .fd = g_client, .events = POLLIN };
        if (poll(&p, 1, timeout_ms) <= 0)
            return 0;
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
    drop_client();
    if (g_listen >= 0)
        close(g_listen);
    g_listen = -1;
}
