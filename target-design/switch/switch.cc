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
#include <cmath>
#include <cstdlib>
#include <functional>
#include <numeric>
#include <queue>
#include <vector>

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

  ports[send_to_port]->outputqueue.push(new_tsp);
}

#define WARMUP_NPKTS 10000
#define TEST_NPKTS 20000
#define PRINT_INTERVAL 1000

// maximum in-flight packets
#define MAX_UNACK_WINDOW 512
// each switching round can only process 6405 words (8Bytes), given 620B
// packets, this is equal to around 82
#define MAX_PKTS_PER_ROUND 20
#define CUSTOM_PROTO_BASE 0x1234

// l2_fwd has around 20000 cycles latency, we time it by 100000x
#define TIMEOUT_CYCLES (100000 * 20000ul)
#define NCORES 4
#define CPU_GHZ (3.2)

static inline double get_us(uint64_t start_ts, uint64_t end_ts) {
  return (end_ts - start_ts) / CPU_GHZ * 1e-3;
}

// adapted from
// https://github.com/kfsone/filebench/blob/f099191cff895eb45baa74dc6ec8aec3981cbbaf/simplebench.cpp#L56-L67
static double nth_percentile(std::vector<double> &vec, double percentile) {
  const size_t length = vec.size();
  const size_t point = length * percentile;
  // If we wouldn't produce a whole number, take the average of the values
  // either side
  if (((point) % 100) != 0) {
    auto left = vec[size_t(std::floor(double(point) / 100.))];
    auto right = vec[size_t(std::ceil(double(point) / 100.))];
    return (left + right) / 2;
  }
  return vec[(point / 100)];
};

// storing per-NF per-packet latency
static std::vector<double> latencies[NCORES];

static std::atomic_flag nf_readyness[NCORES] = {ATOMIC_FLAG_INIT};
static std::atomic<uint32_t> num_ready_nfs;

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

// Each NF keep running even when it has processed TEST_NPKTS + WARMUP_NPKTS of
// packets; it only ends itself after the slowest NF has processed TEST_NPKTS +
// WARMUP_NPKTS of packets.
#define KEEP_RUNNING
static int num_nfs = 0;
static std::atomic<uint32_t> num_finished_nfs;
static std::atomic_flag nf_finishness[NCORES] = {ATOMIC_FLAG_INIT};
static std::atomic<uint8_t> nf_recv_end_pkt[NCORES] = {};
static std::atomic_flag nf_recv_end_pkt_sent[NCORES] = {ATOMIC_FLAG_INIT};

