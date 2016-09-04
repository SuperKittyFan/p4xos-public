#include <stdint.h>
#include <inttypes.h>
#include <signal.h>

#include <rte_eal.h>
#include <rte_ethdev.h>
/* get cores */
#include <rte_lcore.h>
#include <rte_mbuf.h>
#include <rte_ether.h>
#include <rte_ip.h>
#include <rte_udp.h>
/* dump packet contents */
#include <rte_hexdump.h>
#include <rte_malloc.h>
/* logging */
#include <rte_log.h>
/* timer event */
#include <rte_timer.h>
/* get clock cycles */
#include <rte_cycles.h>

/* libpaxos learner */
#include "learner.h"
/* paxos header definition */
#include "rte_paxos.h"
#include "const.h"
#include "utils.h"

static unsigned nb_ports;

/* learner timer for deliver */
static struct rte_timer deliver_timer;
static struct rte_timer hole_timer;

static const struct rte_eth_conf port_conf_default = {
	.rxmode = { .max_rx_pkt_len = ETHER_MAX_LEN, },
};


static const struct ether_addr ether_multicast = {
	.addr_bytes= { 0x01, 0x1b, 0x19, 0x0, 0x0, 0x0 }
};


static struct {
	uint64_t total_cycles;
	uint64_t total_pkts;
} latency_numbers;


struct rte_mempool *mbuf_pool;

static int
paxos_rx_process(struct rte_mbuf *pkt, struct learner* l)
{
	int ret = 0;
	uint8_t l4_proto = 0;
	uint16_t outer_header_len;
	union tunnel_offload_info info = { .data = 0 };
	struct udp_hdr *udp_hdr;
	struct paxos_hdr *paxos_hdr;
	struct ether_hdr *phdr = rte_pktmbuf_mtod(pkt, struct ether_hdr *);

	parse_ethernet(phdr, &info, &l4_proto);

	if (l4_proto != IPPROTO_UDP)
		return -1;

	udp_hdr = (struct udp_hdr *)((char *)phdr +
			info.outer_l2_len + info.outer_l3_len);

	if (udp_hdr->dst_port != rte_cpu_to_be_16(LEARNER_PORT) &&
			(pkt->packet_type & RTE_PTYPE_TUNNEL_MASK) == 0)
		return -1;

	paxos_hdr = (struct paxos_hdr *)((char *)udp_hdr + sizeof(struct udp_hdr));

	if (rte_get_log_level() == RTE_LOG_DEBUG) {
		rte_hexdump(stdout, "udp", udp_hdr, sizeof(struct udp_hdr));
		rte_hexdump(stdout, "paxos", paxos_hdr, sizeof(struct paxos_hdr));
		print_paxos_hdr(paxos_hdr);
	}

	struct paxos_value *v = paxos_value_new((char *)paxos_hdr->paxosval, 32);
	struct paxos_accepted ack = {
			.iid = rte_be_to_cpu_32(paxos_hdr->inst),
			.ballot = rte_be_to_cpu_16(paxos_hdr->rnd),
			.value_ballot = rte_be_to_cpu_16(paxos_hdr->vrnd),
			.aid = rte_be_to_cpu_16(paxos_hdr->acptid),
			.value = *v };

	learner_receive_accepted(l, &ack);

	outer_header_len = info.outer_l2_len + info.outer_l3_len
		+ sizeof(struct udp_hdr) + sizeof(struct paxos_hdr);

	rte_pktmbuf_adj(pkt, outer_header_len);


	return ret;

}

static uint16_t
add_timestamps(uint8_t port __rte_unused, uint16_t qidx __rte_unused,
		struct rte_mbuf **pkts, uint16_t nb_pkts,
		uint16_t max_pkts __rte_unused, void *user_param)
{
	struct learner* learner = (struct learner *)user_param;
	unsigned i;
	uint64_t now = rte_rdtsc();

	for (i = 0; i < nb_pkts; i++) {
		pkts[i]->udata64 = now;
		paxos_rx_process(pkts[i], learner);
	}


	return nb_pkts;
}


static uint16_t
calc_latency(uint8_t port __rte_unused, uint16_t qidx __rte_unused,
		struct rte_mbuf **pkts, uint16_t nb_pkts, void *_ __rte_unused)
{
	uint64_t cycles = 0;
	uint64_t now = rte_rdtsc();
	unsigned i;

