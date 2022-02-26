#include <arpa/inet.h>
#include <omp.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <functional>
#include <queue>

#include "nf_trace.h"

#define IGNORE_PRINTF

#ifdef IGNORE_PRINTF
#define printf(fmt, ...) (0)
#endif

// param: link latency in cycles
// assuming 3.2 GHz, this number / 3.2 = link latency in ns
// e.g. setting this to 35000 gives you 35000/3.2 = 10937.5 ns latency
// IMPORTANT: this must be a multiple of 7
//
// THIS IS SET BY A COMMAND LINE ARGUMENT. DO NOT CHANGE IT HERE.
//#define LINKLATENCY 6405
int LINKLATENCY = 0;

// param: switching latency in cycles
// assuming 3.2 GHz, this number / 3.2 = switching latency in ns
//
// THIS IS SET BY A COMMAND LINE ARGUMENT. DO NOT CHANGE IT HERE.
int switchlat = 0;

#define SWITCHLATENCY (switchlat)

// param: numerator and denominator of bandwidth throttle
// Used to throttle outbound bandwidth from port
//
// THESE ARE SET BY A COMMAND LINE ARGUMENT. DO NOT CHANGE IT HERE.
int throttle_numer = 1;
int throttle_denom = 1;

// uncomment to use a limited output buffer size, OUTPUT_BUF_SIZE
//#define LIMITED_BUFSIZE

// size of output buffers, in # of flits
// only if LIMITED BUFSIZE is set
// TODO: expose in manager
#define OUTPUT_BUF_SIZE (131072L)

// pull in # clients config
#define NUMCLIENTSCONFIG
#include "switchconfig.h"
#undef NUMCLIENTSCONFIG

// DO NOT TOUCH
#define NUM_TOKENS (LINKLATENCY)
#define TOKENS_PER_BIGTOKEN (7)
#define BIGTOKEN_BYTES (64)
#define NUM_BIGTOKENS (NUM_TOKENS / TOKENS_PER_BIGTOKEN)
#define BUFSIZE_BYTES (NUM_BIGTOKENS * BIGTOKEN_BYTES)

// DO NOT TOUCH
#define SWITCHLAT_NUM_TOKENS (SWITCHLATENCY)
#define SWITCHLAT_NUM_BIGTOKENS (SWITCHLAT_NUM_TOKENS / TOKENS_PER_BIGTOKEN)
#define SWITCHLAT_BUFSIZE_BYTES (SWITCHLAT_NUM_BIGTOKENS * BIGTOKEN_BYTES)

uint64_t this_iter_cycles_start = 0;

// pull in mac2port array
#define MACPORTSCONFIG
#include "switchconfig.h"
#undef MACPORTSCONFIG

#include "baseport.h"
#include "shmemport.h"
#include "socketport.h"
#include "sshport.h"

// TODO: replace these port mapping hacks with a mac -> port mapping,
// could be hardcoded

BasePort *ports[NUMPORTS];

void load_nf_trace() {
  // loading nf workload traces
  load_pkt();
#define MAC_NFTOP 0x0200006d1200
#define MAC_PKTGEN 0x0300006d1200
#define ETH_P_IP 0x0800 /* Internet Protocol packet	*/
  for (int i = 0; i < PKT_NUM; i++) {
    uint64_t *pkt_bytes = (uint64_t *)pkts[i].content;
    pkt_bytes[0] = MAC_NFTOP << 16;
    pkt_bytes[1] = MAC_PKTGEN | ((uint64_t)htons(ETH_P_IP) << 48);
  }
}

void send_packet(pkt_t *pkt, uint16_t send_to_port) {
  int data_packet_size_bytes = pkt->len;

  // Convert the packet to a switchpacket
  switchpacket *new_tsp = (switchpacket *)calloc(sizeof(switchpacket), 1);
  new_tsp->timestamp = this_iter_cycles_start;
  new_tsp->amtwritten =
      (data_packet_size_bytes + sizeof(uint64_t) - 1) / sizeof(uint64_t);
  new_tsp->amtread = 0;
  new_tsp->sender = 0;
  memcpy(new_tsp->dat, pkt->content, data_packet_size_bytes);

  // fprintf(
  //     stdout,
  //     "send_packet: data_packet_size_bytes = %d, new_tsp->amtwritten = %d\n",
  //     data_packet_size_bytes, new_tsp->amtwritten);
  ports[send_to_port]->outputqueue.push(new_tsp);
}

// TODO: maybe only stop all other's pktgen after the slowest NF processes
// certain number of packets

