#include "genet_transport.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include "n64net_rendezvous.h"

static int s_Sock = -1;
static struct sockaddr_in s_Peer, s_Last;
static int s_HavePeer = 0, s_HaveLast = 0;
static char s_Error[256];

const char *gnt_error(void) { return s_Error; }

unsigned long long gnt_now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long long)ts.tv_sec * 1000000ull + (unsigned long long)ts.tv_nsec / 1000ull;
}

void gnt_sleep_us(unsigned us)
{
    struct timespec ts = { (time_t)(us / 1000000u), (long)(us % 1000000u) * 1000 };
    nanosleep(&ts, NULL);
}

int gnt_open(int port)
{
    struct sockaddr_in local;
    s_Sock = socket(AF_INET, SOCK_DGRAM, 0);
    memset(&local, 0, sizeof(local));
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    local.sin_port = htons((unsigned short)port);
    if (s_Sock < 0 || bind(s_Sock, (struct sockaddr *)&local, sizeof(local)) < 0) {
        snprintf(s_Error, sizeof(s_Error), "can't use UDP port %d: %s", port, strerror(errno));
        return -1;
    }
    fcntl(s_Sock, F_SETFL, fcntl(s_Sock, F_GETFL, 0) | O_NONBLOCK);
    return 0;
}

int gnt_set_peer(const char *hostport, int defport)
{
    char host[128];
    const char *colon = strrchr(hostport, ':');
    snprintf(host, sizeof(host), "%.*s", colon ? (int)(colon - hostport) : (int)strlen(hostport), hostport);
    memset(&s_Peer, 0, sizeof(s_Peer));
    s_Peer.sin_family = AF_INET;
    s_Peer.sin_port = htons((unsigned short)(colon ? atoi(colon + 1) : defport));
    if (inet_pton(AF_INET, host, &s_Peer.sin_addr) != 1) {
        snprintf(s_Error, sizeof(s_Error), "the peer must be an IPv4 address[:port], not \"%s\"", hostport);
        return -1;
    }
    s_HavePeer = 1;
    return 0;
}

int gnt_have_peer(void) { return s_HavePeer; }

void gnt_accept_last(void)
{
    if (s_HaveLast) {
        s_Peer = s_Last;
        s_HavePeer = 1;
    }
}

int gnt_rendezvous(const char *server, const char *game, const char *room, const char *password,
                   int is_host, int relay, int wait_secs, void (*log)(const char *fmt, ...),
                   char *room_out, unsigned room_cap)
{
    N64NetRendezvousConfig rc;
    N64NetRendezvousResult res;
    memset(&rc, 0, sizeof(rc));
    memset(&res, 0, sizeof(res));
    rc.server = server;
    rc.game = game;
    rc.room = room;
    rc.password = password;
    rc.is_host = is_host;
    rc.force_relay = relay;
    rc.wait_secs = wait_secs;
    rc.log = log;
    if (n64net_rendezvous_connect(s_Sock, &rc, &res) != 0) {
        snprintf(s_Error, sizeof(s_Error), "couldn't connect through the room server (%s)", res.error);
        return -1;
    }
    s_Peer = res.peer;
    s_HavePeer = 1;
    if (room_out && room_cap) {
        snprintf(room_out, room_cap, "%s%s", res.room, res.relayed ? " (relayed)" : "");
    }
    return 0;
}

/* ---- simulated bad link (test aid) ------------------------------------ */

#define SIM_QUEUE 1024
typedef struct {
    unsigned long long at;
    unsigned size;
    unsigned char data[1500];
} SimPacket;
static SimPacket *s_Sim = NULL;
static int s_SimHead = 0, s_SimCount = 0;
static unsigned s_SimLatency = 0, s_SimJitter = 0, s_SimLoss = 0, s_SimRand = 12345;

static unsigned simRand(void)
{
    s_SimRand = s_SimRand * 1103515245u + 12345u;
    return s_SimRand >> 8;
}

void gnt_sim(unsigned latency_ms, unsigned jitter_ms, unsigned loss_pct)
{
    s_SimLatency = latency_ms;
    s_SimJitter = jitter_ms;
    s_SimLoss = loss_pct;
    if ((latency_ms || jitter_ms || loss_pct) && !s_Sim) {
        s_Sim = calloc(SIM_QUEUE, sizeof(SimPacket));
    }
}

static void rawSend(const void *data, unsigned size)
{
    if (s_Sock >= 0 && s_HavePeer) {
        sendto(s_Sock, data, size, 0, (struct sockaddr *)&s_Peer, sizeof(s_Peer));
    }
}

static void simFlush(void)
{
    unsigned long long now = gnt_now_us();
    int i;
    for (i = 0; i < s_SimCount;) {
        SimPacket *p = &s_Sim[(s_SimHead + i) % SIM_QUEUE];
        if (p->at <= now) {
            rawSend(p->data, p->size);
            *p = s_Sim[s_SimHead];
            s_SimHead = (s_SimHead + 1) % SIM_QUEUE;
            s_SimCount--;
        } else {
            i++;
        }
    }
}

void gnt_send(const void *data, unsigned size)
{
    SimPacket *p;
    if (!s_Sim) {
        rawSend(data, size);
        return;
    }
    if (s_SimLoss && simRand() % 100 < s_SimLoss) {
        return;
    }
    if (s_SimCount >= SIM_QUEUE || size > sizeof(p->data)) {
        rawSend(data, size);
        return;
    }
    p = &s_Sim[(s_SimHead + s_SimCount) % SIM_QUEUE];
    p->at = gnt_now_us() + (unsigned long long)s_SimLatency * 1000 +
            (s_SimJitter ? (unsigned long long)(simRand() % (s_SimJitter * 1000)) : 0);
    p->size = size;
    memcpy(p->data, data, size);
    s_SimCount++;
}

int gnt_recv(void *buf, unsigned cap)
{
    struct sockaddr_in from;
    socklen_t fromlen = sizeof(from);
    ssize_t n;
    if (s_SimCount) {
        simFlush();
    }
    if (s_Sock < 0) {
        return 0;
    }
    memset(&from, 0, sizeof(from));
    n = recvfrom(s_Sock, buf, cap, 0, (struct sockaddr *)&from, &fromlen);
    if (n <= 0) {
        return 0;
    }
    s_Last = from;
    s_HaveLast = 1;
    return (int)n;
}

void gnt_close(void)
{
    if (s_Sock >= 0) {
        close(s_Sock);
        s_Sock = -1;
    }
}