	for (i = 0; i < nb_pkts; i++) {
		cycles += now - pkts[i]->udata64;
		rte_log(RTE_LOG_INFO, RTE_LOGTYPE_USER8,
				"Packet%"PRIu64", Latency = %"PRIu64" cycles\n",
				latency_numbers.total_pkts, cycles);
	}

	latency_numbers.total_cycles += cycles;
	latency_numbers.total_pkts += nb_pkts;

	if (latency_numbers.total_pkts > (100 * 1000 * 1000ULL)) {
		rte_log(RTE_LOG_INFO, RTE_LOGTYPE_USER8,
		"Latency = %"PRIu64" cycles\n",
		latency_numbers.total_cycles / latency_numbers.total_pkts);
		latency_numbers.total_cycles = latency_numbers.total_pkts = 0;
	}

	return nb_pkts;
};

static inline int
port_init(uint8_t port, struct rte_mempool *mbuf_pool, struct learner* learner)
{
	struct rte_eth_dev_info dev_info;
	struct rte_eth_txconf *txconf;
	struct rte_eth_rxconf *rxconf;
	struct rte_eth_conf port_conf = port_conf_default;
	const uint16_t rx_rings = 1, tx_rings = 1;
	int retval;
	uint16_t q;

	rte_eth_dev_info_get(port, &dev_info);

	rxconf = &dev_info.default_rxconf;
	txconf = &dev_info.default_txconf;

	txconf->txq_flags &= PKT_TX_IPV4;
	txconf->txq_flags &= PKT_TX_UDP_CKSUM;
	if (port >= rte_eth_dev_count())
		return -1;

	retval = rte_eth_dev_configure(port, rx_rings, tx_rings, &port_conf);
	if (retval != 0)
		return retval;

	for (q = 0; q < rx_rings; q++) {
		retval = rte_eth_rx_queue_setup(port, q, RX_RING_SIZE,
				rte_eth_dev_socket_id(port), rxconf, mbuf_pool);
		if (retval < 0)
			return retval;
	}

	for (q = 0; q < tx_rings; q++) {
		retval = rte_eth_tx_queue_setup(port, q, TX_RING_SIZE,
				rte_eth_dev_socket_id(port), txconf);
		if (retval < 0)
			return retval;
	}

	retval = rte_eth_dev_start(port);
	if (retval < 0)
		return retval;

	struct ether_addr addr;
	rte_eth_macaddr_get(port, &addr);
	printf("Port %u MAC: %02"PRIx8" %02"PRIx8" %02"PRIx8
			" %02"PRIx8" %02"PRIx8" %02"PRIx8"\n",
			(unsigned) port,
			addr.addr_bytes[0], addr.addr_bytes[1],
			addr.addr_bytes[2], addr.addr_bytes[3],
			addr.addr_bytes[4], addr.addr_bytes[5]);

	rte_eth_promiscuous_enable(port);

	rte_eth_add_rx_callback(port, 0, add_timestamps, learner);
	rte_eth_add_tx_callback(port, 0, calc_latency, NULL);
	return 0;
}


static void
lcore_main(void)
{
	uint8_t port;

	for (port = 0; port < nb_ports; port++)
		if (rte_eth_dev_socket_id(port) > 0 &&
				rte_eth_dev_socket_id(port) !=
					(int) rte_socket_id())
			rte_log(RTE_LOG_WARNING, RTE_LOGTYPE_EAL,
					"WARNING, port %u is on retmote NUMA node to "
					"polling thread.\nPerformance will "
					"not be optimal.\n", port);

	rte_log(RTE_LOG_INFO, RTE_LOGTYPE_EAL, "\nCore %u forwarding packets. [Ctrl+C to quit]\n",
			rte_lcore_id());


	for (;;) {
		// Check if signal is received
		if (force_quit)
			break;

		for (port = 0; port < nb_ports; port++) {
			struct rte_mbuf *bufs[BURST_SIZE];
			const uint16_t nb_rx = rte_eth_rx_burst(port, 0, bufs, BURST_SIZE);
			if (unlikely(nb_rx == 0))
				continue;
			const uint16_t nb_tx = rte_eth_tx_burst(port, 0,
					bufs, nb_rx);
			if (unlikely(nb_tx < nb_rx)) {
				uint16_t buf;

				for (buf = nb_tx; buf < nb_rx; buf++)
					rte_pktmbuf_free(bufs[buf]);
			}
		}
	}
}