#define WARMUP_NPKTS 10000
#define TEST_NPKTS 20000
#define PRINT_INTERVAL 1000
#define MAX_UNACK_WINDOW 512
#define CUSTOM_PROTO_BASE 0x1234

// l2_fwd has around 20000 cycles latency, we time it by 100000x
#define TIMEOUT_CYCLES (100000 * 20000ul)
#define NCORES 4

#define MEASURE_TIMEOUT
#ifdef MEASURE_TIMEOUT
static uint64_t timestamps[TEST_NPKTS + WARMUP_NPKTS];
static uint64_t total_pkt_latency = 0;
static uint64_t total_pkt_cnt = 0;
#endif

static std::atomic_flag nf_readyness[NCORES] = {ATOMIC_FLAG_INIT};
static std::atomic<uint32_t> num_ready_nfs;
static std::atomic_flag nf_finishness[NCORES] = {ATOMIC_FLAG_INIT};
static std::atomic<uint32_t> num_finished_nfs;

static std::atomic<uint64_t> unack_pkts[NCORES] = {};
static std::atomic<uint64_t> sent_pkts[NCORES] = {};
static std::atomic<uint64_t> sent_pkts_size[NCORES] = {};
static std::atomic<uint64_t> received_pkts[NCORES] = {};
static std::atomic<uint64_t> lost_pkts[NCORES] = {};
static std::atomic<uint64_t> invalid_pkts;

static std::atomic<uint64_t> last_gen_pkt_timestamp[NCORES] = {};
static std::atomic<uint64_t> start_gen_pkt_timestamp[NCORES] = {};
static std::atomic<uint64_t> finish_gen_pkt_timestamp[NCORES] = {};

static std::atomic<uint8_t> warmup_end[NCORES] = {};
static std::atomic<uint8_t> warmup_end_recv[NCORES] = {};

static pkt_t sending_pkt_vec[NCORES][MAX_UNACK_WINDOW];

void generate_load_packets() {
  // not all NFs are ready, skip generating packets
  if (num_ready_nfs != NCORES) {
    return;
  }

  // for (int nf_idx = 0; nf_idx < NCORES; nf_idx++) {
  for (int nf_idx = 0; nf_idx < 1; nf_idx++) {
    pkt_t *cur_sending_pkt_vec = sending_pkt_vec[nf_idx];

    // this NF has processed all packets.
    if (sent_pkts[nf_idx] >= TEST_NPKTS + WARMUP_NPKTS) {
      if (!nf_finishness[nf_idx].test_and_set()) {
        num_finished_nfs++;
        finish_gen_pkt_timestamp[nf_idx] = this_iter_cycles_start;
#define CPU_GHZ (3.2)
        double time_taken = (finish_gen_pkt_timestamp[nf_idx] -
                             start_gen_pkt_timestamp[nf_idx]) /
                            CPU_GHZ * 1e-3;
        sent_pkts[nf_idx] -= WARMUP_NPKTS;
        received_pkts[nf_idx] -= WARMUP_NPKTS;
        fprintf(
            stdout,
            "[send_pacekts th%d]:     pkts sent: %lu, unacked pkts: %4lu, "
            "potentially lost pkts: %4lu, %.8lf Mpps, %.6lfGbps\n",
            nf_idx, sent_pkts[nf_idx].load(), unack_pkts[nf_idx].load(),
            lost_pkts[nf_idx].load(),
            (double)(sent_pkts[nf_idx].load()) / time_taken,
            (double)(sent_pkts_size[nf_idx].load()) * 8 / (time_taken * 1e3));
      }
      continue;
    }
    // so many unacked packets, not send load packets.
    if (sent_pkts[nf_idx] >= received_pkts[nf_idx] &&
        (unack_pkts[nf_idx] = sent_pkts[nf_idx] - received_pkts[nf_idx]) >=
            MAX_UNACK_WINDOW) {
      if (this_iter_cycles_start - last_gen_pkt_timestamp[nf_idx] >
          TIMEOUT_CYCLES) {
        lost_pkts[nf_idx] += sent_pkts[nf_idx] - received_pkts[nf_idx];
        sent_pkts[nf_idx] = received_pkts[nf_idx].load();
        unack_pkts[nf_idx] = 0;
        fprintf(
            stdout,
            "[send_pacekts th%d]: deadlock detected (caused by packet loss or "
            "NF initing), forcely resolving...\n",
            nf_idx);
      } else {
        continue;
      }
    }
    last_gen_pkt_timestamp[nf_idx] = this_iter_cycles_start;

    // the max number of packet sent per epoch is MAX_UNACK_WINDOW
    uint64_t burst_size = MAX_UNACK_WINDOW - unack_pkts[nf_idx];
    burst_size =
        std::min(burst_size, TEST_NPKTS + WARMUP_NPKTS - sent_pkts[nf_idx]);
    // fprintf(stdout, "generate_load_packets generate packets %lu\n",
    // burst_size);

    for (int i = 0; i < burst_size; i++) {
      pkt_t *pkt = next_pkt(nf_idx);
      pkt_t *cur_sending_pkt = &cur_sending_pkt_vec[i];
      memcpy(cur_sending_pkt, pkt, sizeof(pkt_t));

      // setup differnt eth_type to differenciate different NFs' packets.
      auto eh = (struct ether_hdr *)(cur_sending_pkt->content + NET_IP_ALIGN);
      eh->ether_type = htons(CUSTOM_PROTO_BASE + (uint16_t)nf_idx);

      auto tcph = (struct tcp_hdr *)(cur_sending_pkt->content + NET_IP_ALIGN +
                                     sizeof(struct ether_hdr) +
                                     sizeof(struct ipv4_hdr));
      uint32_t pkt_idx = sent_pkts[nf_idx] + i;
      tcph->sent_seq = 0xdeadbeef;
      tcph->recv_ack = pkt_idx;

#ifdef MEASURE_TIMEOUT
      timestamps[pkt_idx] = this_iter_cycles_start;
#endif

      // 20B inter-pkt frame
      sent_pkts_size[nf_idx] += cur_sending_pkt->len + 20;

      // assume there is only one port
      send_packet(cur_sending_pkt, 0);
    }
    sent_pkts[nf_idx] += burst_size;

    // warm up phase ends
    if (!warmup_end[nf_idx].load() && sent_pkts[nf_idx] >= WARMUP_NPKTS) {
      warmup_end[nf_idx] = (uint8_t)1;
      start_gen_pkt_timestamp[nf_idx] = this_iter_cycles_start;
      sent_pkts_size[nf_idx] = 0;
    }

    if (sent_pkts[nf_idx] % PRINT_INTERVAL == 0) {
      fprintf(stdout,
              "[send_pacekts th%d]:     pkts sent: %lu, unacked pkts: %lu, "
              "potentially lost pkts: %lu\n",
              nf_idx, sent_pkts[nf_idx].load(), unack_pkts[nf_idx].load(),
              lost_pkts[nf_idx].load());
    }
  }
}

