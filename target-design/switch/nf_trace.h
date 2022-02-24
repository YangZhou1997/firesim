#ifndef __NF_TRACE_H__
#define __NF_TRACE_H__

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "ictf2010_100kflow.h"
#include "ictf2010_100kflow_2mseq.h"

// for easy make
// const unsigned long long fsize_ictf2010_100kflow_dat = 62389731L;
// const unsigned char file_ictf2010_100kflow_dat[] = {
// 0x6f,0x00,0x00,0x8c,0xfa,0xf7,0x36,0x30,0x00,0x8c,0xfa,0xf5,0x20,0xb0,0x08,0x00,
// 0x45,0x10,0x00,0x61,0xa3,0xcb,0x40,0x00,0x3e,0x06,0x00,0xd7,0x0a,0x0d,0x7e,0xf6,
// 0x0a,0x10,0x00,0x0a,0xc6,0x96,0x00,0x50,0xde,0x8f,0x5e,0xa6,0xff,0xfd,0x3f,0x5a,
// 0x50,0x18,0x05,0xb4,0xb7,0x50,0x00,0x00,0x3a,0x63,0x61,0x76,0x65,0x64,0x6f,0x6e,
// 0x21,0x63,0x61,0x76,0x65,0x64,0x6f,0x6e,0x40,0x73,0x65,0x63,0x6c,0x61,0x62,0x2e,
// 0x63,0x73,0x2e,0x75,0x63,0x73,0x62,0x2e,0x65,0x64,0x75,0x20,0x4d,0x4f,0x44,0x45,
// 0x20,0x23,0x69,0x63,0x74,0x66,0x20,0x2b,0x6f,0x20,0x61,0x64,0x61,0x6d,0x64,0x0d};

// const unsigned long long fsize_ictf2010_100kflow_2mseq_dat = 8388608L;
// const unsigned char file_ictf2010_100kflow_2mseq_dat[] = {
// 0x76,0x5a,0x01,0x00,0xc1,0x8f,0x01,0x00,0xaa,0x75,0x01,0x00,0x26,0x70,0x01,0x00,
// 0xfc,0x0c,0x01,0x00,0xfa,0x8f,0x01,0x00,0xe0,0x8f,0x01,0x00,0x17,0x7a,0x01,0x00,
// 0xf0,0x8f,0x01,0x00,0x63,0x8e,0x01,0x00,0x5a,0x8f,0x01,0x00,0x06,0x8c,0x01,0x00,
// 0xd4,0x8f,0x01,0x00,0x02,0x8f,0x01,0x00,0x84,0xb5,0x00,0x00,0x45,0x05,0x01,0x00,
// 0xae,0x8b,0x01,0x00,0x4d,0x84,0x01,0x00,0xfd,0x8f,0x01,0x00,0xf3,0x8c,0x01,0x00,
// 0xff,0x8f,0x01,0x00,0xf5,0x8f,0x01,0x00,0xfd,0x8f,0x01,0x00,0xcc,0x6d,0x01,0x00,
// 0xfc,0x8f,0x01,0x00,0xbc,0x8f,0x01,0x00,0xfd,0x8f,0x01,0x00,0xfe,0x8f,0x01,0x00};

#define ETHER_ADDR_LEN 6 /**< Length of Ethernet address. */
#define ETHER_TYPE_LEN 2 /**< Length of Ethernet type field. */
#define NET_IP_ALIGN 2   /**< this is only used for icenet. */

#define ETH_MAX_WORDS 190
#define ETH_HEADER_SIZE 14
#define MAC_ADDR_SIZE 6
#define IP_ADDR_SIZE 4

struct ether_addr {
  uint8_t addr_bytes[ETHER_ADDR_LEN]; /**< Addr bytes in tx order */
} __attribute__((__packed__));

struct ether_hdr {
  struct ether_addr d_addr; /**< Destination address. */
  struct ether_addr s_addr; /**< Source address. */
  uint16_t ether_type;      /**< Frame type. */
} __attribute__((__packed__));