static void
check_deliver(struct rte_timer *tim,
		void *arg)
{
	struct learner* l = (struct learner *) arg;
	unsigned lcore_id = rte_lcore_id();

	rte_log(RTE_LOG_DEBUG, RTE_LOGTYPE_USER8, "%s() on lcore_id %i\n", __func__, lcore_id);

	struct paxos_accepted out;
	int consensus = learner_deliver_next(l, &out);
	if (consensus)
		rte_log(RTE_LOG_INFO, RTE_LOGTYPE_USER8, "consensus reached: %d\n", consensus);
	/* this timer is automatically reloaded until we decide to stop it */
	if (force_quit)
		rte_timer_stop(tim);
}


static uint64_t
craft_new_packet(struct rte_mbuf **created_pkt, uint32_t srcIP, uint32_t dstIP,
		uint16_t sport, uint16_t dport, size_t data_size, uint8_t output_port)
{
	uint64_t ol_flags = 0;
	size_t pkt_size = sizeof(struct ether_hdr) + sizeof(struct ipv4_hdr) +
		sizeof(struct udp_hdr) + data_size;
	(*created_pkt)->data_len = pkt_size;
	(*created_pkt)->pkt_len = pkt_size;
	struct ether_hdr *eth;
	eth = rte_pktmbuf_mtod(*created_pkt, struct ether_hdr*);
	/* set packet s_addr using mac address of output port */
	rte_eth_macaddr_get(output_port, &eth->s_addr);
	/* Set multicast address 01-1b-19-00-00-00 */
	ether_addr_copy(&ether_multicast, &eth->d_addr);
	eth->ether_type = rte_cpu_to_be_16(ETHER_TYPE_IPv4);
	struct ipv4_hdr *iph;
	iph = (struct ipv4_hdr *)rte_pktmbuf_mtod_offset(*created_pkt, struct ipv4_hdr*,
			sizeof(struct ether_hdr));
	iph->src_addr = rte_cpu_to_be_32(srcIP);
	iph->dst_addr = rte_cpu_to_be_32(dstIP);
	iph->version_ihl = 0x45;
	iph->hdr_checksum = 0;
	ol_flags |= PKT_TX_IPV4;
	ol_flags |= PKT_TX_IP_CKSUM;
	iph->total_length = rte_cpu_to_be_16( sizeof(struct ipv4_hdr) +
			sizeof(struct udp_hdr) + sizeof(paxos_message));
	iph->next_proto_id = IPPROTO_UDP;
	struct udp_hdr *udp;
	udp = (struct udp_hdr *)((unsigned char*)iph + sizeof(struct ipv4_hdr));
	udp->src_port = rte_cpu_to_be_16(sport);
	udp->dst_port = rte_cpu_to_be_16(dport);
	ol_flags |= PKT_TX_UDP_CKSUM;
	udp->dgram_cksum = get_psd_sum(iph, ETHER_TYPE_IPv4, ol_flags);
	udp->dgram_len = rte_cpu_to_be_16(data_size);
	return ol_flags;
}

static void
send_repeat_message(paxos_message *pm) {
	uint8_t port_id = 0;
	struct rte_mbuf *created_pkt = rte_pktmbuf_alloc(mbuf_pool);
	created_pkt->l2_len = sizeof(struct ether_hdr);
	created_pkt->l3_len = sizeof(struct ipv4_hdr);
	created_pkt->l4_len = sizeof(struct udp_hdr) + sizeof(paxos_message);
	uint64_t ol_flags = craft_new_packet(&created_pkt, IPv4(192,168,4,95), IPv4(239,3,29,73),
			LEARNER_PORT, ACCEPTOR_PORT, sizeof(paxos_message), port_id);
	//struct udp_hdr *udp;
	size_t udp_offset = sizeof(struct ether_hdr) + sizeof(struct ipv4_hdr);
	//udp  = rte_pktmbuf_mtod_offset(created_pkt, struct udp_hdr *, udp_offset);
	size_t paxos_offset = udp_offset + sizeof(struct udp_hdr);
	paxos_message *px = rte_pktmbuf_mtod_offset(created_pkt, paxos_message *, paxos_offset);
	rte_memcpy(px, pm, sizeof(*pm));	
	//udp->dgram_cksum = get_psd_sum(udp, ETHER_TYPE_IPv4, ol_flags);
	created_pkt->ol_flags = ol_flags;
	const uint16_t nb_tx = rte_eth_tx_burst(port_id, 0, &created_pkt, 1);
	rte_pktmbuf_free(created_pkt);
	rte_log(RTE_LOG_DEBUG, RTE_LOGTYPE_USER8, "Send %d repeated messages\n", nb_tx);
}

