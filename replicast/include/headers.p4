#ifndef _HEADERS_
#define _HEADERS_
#include "macro.p4"

typedef bit<8>  pkt_type_t;
const pkt_type_t PKT_TYPE_NORMAL = 1;   //
const pkt_type_t PKT_TYPE_MIRROR = 2;
const pkt_type_t PKT_TYPE_UNFULFILLED = 3;


#if __TARGET_TOFINO__ == 1
typedef bit<3> mirror_type_t;
#else
typedef bit<4> mirror_type_t;
#endif
const mirror_type_t MIRROR_TYPE_I2E = 1;    //replica or unfulfilled notification
const mirror_type_t MIRROR_TYPE_E2E = 2;
const mirror_type_t MIRROR_TYPE_I2E_1 =3;   //mirror to construct request

typedef bit<48> mac_addr_t;
typedef bit<32> ipv4_addr_t;
typedef bit<128> ipv6_addr_t;
typedef bit<12> vlan_id_t;

typedef bit<16> ether_type_t;
const ether_type_t ETHERTYPE_IPV4 = 16w0x0800;
const ether_type_t ETHERTYPE_ARP = 16w0x0806;
const ether_type_t ETHERTYPE_IPV6 = 16w0x86dd;
const ether_type_t ETHERTYPE_VLAN = 16w0x8100;

typedef bit<8> ip_protocol_t;
const ip_protocol_t IP_PROTOCOLS_ICMP = 1;
const ip_protocol_t IP_PROTOCOLS_TCP = 6;
const ip_protocol_t IP_PROTOCOLS_UDP = 17;\

typedef  bit<16> W;

header ethernet_h {
    mac_addr_t dst_addr;
    mac_addr_t src_addr;
    bit<16> ether_type;
}

header vlan_tag_h {
    bit<3> pcp;
    bit<1> cfi;
    vlan_id_t vid;
    bit<16> ether_type;
}

header mpls_h {
    bit<20> label;
    bit<3> exp;
    bit<1> bos;
    bit<8> ttl;
}

header ipv4_h {
    bit<4> version;
    bit<4> ihl;
    bit<8> diffserv;
    bit<16> total_len;
    bit<16> identification;
    bit<3> flags;
    bit<13> frag_offset;
    bit<8> ttl;
    bit<8> protocol;
    bit<16> hdr_checksum;
    ipv4_addr_t src_addr;
    ipv4_addr_t dst_addr;
}

header ipv6_h {
    bit<4> version;
    bit<8> traffic_class;
    bit<20> flow_label;
    bit<16> payload_len;
    bit<8> next_hdr;
    bit<8> hop_limit;
    ipv6_addr_t src_addr;
    ipv6_addr_t dst_addr;
}

header tcp_h {
    bit<16> src_port;
    bit<16> dst_port;
    bit<32> seq_no;
    bit<32> ack_no;
    bit<4> data_offset;
    bit<4> res;
    bit<8> flags;
    bit<16> window;
    bit<16> checksum;
    bit<16> urgent_ptr;
}
header udp_h {
    bit<16> src_port;
    bit<16> dst_port;
    bit<16> hdr_length;
    bit<16> checksum;
}

header icmp_h {
    bit<8> type;
    bit<8> code;
    bit<16> hdr_checksum;
}

// Address Resolution Protocol -- RFC 6747
header arp_h {
    bit<16> hw_type;
    bit<16> proto_type;
    bit<8> hw_addr_len;
    bit<8> proto_addr_len;
    bit<16> opcode;
    mac_addr_t sender_hw_addr;
    ipv4_addr_t sender_ip_addr;
    mac_addr_t target_hw_addr;
    ipv4_addr_t target_ip_addr;
}

// Segment Routing Extension (SRH) -- IETFv7
header ipv6_srh_h {
    bit<8> next_hdr;
    bit<8> hdr_ext_len;
    bit<8> routing_type;
    bit<8> seg_left;
    bit<8> last_entry;
    bit<8> flags;
    bit<16> tag;
}

// VXLAN -- RFC 7348
header vxlan_h {
    bit<8> flags;
    bit<24> reserved;
    bit<24> vni;
    bit<8> reserved2;
}

// Generic Routing Encapsulation (GRE) -- RFC 1701
header gre_h {
    bit<1> C;
    bit<1> R;
    bit<1> K;
    bit<1> S;
    bit<1> s;
    bit<3> recurse;
    bit<5> flags;
    bit<3> version;
    bit<16> proto;
}


