#include <inttypes.h>
#include <rte_cycles.h>
#include <rte_eal.h>
#include <rte_ethdev.h>
#include <rte_lcore.h>
#include <rte_log.h>
#include <rte_malloc.h>
#include <rte_mbuf.h>
#include <signal.h>
#include <stdint.h>
#include <stdlib.h>

#define REQUIRE_CORES 4
#define RX_RING_SIZE  1024
#define TX_RING_SIZE  1024

#define NUM_MBUFS       8191
#define MBUF_CACHE_SIZE 250
#define BURST_SIZE      32

#define RING_SIZE 16384
struct rte_ring *rx_ring;
struct rte_ring *tx_ring;

#define RTE_LOGTYPE_TEST RTE_LOGTYPE_USER1

static volatile bool force_quit;

/* meta data */
struct meta {
	uint64_t tsc;
};

/* mbuf with meta data */
struct mbuf_meta {
	struct rte_mbuf *buf;
	struct meta      meta;
} __rte_cache_aligned;

static inline int port_init(uint16_t port, struct rte_mempool *mbuf_pool) {
	struct rte_eth_conf     port_conf;
	const uint16_t          rx_rings = 1, tx_rings = 1;
	uint16_t                nb_rxd = RX_RING_SIZE;
	uint16_t                nb_txd = TX_RING_SIZE;
	int                     retval;
	uint16_t                q;
	struct rte_eth_dev_info dev_info;
	struct rte_eth_txconf   txconf;

	if (!rte_eth_dev_is_valid_port(port))
		return -1;

	memset(&port_conf, 0, sizeof(struct rte_eth_conf));

	retval = rte_eth_dev_info_get(port, &dev_info);
	if (retval != 0) {
		RTE_LOG(ERR, TEST, "Error during getting device (port %u) info: %s\n",
		        port, strerror(-retval));
		return retval;
	}

	if (dev_info.tx_offload_capa & RTE_ETH_TX_OFFLOAD_MBUF_FAST_FREE)
		port_conf.txmode.offloads |= RTE_ETH_TX_OFFLOAD_MBUF_FAST_FREE;

	/* Configure the Ethernet device. */
	retval = rte_eth_dev_configure(port, rx_rings, tx_rings, &port_conf);
	if (retval != 0)
		return retval;

	retval = rte_eth_dev_adjust_nb_rx_tx_desc(port, &nb_rxd, &nb_txd);
	if (retval != 0)
		return retval;

	/* Allocate and set up 1 RX queue per Ethernet port. */
	for (q = 0; q < rx_rings; q++) {
		retval = rte_eth_rx_queue_setup(
			port, q, nb_rxd, rte_eth_dev_socket_id(port), NULL, mbuf_pool);
		if (retval < 0)
			return retval;
	}

	txconf          = dev_info.default_txconf;
	txconf.offloads = port_conf.txmode.offloads;
	/* Allocate and set up 1 TX queue per Ethernet port. */
	for (q = 0; q < tx_rings; q++) {
		retval = rte_eth_tx_queue_setup(port, q, nb_txd,
		                                rte_eth_dev_socket_id(port), &txconf);
		if (retval < 0)
			return retval;
	}

	/* Starting Ethernet port. 8< */
	retval = rte_eth_dev_start(port);
	/* >8 End of starting of ethernet port. */
	if (retval < 0)
		return retval;

	/* Display the port MAC address. */
	struct rte_ether_addr addr;
	retval = rte_eth_macaddr_get(port, &addr);
	if (retval != 0)
		return retval;

	RTE_LOG(INFO, TEST,
	        "Port %u MAC: %02" PRIx8 " %02" PRIx8 " %02" PRIx8 " %02" PRIx8
	        " %02" PRIx8 " %02" PRIx8 "\n",
	        port, RTE_ETHER_ADDR_BYTES(&addr));

	/* Enable RX in promiscuous mode for the Ethernet device. */
	retval = rte_eth_promiscuous_enable(port);
	/* End of setting RX port in promiscuous mode. */
	if (retval != 0)
		return retval;

	return 0;
}

static void signal_handler(int signum) {
	if (signum == SIGINT || signum == SIGTERM) {
		printf("\n\nSignal %d received, preparing to exit...\n", signum);
		force_quit = true;
	}
}