// Processing packet received from the NIC, updating the above states
// accordingly ether_type == CUSTOM_PROTO_BASE + nf_idx: Packets from NIC core
// nf_idx ether_type == CUSTOM_PROTO_BASE + NCORES + nf_idx: Boot packet used by
// NIC core to indicate the readyness of one NF
void process_recv_packet(uint8_t *pkt_data) {
  struct ether_hdr *eh_recv = (struct ether_hdr *)(pkt_data + NET_IP_ALIGN);
  int ether_type = (int)htons((eh_recv->ether_type));
  // fprintf(stdout, "process_recv_packet recv one packet: ether_type %d\n",
  //         ether_type);

  if (!(ether_type >= CUSTOM_PROTO_BASE &&
        ether_type < CUSTOM_PROTO_BASE + 2 * NCORES)) {
    fprintf(stdout, "invalid packet ether_type: %hu\n", ether_type);
    return;
  }

  // processing boot packets.
  int nf_idx = ether_type - CUSTOM_PROTO_BASE;
  if (nf_idx >= NCORES) {
    nf_idx -= NCORES;
    if (!nf_readyness[nf_idx].test_and_set()) {
      num_ready_nfs++;
      fprintf(stdout, "process_recv_packet recv one boot packet\n");
      if (num_ready_nfs == NCORES) {
        fprintf(stdout, "process_recv_packet recv all boot packets\n");
      }
    }
    return;
  }

  struct tcp_hdr *tcph =
      (struct tcp_hdr *)(pkt_data + NET_IP_ALIGN + sizeof(struct ipv4_hdr) +
                         sizeof(struct ether_hdr));
  // fprintf(stdout, "[recv_pacekts %d] nf_idx = %d, tcph->sent_seq = %x\n",
  // nf_idx, nf_idx, tcph->sent_seq);
  if (tcph->sent_seq != 0xdeadbeef) {
    invalid_pkts++;
    return;
  }
  if (tcph->recv_ack == 0xFFFFFFFF) {
    return;
  }
  received_pkts[nf_idx]++;

  uint32_t pkt_idx = tcph->recv_ack;
  uint64_t curr_received_pkts = received_pkts[nf_idx];
  uint32_t lost_pkts = pkt_idx + 1 - curr_received_pkts;

#ifdef MEASURE_TIMEOUT
  timestamps[pkt_idx] = this_iter_cycles_start - timestamps[pkt_idx];
  total_pkt_latency += timestamps[pkt_idx];
  total_pkt_cnt++;
  if (total_pkt_cnt % PRINT_INTERVAL == 0) {
    fprintf(stdout, "total_pkt_cnt = %lu, avg pkt latency = %lu cycles\n",
            total_pkt_cnt, total_pkt_latency / total_pkt_cnt);
  }
#endif

  // warm up phase ends
  if (!warmup_end_recv[nf_idx].load() && pkt_idx >= WARMUP_NPKTS - 1) {
    warmup_end_recv[nf_idx] = (uint8_t)1;
    int lost_pkt_during_cold_start = lost_pkts;
    printf(
        "[recv_pacekts th%d]: warm up ends, lost_pkt_during_cold_start = "
        "%llu\n",
        nf_idx, lost_pkt_during_cold_start);
  }

  // TODO: handle re-ordered packets.
  // fprintf(stdout, "%lu, %llu\n", pkt_idx, curr_received_pkts);

  // these packets are lost
  // if(lost_pkts != 0){
  //     received_pkts[nf_idx] += lost_pkts
  // }

  if (curr_received_pkts % PRINT_INTERVAL == 0) {
    fprintf(stdout, "[recv_pacekts th%d]: pkts received: %lu\n", nf_idx,
            curr_received_pkts);
  }
}

