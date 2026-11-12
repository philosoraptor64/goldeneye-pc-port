/* UDP transport for the netplay glue (port/src/genet.c). Compiled with the
 * host's headers (cmake/n64net), so the game's include/ shadows (string.h,
 * stdlib.h, ...) never meet the socket headers. One socket, one peer. */
#ifndef GENET_TRANSPORT_H
#define GENET_TRANSPORT_H

#ifdef __cplusplus
extern "C" {
#endif

int gnt_open(int port);                               /* bind UDP <port> (0 = any); 0 or -1 (gnt_error) */
int gnt_set_peer(const char *hostport, int defport);  /* "a.b.c.d[:port]"; 0 or -1 */
int gnt_have_peer(void);
void gnt_accept_last(void);                           /* the sender of the last datagram becomes the peer */
int gnt_rendezvous(const char *server, const char *game, const char *room, const char *password,
                   int is_host, int relay, int wait_secs, void (*log)(const char *fmt, ...),
                   char *room_out, unsigned room_cap);
void gnt_send(const void *data, unsigned size);       /* to the peer (through the simulated link if set) */
int gnt_recv(void *buf, unsigned cap);                /* one datagram (0 = none) */
void gnt_close(void);
const char *gnt_error(void);

void gnt_sim(unsigned latency_ms, unsigned jitter_ms, unsigned loss_pct); /* test aid: a bad link */

unsigned long long gnt_now_us(void);
void gnt_sleep_us(unsigned us);

#ifdef __cplusplus
}
#endif
#endif
