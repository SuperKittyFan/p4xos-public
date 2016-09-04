#ifndef _CONST_H_
#define _CONST_H_

#include <stdbool.h>

/* number of elements in the mbuf pool */
#define NUM_MBUFS 1023
/* Size of the per-core object cache */
#define MBUF_CACHE_SIZE 250
/* Maximum number of packets in sending or receiving */
#define BURST_SIZE 32

#define RX_RING_SIZE 128
#define TX_RING_SIZE 512

#define NUM_ACCEPTORS 3

#define TIMER_RESOLUTION_CYCLES 20000000ULL /* around 10ms at 2 Ghz */

#define BUFSIZE 1500

#define PROPOSER_PORT    34950
#define COORDINATOR_PORT 34951
#define ACCEPTOR_PORT    34952
#define LEARNER_PORT     34953
volatile bool force_quit;

#endif