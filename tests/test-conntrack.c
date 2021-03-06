/*
 * Copyright (c) 2015 Nicira, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at:
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <config.h>
#include "conntrack.h"

#include "dp-packet.h"
#include "fatal-signal.h"
#include "flow.h"
#include "netdev.h"
#include "ovs-thread.h"
#include "ovstest.h"
#include "pcap-file.h"
#include "timeval.h"

static const char payload[] = "50540000000a50540000000908004500001c0000000000"
                              "11a4cd0a0101010a0101020001000200080000";

static struct dp_packet_batch *
prepare_packets(size_t n, bool change, unsigned tid, ovs_be16 *dl_type)
{
    struct dp_packet_batch *pkt_batch = xzalloc(sizeof *pkt_batch);
    struct flow flow;
    size_t i;

    ovs_assert(n <= ARRAY_SIZE(pkt_batch->packets));

    dp_packet_batch_init(pkt_batch);
    pkt_batch->count = n;

    for (i = 0; i < n; i++) {
        struct udp_header *udp;
        struct dp_packet *pkt = dp_packet_new(sizeof payload/2);

        dp_packet_put_hex(pkt, payload, NULL);
        flow_extract(pkt, &flow);

        udp = dp_packet_l4(pkt);
        udp->udp_src = htons(ntohs(udp->udp_src) + tid);

        if (change) {
            udp->udp_dst = htons(ntohs(udp->udp_dst) + i);
        }

        pkt_batch->packets[i] = pkt;
        *dl_type = flow.dl_type;
    }


    return pkt_batch;
}

static void
destroy_packets(struct dp_packet_batch *pkt_batch)
{
    dp_packet_delete_batch(pkt_batch, true);
    free(pkt_batch);
}

struct thread_aux {
    pthread_t thread;
    unsigned tid;
};

static struct conntrack ct;
static unsigned long n_threads, n_pkts, batch_size;
static bool change_conn = false;
static struct ovs_barrier barrier;

static void *
ct_thread_main(void *aux_)
{
    struct thread_aux *aux = aux_;
    struct dp_packet_batch *pkt_batch;
    ovs_be16 dl_type;
    size_t i;

    pkt_batch = prepare_packets(batch_size, change_conn, aux->tid, &dl_type);
    ovs_barrier_block(&barrier);
    for (i = 0; i < n_pkts; i += batch_size) {
        conntrack_execute(&ct, pkt_batch, dl_type, true, 0, NULL, NULL, NULL);
    }
    ovs_barrier_block(&barrier);
    destroy_packets(pkt_batch);

    return NULL;
}

static void
test_benchmark(struct ovs_cmdl_context *ctx)
{
    struct thread_aux *threads;
    long long start;
    unsigned i;

    fatal_signal_init();

    /* Parse arguments */
    n_threads = strtoul(ctx->argv[1], NULL, 0);
    if (!n_threads) {
        ovs_fatal(0, "n_threads must be at least one");
    }
    n_pkts = strtoul(ctx->argv[2], NULL, 0);
    batch_size = strtoul(ctx->argv[3], NULL, 0);
    if (batch_size == 0 || batch_size > NETDEV_MAX_BURST) {
        ovs_fatal(0, "batch_size must be between 1 and NETDEV_MAX_BURST(%u)",
                  NETDEV_MAX_BURST);
    }
    if (ctx->argc > 4) {
        change_conn = strtoul(ctx->argv[4], NULL, 0);
    }

    threads = xcalloc(n_threads, sizeof *threads);
    ovs_barrier_init(&barrier, n_threads + 1);
    conntrack_init(&ct);

    /* Create threads */
    for (i = 0; i < n_threads; i++) {
        threads[i].tid = i;
        threads[i].thread = ovs_thread_create("ct_thread", ct_thread_main,
                                              &threads[i]);
    }
    /* Starts the work inside the threads */
    ovs_barrier_block(&barrier);
    start = time_msec();

    /* Wait for the threads to finish the work */
    ovs_barrier_block(&barrier);
    printf("conntrack:  %5lld ms\n", time_msec() - start);

    for (i = 0; i < n_threads; i++) {
        xpthread_join(threads[i].thread, NULL);
    }

    conntrack_destroy(&ct);
    ovs_barrier_destroy(&barrier);
    free(threads);
}

