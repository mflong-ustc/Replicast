#include "macro.p4"
#include "util.p4"
// ---------------------------------------------------------------------------
// Ingress parser
// ---------------------------------------------------------------------------
parser SwitchIngressParser(
        packet_in pkt,
        out header_t hdr,
        out ingress_metadata_t ig_md,
        out ingress_intrinsic_metadata_t ig_intr_md) {
    TofinoIngressParser() tofino_parser;

    state start {
        //initialize metadata here
        ig_md.flow_idx=0;
        ig_md.hash_idx=0;
        ig_md.addr_1=0;
        ig_md.addr_2=0;
        ig_md.app_port=0;

        //public
        ig_md.is_WRITE=0;


        // DstToR
        ig_md.psn=0;    //PSN
        ig_md.epsn=0;   //ePSN
        ig_md.dif=0;
        ig_md.psn_compare_result=0;
        // ig_md.pair.first=0;
        // ig_md.pair.second=0;
        ig_md.bitmap=0;    //bitmap
        ig_md.bitmap_1=0;
        ig_md.bitmap_2=0;
        ig_md.bitmap_3=0;
        ig_md.bitmap_4=0;
        ig_md.bitmap_lowest=0;
        ig_md.bitmap_idx=0;
        ig_md.bitmap_delta=0;
        ig_md.bitmap_delta_after_mapping=0;

        ig_md.bitmap_pos=0;
        ig_md.bitmap_bit=0;
        ig_md.bitmap_mask=0;
        
        ig_md.buffer_occupied=0;
        ig_md.tenary_state=0;   //tenary state machine  00:ordered; 01:ReTx; 10:waiting;
        ig_md.request_timer=0;  //Timer for retransmission-request sending.
        ig_md.send_request_result=0;

        ig_md.ing_mir_ses=0;
        ig_md.pkt_type=0;
        ig_md.mirror_1=0;
        ig_md.mirror_2=0;
        // ig_md.mirror_type=0;


        //SrcToR
        ig_md.release_result=0;
        // ig_md.bitmap_is_0=0;
        ig_md.request_state=0;
        ig_md.bitmap_flag=0;
        ig_md.replica_hit=0;
        ig_md.replica_hit_with_lost_psn=0;
        ig_md.lost_psn=0;
        ig_md.bitmap_and=0;
        ig_md.bitmap_word=0;
        ig_md.epsn_is_zero=0;
        ig_md.bitmap_is_set=0;

        ig_md.replica_occupied=0;
        ig_md.all_replica_occupied=0;
        ig_md.replica_pool_is_full=0;

        ig_md.send_unfulfilled_result=0;
        // ig_md.request_is_fulfilled=0;
        

        tofino_parser.apply(pkt,ig_intr_md);
        transition parse_ethernet;
    }

    state parse_ethernet {
        pkt.extract(hdr.ethernet);
        transition select(hdr.ethernet.ether_type){
            (bit<16>)ETHERTYPE_IPV4: parse_ipv4;
			(bit<16>)ETHERTYPE_ARP: parse_arp;
            default: accept;
        }
    }
    
    state parse_arp {
        pkt.extract(hdr.arp);
        transition accept;
    }

    state parse_ipv4 {
        pkt.extract(hdr.ipv4);
        transition select(hdr.ipv4.protocol){
            // 0x1:parse_icmp;
            0x6:parse_tcp;
            0x11:parse_udp;
            default:accept;
        }
    }
    // parse_icmp{
    //     pkt.extract(hdr.icmp);
    //     transition accept;
    // }
    state parse_tcp{
        pkt.extract(hdr.tcp);
        transition accept;
    }
    state parse_udp{
        pkt.extract(hdr.udp);
        transition select(hdr.udp.dst_port){
			ROCEV2_UDP_PORT: parse_rocev2_bth;
			default: accept;
		}
    }
    state parse_rocev2_bth{
		pkt.extract(hdr.bth);

        transition select(hdr.bth.opcode){
            0x11: parse_aeth;   //Ack/NAK
            0x1e: parse_bitmap_eth;
            default:accept;
        }

		// transition accept;
	}

    state parse_aeth{
        pkt.extract(hdr.aeth);
        transition accept;
    }

    state parse_bitmap_eth{
        pkt.extract(hdr.bitmap_eth);
        transition accept;
    }


}