/* switch from input ports to output ports */
void do_fast_switching() {
#pragma omp parallel for
  for (int port = 0; port < NUMPORTS; port++) {
    ports[port]->setup_send_buf();
  }

// preprocess from raw input port to packets
#pragma omp parallel for
  for (int port = 0; port < NUMPORTS; port++) {
    BasePort *current_port = ports[port];
    uint8_t *input_port_buf = current_port->current_input_buf;

    for (int tokenno = 0; tokenno < NUM_TOKENS; tokenno++) {
      if (is_valid_flit(input_port_buf, tokenno)) {
        uint64_t flit = get_flit(input_port_buf, tokenno);

        switchpacket *sp;
        if (!(current_port->input_in_progress)) {
          sp = (switchpacket *)calloc(sizeof(switchpacket), 1);
          current_port->input_in_progress = sp;

          // here is where we inject switching latency. this is min port-to-port
          // latency
          sp->timestamp = this_iter_cycles_start + tokenno + SWITCHLATENCY;
          sp->sender = port;
        }
        sp = current_port->input_in_progress;

        sp->dat[sp->amtwritten++] = flit;
        if (is_last_flit(input_port_buf, tokenno)) {
          current_port->input_in_progress = NULL;
          if (current_port->push_input(sp)) {
            // fprintf(stdout, "packet timestamp: %ld, len: %ld, sender: %d\n",
            //         this_iter_cycles_start + tokenno, sp->amtwritten, port);
          }
        }
      }
    }
  }

  // next do the switching. but this switching is just shuffling pointers,
  // so it should be fast. it has to be serial though...

  // NO PARALLEL!
  // shift pointers to output queues, but in order. basically.
  // until the input queues have no more complete packets
  // 1) find the next switchpacket with the lowest timestamp across all the
  // inputports 2) look at its mac, copy it into the right ports
  //          i) if it's a broadcast: sorry, you have to make N-1 copies of
  //          it... to put into the other queues

  struct tspacket {
    uint64_t timestamp;
    switchpacket *switchpack;

    bool operator<(const tspacket &o) const { return timestamp > o.timestamp; }
  };

  typedef struct tspacket tspacket;

  // TODO thread safe priority queue? could do in parallel?
  std::priority_queue<tspacket> pqueue;

  for (int i = 0; i < NUMPORTS; i++) {
    while (!(ports[i]->inputqueue.empty())) {
      switchpacket *sp = ports[i]->inputqueue.front();
      ports[i]->inputqueue.pop();
      pqueue.push(tspacket{sp->timestamp, sp});
    }
  }

  // next, put back into individual output queues
  while (!pqueue.empty()) {
    switchpacket *tsp = pqueue.top().switchpack;
    pqueue.pop();
    uint16_t send_to_port =
        get_port_from_flit(tsp->dat[0], 0 /* junk remove arg */);
    // fprintf(stdout, "packet for port: %x\n", send_to_port);
    // fprintf(stdout, "packet timestamp: %ld\n", tsp->timestamp);
    // we bypass the switching logic, just doing ack packet processing and load
    // generaion
    /*
        if (send_to_port == BROADCAST_ADJUSTED) {
    #define ADDUPLINK(NUMUPLINKS > 0 ? 1 : 0)
              // this will only send broadcasts to the first (zeroeth) uplink.
              // on a switch receiving broadcast packet from an uplink, this
    should
              // automatically prevent switch from sending the broadcast to any
              // uplink
              for (int i = 0; i < NUMDOWNLINKS + ADDUPLINK; i++) {
            if (i != tsp->sender) {
              switchpacket *tsp2 = (switchpacket *)malloc(sizeof(switchpacket));
              memcpy(tsp2, tsp, sizeof(switchpacket));
              ports[i]->outputqueue.push(tsp2);
            }
          }
          free(tsp);
        } else {
          ports[send_to_port]->outputqueue.push(tsp);
        }
    */
    process_recv_packet((uint8_t *)tsp->dat);
    free(tsp);
  }

  generate_load_packets();

  // finally in parallel, flush whatever we can to the output queues based on
  // timestamp

#pragma omp parallel for
  for (int port = 0; port < NUMPORTS; port++) {
    BasePort *thisport = ports[port];
    thisport->write_flits_to_output();
  }
}