static void
pcap_batch_execute_conntrack(struct conntrack *ct,
                             struct dp_packet_batch *pkt_batch)
{
    size_t i;
    struct dp_packet_batch new_batch;
    ovs_be16 dl_type = htons(0);

    dp_packet_batch_init(&new_batch);

    /* pkt_batch contains packets with different 'dl_type'. We have to
     * call conntrack_execute() on packets with the same 'dl_type'. */

    for (i = 0; i < pkt_batch->count; i++) {
        struct dp_packet *pkt = pkt_batch->packets[i];
        struct flow flow;

        /* This also initializes the l3 and l4 pointers. */
        flow_extract(pkt, &flow);

        if (!new_batch.count) {
            dl_type = flow.dl_type;
        }

        if (flow.dl_type != dl_type) {
            conntrack_execute(ct, &new_batch, dl_type, true, 0, NULL, NULL,
                              NULL);
            dp_packet_batch_init(&new_batch);
        }
        new_batch.packets[new_batch.count++] = pkt;
    }

    if (new_batch.count) {
        conntrack_execute(ct, &new_batch, dl_type, true, 0, NULL, NULL, NULL);
    }

}

static void
test_pcap(struct ovs_cmdl_context *ctx)
{
    size_t total_count, i, batch_size;
    FILE *pcap;
    int err;

    pcap = ovs_pcap_open(ctx->argv[1], "rb");
    if (!pcap) {
        return;
    }

    batch_size = 1;
    if (ctx->argc > 2) {
        batch_size = strtoul(ctx->argv[2], NULL, 0);
        if (batch_size == 0 || batch_size > NETDEV_MAX_BURST) {
            ovs_fatal(0,
                      "batch_size must be between 1 and NETDEV_MAX_BURST(%u)",
                      NETDEV_MAX_BURST);
        }
    }

    fatal_signal_init();

    conntrack_init(&ct);
    total_count = 0;
    for (;;) {
        struct dp_packet_batch pkt_batch;

        dp_packet_batch_init(&pkt_batch);

        for (i = 0; i < batch_size; i++) {
            err = ovs_pcap_read(pcap, &pkt_batch.packets[i], NULL);
            if (err) {
                break;
            }
        }

        pkt_batch.count = i;
        if (pkt_batch.count == 0) {
            break;
        }

        pcap_batch_execute_conntrack(&ct, &pkt_batch);

        for (i = 0; i < pkt_batch.count; i++) {
            struct ds ds = DS_EMPTY_INITIALIZER;
            struct dp_packet *pkt = pkt_batch.packets[i];

            total_count++;

            format_flags(&ds, ct_state_to_string, pkt->md.ct_state, '|');
            printf("%"PRIuSIZE": %s\n", total_count, ds_cstr(&ds));

            ds_destroy(&ds);
        }

        dp_packet_delete_batch(&pkt_batch, true);
        if (err) {
            break;
        }
    }
    conntrack_destroy(&ct);
}

static const struct ovs_cmdl_command commands[] = {
    /* Connection tracker tests. */
    /* Starts 'n_threads' threads. Each thread will send 'n_pkts' packets to
     * the connection tracker, 'batch_size' per call. If 'change_connection'
     * is '1', each packet in a batch will have a different source and
     * destination port */
    {"benchmark", "n_threads n_pkts batch_size [change_connection]", 3, 4,
     test_benchmark, OVS_RO},
    /* Reads packets from 'file' and sends them to the connection tracker,
     * 'batch_size' (1 by default) per call, with the commit flag set.
     * Prints the ct_state of each packet. */
    {"pcap", "file [batch_size]", 1, 2, test_pcap, OVS_RO},

    {NULL, NULL, 0, 0, NULL, OVS_RO},
};

static void
test_conntrack_main(int argc, char *argv[])
{
    struct ovs_cmdl_context ctx = {
        .argc = argc - 1,
        .argv = argv + 1,
    };
    set_program_name(argv[0]);
    ovs_cmdl_run_command(&ctx, commands);
}

OVSTEST_REGISTER("test-conntrack", test_conntrack_main);