//roce
//12 bytes
header infiniband_bth_h
{
	bit<8> opcode;
	bit<1> solicitedEvent;
	bit<1> migReq;
	bit<2> padCount;
	bit<4> transportHeaderVersion;
	bit<16> partitionKey;
	bit<1> fRes;
	bit<1> bRes;
	bit<6> reserved1;
	bit<24> destinationQP;
	bit<1> ackRequest;
	bit<7> reserved2;
	bit<24> packetSequenceNumber;   //PSN
}

//See Infiniband spec page 254
//16 bytes
header infiniband_reth_h
{
	bit<64> virtualAddress;
	bit<32> rKey;
	bit<32> dmaLength;
}

header infiniband_aeth_h
{
    bit<1> reserved;
    bit<2> opcode;      // (0: ACK, 3: NACK)
    bit<5> error_code;  // (PSN SEQ ERROR)
    bit<8> msg_seq_number;
}

//See Infiniband spec page 242
header infiniband_atomiceth_h
{
	bit<64> virtualAddress;
	bit<32> rKey;
	bit<64> data;
	bit<64> compare;
}

header bitmap_eth_h   //replicast: Extender bitmap header
{
    bit<128> bitmap;
}

header mirror_h{
    pkt_type_t pkt_type;
    bit<32> epsn;
    bit<128> bitmap;
}

@flexible
header mirror_bridged_metadata_h {
    pkt_type_t pkt_type;
}

struct header_t {
    mirror_bridged_metadata_h bridged_md;
    ethernet_h ethernet;
    arp_h arp;
    ipv4_h ipv4;
    tcp_h tcp;
    udp_h udp;

    // Add more headers here.
    /*** RoCEv2 ***/
    infiniband_bth_h bth;
	infiniband_aeth_h aeth;
    bitmap_eth_h    bitmap_eth;
}
// struct pair_t{
//     bit<128> first;
//     bit<8> second;
// }

struct ingress_metadata_t {         //replicast: my ingress header
    bit<32> flow_idx;
    bit<12> hash_idx;
    ipv4_addr_t addr_1;
    ipv4_addr_t addr_2;
    bit<16> app_port;

    //Public
    bit<24> psn;    //PSN
    bit<1> epsn_is_zero;
    bit<24> epsn;   //ePSN    
    bit<128> bitmap;    //bitmap
    bit<32> bitmap_1;   //bitmap[127:96]
    bit<32> bitmap_2;   //bitmap[95:64]
    bit<32> bitmap_3;   //bitmap[63:32]
    bit<32> bitmap_4;   //bitmap[31:0]
    bit<1> bitmap_lowest;
    bit<8> request_timer;  //Timer for retransmission-request sending.

    bit<8> is_WRITE;

    //  DstToR
    bit<24> dif;
    bit<24> psn_compare_result;

    bit<1> bitmap_idx;  //for bitmap >> 1
    bit<24> bitmap_delta;
    bit<128> bitmap_delta_after_mapping;

    bit<7>   bitmap_pos;          // 0..127: absolute bit position
    bit<5>   bitmap_bit;          // 0..31: bit within the 32-bit word (mask table key)
    bit<32>  bitmap_mask;         // 1 << bitmap_bit
    
    bit<16> buffer_occupied;
    bit<2> tenary_state;   //tenary state machine  00:ordered; 01:ReTx; 10:waiting;

    bit<8> send_request_result;

    //mirror
    MirrorId_t ing_mir_ses;
    pkt_type_t pkt_type;
    bit<32> mirror_1;   //epsn  
    bit<128> mirror_2;  //bitmap
    // bit<2> mirror_type; //back_up or request

    //  SrcToR
    bit<8> release_result;
    // bit<8> bitmap_is_0;
    bit<8> request_state;
    bit<8> bitmap_flag; //point to the latest bitmap location
    // bit<8> bitmap_process_result;
    bit<32> replica_hit_with_lost_psn;
    bit<8> replica_hit; 
    bit<24> lost_psn;
    bit<1> bitmap_is_set;
    // bit<128> bitmap_mask128;      // 1 << pos (Src-side variable bit check)
    // bit<128> bitmap_clear_mask128; // ~(1 << pos) (precomputed by control plane)
    bit<32> bitmap_and;
    bit<2> bitmap_word;

    bit<16> replica_occupied;
    bit<16> all_replica_occupied;
    bit<8> replica_pool_is_full;

    bit<8> send_unfulfilled_result; 
    
}

struct egress_metadata_t {
    mac_addr_t mac_addr;
    ipv4_addr_t ipv4_addr;

    //mirror
    pkt_type_t pkt_type;
    bit<32> mirror_1;   //epsn  
    bit<128> mirror_2;  //bitmap

    bit<16> random_num;
    bit<8> random_loss;
    bit<8> exceeded_ecn_marking_threshold;

}
struct empty_header_t {}

struct empty_metadata_t {}
#endif /* _HEADERS_ */