static void
check_holes(struct rte_timer *tim, void *arg)
{
	struct learner *l = (struct learner *) arg;
	unsigned lcore_id = rte_lcore_id();

	rte_log(RTE_LOG_DEBUG, RTE_LOGTYPE_USER8, "%s() on lcore_id %i\n", __func__, lcore_id);

	paxos_repeat repeat;
	unsigned chunks = 10;
	if (learner_has_holes(l, &repeat.from, &repeat.to)) {
		if ((repeat.to - repeat.from) > chunks)
			repeat.to = repeat.from + chunks;
		rte_log(RTE_LOG_INFO, RTE_LOGTYPE_USER8,
				"Learner has holes from %d to %d\n", repeat.from, repeat.to);
		__attribute__((unused)) paxos_message repeat_msg = {
			.type = PAXOS_REPEAT,
			.u.repeat = repeat,
		};
		send_repeat_message(&repeat_msg);
	}

	/* this timer is automatically reloaded until we decide to stop it */
	if (force_quit)
		rte_timer_stop(tim);
}

static __attribute__((noreturn)) int
lcore_mainloop(__attribute__((unused)) void *arg)
{
	uint64_t prev_tsc = 0, cur_tsc, diff_tsc;
	unsigned lcore_id;

	lcore_id = rte_lcore_id();

	rte_log(RTE_LOG_DEBUG, RTE_LOGTYPE_TIMER,
			"Starting mainloop on core %u\n", lcore_id);

	while(1) {
		cur_tsc = rte_rdtsc();
		diff_tsc = cur_tsc - prev_tsc;
		if (diff_tsc > TIMER_RESOLUTION_CYCLES) {
			rte_timer_manage();
			prev_tsc = cur_tsc;
		}
	}
}

int
main(int argc, char *argv[])
{
	uint8_t portid = 0;
	unsigned master_core, lcore_id;
	signal(SIGTERM, signal_handler);
	signal(SIGINT, signal_handler);
	force_quit = false;

	/* init EAL */
	int ret = rte_eal_init(argc, argv);

	if (ret < 0)
		rte_exit(EXIT_FAILURE, "Error with EAL initialization\n");


	nb_ports = rte_eth_dev_count();

	mbuf_pool = rte_pktmbuf_pool_create("MBUF_POOL",
			NUM_MBUFS * nb_ports, MBUF_CACHE_SIZE, 0,
			RTE_MBUF_DEFAULT_BUF_SIZE, rte_socket_id());

	if (mbuf_pool == NULL)
		rte_exit(EXIT_FAILURE, "Cannot create mbuf_pool\n");

	//initialize learner
	struct learner *learner = learner_new(NUM_ACCEPTORS);
	learner_set_instance_id(learner, 0);

	/* init RTE timer library */
	rte_timer_subsystem_init();

	/* init timer structure */
	rte_timer_init(&deliver_timer);
	rte_timer_init(&hole_timer);

	/* load deliver_timer, every second, on a slave lcore, reloaded automatically */
	uint64_t hz = rte_get_timer_hz();
	/* master core */
	master_core = rte_lcore_id();
	/* slave core */
	lcore_id = rte_get_next_lcore(master_core, 0, 1);
	rte_log(RTE_LOG_DEBUG, RTE_LOGTYPE_USER1, "lcore_id: %d\n", lcore_id);
	rte_timer_reset(&deliver_timer, hz, PERIODICAL, lcore_id, check_deliver, learner);

	/* slave core */
	lcore_id = rte_get_next_lcore(lcore_id, 0, 1);
	rte_log(RTE_LOG_DEBUG, RTE_LOGTYPE_USER1, "lcore_id: %d\n", lcore_id);
	rte_timer_reset(&hole_timer, hz, PERIODICAL, lcore_id, check_holes, learner);

	for (portid = 0; portid < nb_ports; portid++)
		if (port_init(portid, mbuf_pool, learner) != 0)
			rte_exit(EXIT_FAILURE, "Cannot init port %"PRIu8"\n", portid);


	/* start mainloop on every lcore */
	RTE_LCORE_FOREACH_SLAVE(lcore_id) {
		rte_eal_remote_launch(lcore_mainloop, NULL, lcore_id);
	}

	lcore_main();

	rte_log(RTE_LOG_DEBUG, RTE_LOGTYPE_USER1, "free learner\n");
	learner_free(learner);
	return 0;
}