struct ipv4_hdr {
  uint8_t version_ihl;      /**< version and header length */
  uint8_t type_of_service;  /**< type of service */
  uint16_t total_length;    /**< length of packet */
  uint16_t packet_id;       /**< packet ID */
  uint16_t fragment_offset; /**< fragmentation offset */
  uint8_t time_to_live;     /**< time to live */
  uint8_t next_proto_id;    /**< protocol ID */
  uint16_t hdr_checksum;    /**< header checksum */
  uint32_t src_addr;        /**< source address */
  uint32_t dst_addr;        /**< destination address */
} __attribute__((__packed__));

struct tcp_hdr {
  uint16_t src_port; /**< TCP source port. */
  uint16_t dst_port; /**< TCP destination port. */
  uint32_t sent_seq; /**< TX data sequence number. */
  uint32_t recv_ack; /**< RX data acknowledgement sequence number. */
  uint8_t data_off;  /**< Data offset. */
  uint8_t tcp_flags; /**< TCP flags */
  uint16_t rx_win;   /**< RX flow control window. */
  uint16_t cksum;    /**< TCP checksum. */
  uint16_t tcp_urp;  /**< TCP urgent pointer, if any. */
} __attribute__((__packed__));

typedef struct _pkt {
  uint16_t len;
  uint8_t *content;
} pkt_t;

#define PKT_NUM (100 * 1024)
#define TOTAL_SEQ (1 << 21)  // 2*1024*1024

pkt_t pkts[PKT_NUM];
uint32_t seqs[TOTAL_SEQ];

// the following is to facilitate parsing embedded file.h
typedef struct {
  const unsigned char* file_data;
  unsigned long long file_size;
  unsigned long long cur_read_pos;
} MY_FILE;

void init_my_fread(const unsigned char* file_data, unsigned long long file_size,
                   MY_FILE* file) {
  file->file_data = file_data;
  file->file_size = file_size;
  file->cur_read_pos = 0;
}

size_t my_fread(void* ptr, size_t size, size_t n, MY_FILE* file) {
  size_t read_size = size * n;
  if (read_size + file->cur_read_pos > file->file_size) {
    return 0;
  }
  memcpy(ptr, file->file_data + file->cur_read_pos, read_size);
  file->cur_read_pos += read_size;
  return read_size;
}

void load_pkt() {
  MY_FILE file, file_seq;
  init_my_fread(file_ictf2010_100kflow_dat, fsize_ictf2010_100kflow_dat, &file);
  init_my_fread(file_ictf2010_100kflow_2mseq_dat, fsize_ictf2010_100kflow_2mseq_dat, &file_seq);
  printf("opening succeeds\n");

  uint32_t pkt_cnt = 0;
  uint32_t pkt_size = 0;
  uint32_t seq = 0;
  while (1) {
    my_fread(&pkts[pkt_cnt].len, sizeof(uint16_t), 1, &file);
    pkt_size += pkts[pkt_cnt].len;
    pkts[pkt_cnt].content = (uint8_t *)malloc(pkts[pkt_cnt].len + NET_IP_ALIGN);
    my_fread(pkts[pkt_cnt].content + NET_IP_ALIGN, 1, pkts[pkt_cnt].len, &file);
    pkts[pkt_cnt].len += NET_IP_ALIGN;
    pkt_cnt++;
    if (pkt_cnt == PKT_NUM) {
      break;
    }
  }
  uint32_t cnt = 0;
  while (cnt < TOTAL_SEQ) {
    my_fread(&seqs[cnt], sizeof(uint32_t), 1, &file_seq);
    cnt++;
  }
  printf("Reading pkt trace done!\n");
  printf("average pkt size = %lf\n", pkt_size * 1.0 / PKT_NUM);
}

uint32_t seq_idx[7] = {0};
pkt_t *next_pkt(uint8_t nf_idx) {
  pkt_t *curr_pkt = pkts + seqs[seq_idx[nf_idx]];
  seq_idx[nf_idx] = (seq_idx[nf_idx] + 1) & (TOTAL_SEQ - 1);
  return curr_pkt;
}

#endif /* __NF_TRACE_H__ */