void generate_load_packets() {
  // not all NFs are ready, skip generating packets
  if (num_ready_nfs != NCORES) {
    return;
  }

  // TODO(yangzhou): mixing different NFs' packets in sendinng queue
  for (int nf_idx = 0; nf_idx < NCORES; nf_idx++) {
    pkt_t *cur_sending_pkt_vec = sending_pkt_vec[nf_idx];

    // check if the NF has finished
    // note that nf_recv_end_pkt is set by process_recv_packet().
    if (nf_recv_end_pkt[nf_idx].load()) {
      // first time arriving here will send an end pkt to NF
      if (!nf_recv_end_pkt_sent->test_and_set()) {
        pkt_t *pkt = next_pkt(nf_idx);
        pkt_t *cur_sending_pkt = &cur_sending_pkt_vec[0];
        memcpy(cur_sending_pkt, pkt, sizeof(pkt_t));

        // setup differnt eth_type to differenciate different NFs' packets.
        auto eh = (struct ether_hdr *)(cur_sending_pkt->content + NET_IP_ALIGN);
        eh->ether_type = htons(CUSTOM_PROTO_BASE + (uint16_t)nf_idx);

        auto tcph = (struct tcp_hdr *)(cur_sending_pkt->content + NET_IP_ALIGN +
                                       sizeof(struct ether_hdr) +
                                       sizeof(struct ipv4_hdr));
        uint32_t pkt_idx = sent_pkts[nf_idx] + 0;
        tcph->sent_seq = 0xdeadbeef;
        tcph->recv_ack = 0xFFFFFFFF;

        // assume there is only one port
        send_packet(cur_sending_pkt, 0);
      }
      continue;
    }

    // calculate unacked_pkts
    if (sent_pkts[nf_idx] < received_pkts[nf_idx]) {
      unack_pkts[nf_idx] = 0;
    } else {
      unack_pkts[nf_idx] = sent_pkts[nf_idx] - received_pkts[nf_idx];
    }

    // so many unacked packets, not sending load packets in this round.
    if (unack_pkts[nf_idx] >= MAX_UNACK_WINDOW) {
      // timeout triggered, force resolve
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

    // the number of maximum infligh packets is MAX_UNACK_WINDOW
    int burst_size = MAX_UNACK_WINDOW - unack_pkts[nf_idx];
    // maximum packets sent this round is limited to MAX_PKTS_PER_ROUND
    burst_size = std::min(burst_size, MAX_PKTS_PER_ROUND);

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

      // embedding packet sending timestamp into tcp header.
      auto time_p = (uint64_t *)((uint8_t *)tcph + 12);
      *time_p = this_iter_cycles_start;

      // 20B inter-pkt frame
      sent_pkts_size[nf_idx] += cur_sending_pkt->len + 20;

      // assume there is only one port
      send_packet(cur_sending_pkt, 0);
    }
    sent_pkts[nf_idx] += burst_size;

    // warm up phase ends
    if (sent_pkts[nf_idx] >= WARMUP_NPKTS && !warmup_end[nf_idx].load()) {
      warmup_end[nf_idx] = (uint8_t)1;
      start_gen_pkt_timestamp[nf_idx] = this_iter_cycles_start;
      sent_pkts_size[nf_idx] = 0;
    }

    if (sent_pkts[nf_idx] % PRINT_INTERVAL == 0) {
      fprintf(stdout,
              "[send_pacekts th%d]: pkts sent: %lu, unacked pkts: %lu, "
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
  struct ipv4_hdr *ipv4 =
      (struct ipv4_hdr *)(pkt_data + NET_IP_ALIGN + sizeof(struct ether_hdr));
  struct tcp_hdr *tcph =
      (struct tcp_hdr *)(pkt_data + NET_IP_ALIGN + sizeof(struct ether_hdr) +
                         sizeof(struct ipv4_hdr));

  int ether_type = (int)htons((eh_recv->ether_type));

  // filtering non-related packets
  if (!(ether_type >= CUSTOM_PROTO_BASE &&
        ether_type < CUSTOM_PROTO_BASE + 2 * NCORES)) {
    fprintf(stdout, "invalid packet ether_type: %hu tcph->sent_seq: %x\n",
            ether_type, tcph->sent_seq);
    return;
  }

  // processing boot packets.
  int nf_idx = ether_type - CUSTOM_PROTO_BASE;
  if (nf_idx >= NCORES) {
    nf_idx -= NCORES;
    if (!nf_readyness[nf_idx].test_and_set()) {
      num_nfs = htons(ipv4->packet_id);
      num_ready_nfs++;
      fprintf(stdout, "process_recv_packet recv one boot packet\n");
      if (num_ready_nfs == NCORES) {
        fprintf(stdout, "process_recv_packet recv all boot packets\n");
      }
    }
    return;
  }

  // filtering non-related packets
  if (tcph->sent_seq != 0xdeadbeef) {
    invalid_pkts++;
    return;
  }

  // receive an ack packet from this NF
  received_pkts[nf_idx]++;

  // indicating this NF is done
  if (received_pkts[nf_idx] >= WARMUP_NPKTS + TEST_NPKTS) {
    // first time arrive here
    if (!nf_finishness[nf_idx].test_and_set()) {
      auto print_stats = [&](int nf_idx) {
        finish_gen_pkt_timestamp[nf_idx] = this_iter_cycles_start;
        double time_taken = get_us(start_gen_pkt_timestamp[nf_idx],
                                   finish_gen_pkt_timestamp[nf_idx]);
        double avg = std::accumulate(latencies[nf_idx].begin(),
                                     latencies[nf_idx].end(), 0.0) /
                     latencies[nf_idx].size();
        std::sort(latencies[nf_idx].begin(), latencies[nf_idx].end());
        auto p50 = nth_percentile(latencies[nf_idx], 50);
        auto p75 = nth_percentile(latencies[nf_idx], 70);
        auto p90 = nth_percentile(latencies[nf_idx], 90);
        auto p99 = nth_percentile(latencies[nf_idx], 99);
        auto p99_9 = nth_percentile(latencies[nf_idx], 99.9);
        fprintf(
            stdout,
            "[send_pacekts th%d done]: pkts sent: %lu (warmup %lu), "
            "unacked pkts: %4lu, "
            "potentially lost pkts: %4lu, time_taken %.1lf us, %.8lf Mpps, "
            "%.6lfGbps\n",
            nf_idx, sent_pkts[nf_idx].load(), WARMUP_NPKTS,
            unack_pkts[nf_idx].load(), lost_pkts[nf_idx].load(), time_taken,
            (double)(sent_pkts[nf_idx].load() - WARMUP_NPKTS) / time_taken,
            (double)(sent_pkts_size[nf_idx].load()) * 8 / (time_taken * 1e3));
        fprintf(
            stdout,
            "                 latency: avg = %lf, p50 = %lf, p75 = %lf, p90 "
            "= %lf, p99 = %lf, p99.9 = %lf\n",
            avg, p50, p75, p90, p99, p99_9);
      };

#ifdef KEEP_RUNNING
      if (num_finished_nfs.fetch_add(1) == num_nfs - 1) {
        for (int i = 0; i < NCORES; i++) {
          // so that generate_load_packets() for this nf_idx will stop
          nf_recv_end_pkt[i] = 1;
          print_stats(i);
        }
      }
#else
      // so that generate_load_packets() for this nf_idx will stop
      nf_recv_end_pkt[nf_idx] = 1;
      print_stats(nf_idx);
#endif
    }
    return;
  }

  uint32_t pkt_idx = tcph->recv_ack;
  uint64_t curr_received_pkts = received_pkts[nf_idx];
  uint32_t lost_pkts = pkt_idx + 1 - curr_received_pkts;

  // calculate and record packet latency
  auto time_p = (uint64_t *)((uint8_t *)tcph + 12);
  auto start_cycle = *time_p;
  auto pkt_latency = get_us(start_cycle, this_iter_cycles_start);
  latencies[nf_idx].push_back(pkt_latency);
  if (latencies[nf_idx].size() % PRINT_INTERVAL == 0) {
    double avg = std::accumulate(latencies[nf_idx].begin(),
                                 latencies[nf_idx].end(), 0.0) /
                 latencies[nf_idx].size();
    fprintf(stdout, "total_pkt_cnt = %lu, avg pkt latency = %lf us\n",
            latencies[nf_idx].size(), avg);
  }

  // warm up phase ends
  if (pkt_idx >= WARMUP_NPKTS - 1 && !warmup_end_recv[nf_idx].load()) {
    warmup_end_recv[nf_idx] = (uint8_t)1;
    latencies[nf_idx].clear();
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
    nf_recv_end_pkt[i] = (uint8_t)0;
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