// ---------------------------------------------------------------------------
// Ingress Deparser
// ---------------------------------------------------------------------------
control SwitchIngressDeparser(
        packet_out pkt,
        inout header_t hdr,
        in ingress_metadata_t ig_md,
        in ingress_intrinsic_metadata_for_deparser_t ig_dprsr_md) {

    Mirror() mirror;
    Checksum() ipv4_checksum;

    apply {
        if(ig_dprsr_md.mirror_type == MIRROR_TYPE_I2E_1){   //request construct
            mirror.emit<mirror_h>(ig_md.ing_mir_ses,{ig_md.pkt_type,ig_md.mirror_1,ig_md.mirror_2});  //carry epsn and bitmap to construct retx request
        }else if(ig_dprsr_md.mirror_type == MIRROR_TYPE_I2E){   //replica, unfulfilled
            mirror.emit<mirror_bridged_metadata_h>(ig_md.ing_mir_ses,{ig_md.pkt_type});
        }
            

        hdr.ipv4.hdr_checksum = ipv4_checksum.update({
            hdr.ipv4.version,
            hdr.ipv4.ihl,
            hdr.ipv4.diffserv,
            hdr.ipv4.total_len,
            hdr.ipv4.identification,
            hdr.ipv4.flags,
            hdr.ipv4.frag_offset,
            hdr.ipv4.ttl,
            hdr.ipv4.protocol,
            hdr.ipv4.src_addr,
            hdr.ipv4.dst_addr});

         pkt.emit(hdr);
    }
}

parser SwitchEgressParser(
        packet_in pkt,
        out header_t hdr,
        out egress_metadata_t eg_md,
        out egress_intrinsic_metadata_t eg_intr_md){
    state start {
        //initialize here
        eg_md.mac_addr=0;
        eg_md.ipv4_addr=0;
        eg_md.pkt_type=0;
        eg_md.mirror_1=0;
        eg_md.mirror_2=0;

        eg_md.random_num=0;
        eg_md.random_loss=0;
        eg_md.exceeded_ecn_marking_threshold=0;

        pkt.extract(eg_intr_md);
        transition parse_metadata;
        // transition accept;
    }

    state parse_metadata{
        mirror_h mirror_md=pkt.lookahead<mirror_h>();
        eg_md.pkt_type=mirror_md.pkt_type;  //get pkt type
        transition select(mirror_md.pkt_type){
            PKT_TYPE_NORMAL: parse_bridged_md;
            PKT_TYPE_MIRROR: parse_mirror_md;
            PKT_TYPE_UNFULFILLED: parse_bridged_md; //constructed from retransmission request
            default: accept;
        }
    }

    state parse_bridged_md{
        mirror_bridged_metadata_h bridged_md;
        pkt.extract(bridged_md);
        transition parse_ethernet;
    }

    state parse_mirror_md{
        mirror_h mirror_md;
        pkt.extract(mirror_md);
        eg_md.mirror_1=mirror_md.epsn;  //get request-epsn
        eg_md.mirror_2=mirror_md.bitmap;//get request-bitmap
        transition parse_ethernet;
    }

    state parse_ethernet {
        pkt.extract(hdr.ethernet);
        transition select(hdr.ethernet.ether_type){
            (bit<16>)ETHERTYPE_IPV4: parse_ipv4;
            (bit<16>)ETHERTYPE_ARP: parse_arp;
            default: accept;
        }
    }
    
    state parse_arp {
        pkt.extract(hdr.arp);
        transition accept;
    }

    state parse_ipv4 {
        pkt.extract(hdr.ipv4);
        transition select(hdr.ipv4.protocol){
            // 0x1:parse_icmp;
            0x6:parse_tcp;
            0x11:parse_udp;
            default:accept;
        }
    }
    // parse_icmp{
    //     pkt.extract(hdr.icmp);
    //     transition accept;
    // }
    state parse_tcp{
        pkt.extract(hdr.tcp);
        transition accept;
    }
    state parse_udp{
        pkt.extract(hdr.udp);
        transition select(hdr.udp.dst_port){
            ROCEV2_UDP_PORT: parse_rocev2_bth;
            default: accept;
        }
    }

    state parse_rocev2_bth{
        pkt.extract(hdr.bth);

        transition select(hdr.bth.opcode){
            0x11: parse_aeth;   //Ack/NAK
            0x1e: parse_bitmap_eth;
            default:accept;
        }

        // transition accept;
    }

    state parse_aeth{
        pkt.extract(hdr.aeth);
        transition accept;
    }

    state parse_bitmap_eth{
        pkt.extract(hdr.bitmap_eth);
        transition accept;
    }
}

control SwitchEgressDeparser(
    packet_out pkt,
    inout header_t hdr,
    in egress_metadata_t eg_md,
    // in egress_intrinsic_metadata_t eg_intr_md,  //error
    // in egress_intrinsic_metadata_from_parser_t eg_intr_md_from_prsr
    in egress_intrinsic_metadata_for_deparser_t eg_dprse_md

    ) {
    apply {
        pkt.emit(hdr);
    }
}