static int rx(void *arg) {
	uint16_t port = 0;

	/*
	 * Check that the port is on the same NUMA node as the polling thread
	 * for best performance.
	 */
	if (rte_eth_dev_socket_id(port) >= 0 &&
	    rte_eth_dev_socket_id(port) != (int)rte_socket_id())
		RTE_LOG(WARNING, TEST,
		        "WARNING, port %u is on remote NUMA node to "
		        "polling thread.\n\tPerformance will "
		        "not be optimal.\n",
		        port);

	RTE_LOG(INFO, TEST, "START Rx, on lcore %u\n", rte_lcore_id());

	while (!force_quit) {
		/* Get burst of RX packets */
		struct rte_mbuf *bufs[BURST_SIZE];
		const uint16_t   nb_rx = rte_eth_rx_burst(port, 0, bufs, BURST_SIZE);

		if (unlikely(nb_rx == 0))
			continue;

		RTE_LOG(INFO, TEST, "Rx: RECEIVE %d packets\n", nb_rx);

		const uint16_t nb_tx =
			rte_ring_enqueue_burst(rx_ring, (void *)bufs, nb_rx, NULL);
		if (nb_tx < nb_rx) {
			for (int i = nb_tx; i < nb_rx; i++) {
				rte_pktmbuf_free(bufs[i]);
			}
		}
	}

	return 0;
}

static int worker(void *arg) {
	static int64_t    count                       = 0;
	struct mbuf_meta  mbuf_meta_array[BURST_SIZE] = {0};
	struct mbuf_meta *mbuf_meta_ptble[BURST_SIZE] = {0};

	/* initialize pointer table */
	for (int i = 0; i < BURST_SIZE; i++) {
		mbuf_meta_ptble[i] = &mbuf_meta_array[i];
	}

	RTE_LOG(INFO, TEST, "START Worker, on lcore %u\n", rte_lcore_id());

	while (!force_quit) {
		struct rte_mbuf *bufs[BURST_SIZE];
		const uint16_t   nb_rx =
			rte_ring_dequeue_burst(rx_ring, (void *)bufs, BURST_SIZE, NULL);

		if (unlikely(nb_rx == 0))
			continue;

		RTE_LOG(INFO, TEST, "Worker: RECEIVE %d packets\n", nb_rx);

		for (int i = 0; i < nb_rx; i++) {
			mbuf_meta_ptble[i]->buf      = bufs[i];
			mbuf_meta_ptble[i]->meta.tsc = rte_rdtsc();
			RTE_LOG(INFO, TEST, "Worker: TSC %#016lx\n",
			        mbuf_meta_ptble[i]->meta.tsc);
		}

		const uint16_t nb_tx = rte_ring_enqueue_burst(
			tx_ring, (void *)mbuf_meta_ptble, nb_rx, NULL);
		if (nb_tx < nb_rx) {
			for (int i = nb_tx; i < nb_rx; i++) {
				rte_pktmbuf_free(bufs[i]);
			}
		}
	}

	return 0;
}

static int tx(void *arg) {
	uint16_t port = 1;

	RTE_LOG(INFO, TEST, "START Tx, on lcore %u\n", rte_lcore_id());

	while (!force_quit) {
		struct mbuf_meta *mbuf_meta_ptble[BURST_SIZE];
		struct rte_mbuf  *bufs[BURST_SIZE];

		const uint16_t nb_rx =
			// rte_ring_dequeue_burst(tx_ring, (void *)bufs, BURST_SIZE, NULL);
			rte_ring_dequeue_burst(tx_ring, (void *)mbuf_meta_ptble, BURST_SIZE,
		                           NULL);

		if (unlikely(nb_rx == 0))
			continue;

		RTE_LOG(INFO, TEST, "Tx: RECEIVE %d packets\n", nb_rx);

		for (int i = 0; i < nb_rx; i++) {
			bufs[i] = mbuf_meta_ptble[i]->buf;
			RTE_LOG(INFO, TEST, "Tx: TSC %#016lx\n",
			        mbuf_meta_ptble[i]->meta.tsc);
		}

		/* Send burst of TX packets, to second port of pair. */
		const uint16_t nb_tx = rte_eth_tx_burst(port, 0, bufs, nb_rx);

		/* Free any unsent packets. */
		if (unlikely(nb_tx < nb_rx)) {
			uint16_t buf;
			for (buf = nb_tx; buf < nb_rx; buf++)
				rte_pktmbuf_free(bufs[buf]);
		}
	}
	return 0;
}

