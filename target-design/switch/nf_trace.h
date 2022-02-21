#ifndef __NF_TRACE_H__
#define __NF_TRACE_H__

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <time.h>
#include <string.h>

#define ETHER_ADDR_LEN  6 /**< Length of Ethernet address. */
#define ETHER_TYPE_LEN  2 /**< Length of Ethernet type field. */
#define NET_IP_ALIGN 2 /**< this is only used for icenet. */

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
	uint8_t  version_ihl;		/**< version and header length */
	uint8_t  type_of_service;	/**< type of service */
	uint16_t total_length;		/**< length of packet */
	uint16_t packet_id;		/**< packet ID */
	uint16_t fragment_offset;	/**< fragmentation offset */
	uint8_t  time_to_live;		/**< time to live */
	uint8_t  next_proto_id;		/**< protocol ID */
	uint16_t hdr_checksum;		/**< header checksum */
	uint32_t src_addr;		/**< source address */
	uint32_t dst_addr;		/**< destination address */
} __attribute__((__packed__));


struct tcp_hdr {
	uint16_t src_port;  /**< TCP source port. */
	uint16_t dst_port;  /**< TCP destination port. */
	uint32_t sent_seq;  /**< TX data sequence number. */
	uint32_t recv_ack;  /**< RX data acknowledgement sequence number. */
	uint8_t  data_off;  /**< Data offset. */
	uint8_t  tcp_flags; /**< TCP flags */
	uint16_t rx_win;    /**< RX flow control window. */
	uint16_t cksum;     /**< TCP checksum. */
	uint16_t tcp_urp;   /**< TCP urgent pointer, if any. */
} __attribute__((__packed__));

typedef struct _pkt {
    uint16_t len;
    uint8_t* content;
} pkt_t;

#define PKT_NUM (100 * 1024)
#define TOTAL_SEQ (1 << 21) // 2*1024*1024

pkt_t pkts[PKT_NUM];
uint32_t seqs[TOTAL_SEQ];

void load_pkt(char *filename){
    if(strcmp(filename, "/tmp/ictf2010_100kflow.dat") != 0){
        printf("pkt_puller3 must use trace of /tmp/ictf2010_100kflow.dat\n");
        exit(0);
    }

    printf("trying to open file %s\n", "/tmp/ictf2010_100kflow.dat");
    FILE * file = fopen("/tmp/ictf2010_100kflow.dat", "r");

    printf("trying to open file %s\n", "/tmp/ictf2010_100kflow_2mseq.dat");
    FILE * file_seq = fopen("/tmp/ictf2010_100kflow_2mseq.dat", "r");
    
    if(file == NULL || file_seq == NULL){
        printf("file open error\n");
        exit(0);
    }
    printf("opening succeeds\n");

    uint32_t pkt_cnt = 0;
    uint32_t pkt_size = 0;
    uint32_t seq = 0;
    while(1){
        fread(&pkts[pkt_cnt].len, sizeof(uint16_t), 1, file);
        pkt_size += pkts[pkt_cnt].len;
        pkts[pkt_cnt].content = (uint8_t *)malloc(pkts[pkt_cnt].len + NET_IP_ALIGN);
        fread(pkts[pkt_cnt].content + NET_IP_ALIGN, 1, pkts[pkt_cnt].len, file);
        pkts[pkt_cnt].len += NET_IP_ALIGN;
        pkt_cnt ++;
        if(pkt_cnt == PKT_NUM){
            break;
        }
    }
    uint32_t cnt = 0;
    while(cnt < TOTAL_SEQ){
        fread(&seqs[cnt], sizeof(uint32_t), 1, file_seq);
        cnt ++;
    }
    fclose(file);
    fclose(file_seq);
    printf("Reading pkt trace done!\n");
    printf("average pkt size = %lf\n", pkt_size * 1.0 / PKT_NUM);
}

uint32_t seq_idx[7] = {0};
pkt_t *next_pkt(uint8_t nf_idx){
    pkt_t* curr_pkt = pkts + seqs[seq_idx[nf_idx]];
    seq_idx[nf_idx] = (seq_idx[nf_idx] + 1) & (TOTAL_SEQ - 1);
    return curr_pkt;
}

#endif /* __NF_TRACE_H__ */