static void simplify_frac(int n, int d, int *nn, int *dd) {
  int a = n, b = d;

  // compute GCD
  while (b > 0) {
    int t = b;
    b = a % b;
    a = t;
  }

  *nn = n / a;
  *dd = d / a;
}

int main(int argc, char *argv[]) {
  int bandwidth;

  if (argc < 4) {
    // if insufficient args, error out
    fprintf(stdout, "usage: ./switch LINKLATENCY SWITCHLATENCY BANDWIDTH\n");
    fprintf(stdout, "insufficient args provided\n.");
    fprintf(stdout,
            "LINKLATENCY and SWITCHLATENCY should be provided in cycles.\n");
    fprintf(stdout, "BANDWIDTH should be provided in Gbps\n");
    exit(1);
  }

  LINKLATENCY = atoi(argv[1]);
  switchlat = atoi(argv[2]);
  bandwidth = atoi(argv[3]);

  simplify_frac(bandwidth, 200, &throttle_numer, &throttle_denom);

  fprintf(stdout, "Using link latency: %d\n", LINKLATENCY);
  fprintf(stdout, "Using switching latency: %d\n", SWITCHLATENCY);
  fprintf(stdout, "BW throttle set to %d/%d\n", throttle_numer, throttle_denom);

  if ((LINKLATENCY % 7) != 0) {
    // if invalid link latency, error out.
    fprintf(stdout,
            "INVALID LINKLATENCY. Currently must be multiple of 7 cycles.\n");
    exit(1);
  }

  num_ready_nfs = 0u;
  invalid_pkts = 0ul;
  num_finished_nfs = 0u;

  for (int i = 0; i < NCORES; i++) {
    unack_pkts[i] = 0ul;
    sent_pkts[i] = 0ul;
    sent_pkts_size[i] = 0ul;
    received_pkts[i] = 0ul;
    lost_pkts[i] = 0ul;
    last_gen_pkt_timestamp[i] = 0ul;
    finish_gen_pkt_timestamp[i] = 0ul;
    start_gen_pkt_timestamp[i] = 0ul;
    warmup_end[i] = (uint8_t)0;
    warmup_end_recv[i] = (uint8_t)0;
  }

  load_nf_trace();

  omp_set_num_threads(
      NUMPORTS);  // we parallelize over ports, so max threads = # ports

#define PORTSETUPCONFIG
#include "switchconfig.h"
#undef PORTSETUPCONFIG

  while (true) {
    // handle sends
#pragma omp parallel for
    for (int port = 0; port < NUMPORTS; port++) {
      ports[port]->send();
    }

    // handle receives. these are blocking per port
#pragma omp parallel for
    for (int port = 0; port < NUMPORTS; port++) {
      ports[port]->recv();
    }

#pragma omp parallel for
    for (int port = 0; port < NUMPORTS; port++) {
      ports[port]->tick_pre();
    }

    do_fast_switching();

    this_iter_cycles_start += LINKLATENCY;  // keep track of time

    // some ports need to handle extra stuff after each iteration
    // e.g. shmem ports swapping shared buffers
#pragma omp parallel for
    for (int port = 0; port < NUMPORTS; port++) {
      ports[port]->tick();
    }
  }
}