/*
 * The main function, which does initialization and calls the per-lcore
 * functions.
 */
int main(int argc, char *argv[]) {
	struct rte_mempool *mbuf_pool;
	unsigned            nb_ports;
	uint16_t            portid;
	unsigned int        nb_cores;
	unsigned int        nb_used_cores;

	rte_log_set_global_level(RTE_LOG_DEBUG);

	/* Initializion the Environment Abstraction Layer (EAL). 8< */
	int ret = rte_eal_init(argc, argv);
	if (ret < 0)
		rte_exit(EXIT_FAILURE, "Error with EAL initialization\n");
	/* >8 End of initialization the Environment Abstraction Layer (EAL). */

	argc -= ret;
	argv += ret;

	force_quit = false;
	signal(SIGINT, signal_handler);
	signal(SIGTERM, signal_handler);

	/* Check that there is an even number of ports to send/receive on. */
	nb_ports = rte_eth_dev_count_avail();
	if (nb_ports < 2 || (nb_ports & 1))
		rte_exit(EXIT_FAILURE, "Error: number of ports must be even\n");

	/* Creates a new mempool in memory to hold the mbufs. */

	/* Allocates mempool to hold the mbufs. 8< */
	mbuf_pool = rte_pktmbuf_pool_create(
		"MBUF_POOL", NUM_MBUFS * nb_ports, MBUF_CACHE_SIZE, 0,
		RTE_MBUF_DEFAULT_BUF_SIZE, rte_socket_id());
	/* >8 End of allocating mempool to hold mbuf. */

	if (mbuf_pool == NULL)
		rte_exit(EXIT_FAILURE, "Cannot create mbuf pool\n");

	/* Initializing all ports. 8< */
	RTE_ETH_FOREACH_DEV(portid)
	if (port_init(portid, mbuf_pool) != 0)
		rte_exit(EXIT_FAILURE, "Cannot init port %" PRIu16 "\n", portid);
	/* >8 End of initializing all ports. */

	nb_cores = rte_lcore_count();
	if (nb_cores < REQUIRE_CORES)
		rte_exit(EXIT_FAILURE,
		         "Too few cores, requires at least %" PRId8 " cores.\n",
		         REQUIRE_CORES);

	rx_ring =
		rte_ring_create("wk_rx0", RING_SIZE, rte_socket_id(), RING_F_SP_ENQ);
	tx_ring =
		rte_ring_create("wk_tx0", RING_SIZE, rte_socket_id(), RING_F_SC_DEQ);
	if (rx_ring == NULL || tx_ring == NULL)
		rte_exit(EXIT_FAILURE, "Cannot create rx/tx ring\n");

	nb_used_cores++;

	unsigned int tgt_core = rte_get_next_lcore(rte_lcore_id(), true, false);
	// unsigned int tgt_core = rte_lcore_id();
	rte_eal_remote_launch(rx, NULL, tgt_core);
	nb_used_cores++;

	tgt_core = rte_get_next_lcore(tgt_core, true, false);
	rte_eal_remote_launch(tx, NULL, tgt_core);
	nb_used_cores++;

	for (nb_used_cores; nb_used_cores < nb_cores; nb_used_cores++) {
		tgt_core = rte_get_next_lcore(tgt_core, true, false);
		if (tgt_core == RTE_MAX_LCORE) {
			rte_exit(EXIT_FAILURE,
			         "Too few cores, requires at least %" PRId8 " cores.\n",
			         REQUIRE_CORES);
		}
		rte_eal_remote_launch(worker, NULL, tgt_core);
	}

	/* Call lcore_main on the main core only. Called on single lcore. 8< */
	// lcore_main();
	/* >8 End of called on single lcore. */

	/* force_quit is true when we get here */
	rte_eal_mp_wait_lcore();

	RTE_ETH_FOREACH_DEV(portid) {
		printf("Closing port %d\n", portid);
		ret = rte_eth_dev_stop(portid);
		if (ret != 0)
			RTE_LOG(ERR, TEST, "rte_eth_dev_stop: err=%s, port=%u\n",
			        rte_strerror(-ret), portid);
		rte_eth_dev_close(portid);
	}

	/* clean up the EAL */
	rte_eal_cleanup();

	return 0;
}
