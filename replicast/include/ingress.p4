#include <core.p4>
#if __TARGET_TOFINO__ == 2
#include <t2na.p4>
#else
#include <tna.p4>
#endif

control get_flow_idx(
    inout header_t hdr,
    inout ingress_metadata_t ig_md
){
    Hash<bit<32>>(HashAlgorithm_t.CRC32) hash_crc32;
    apply{
        if(hdr.aeth.isValid()){           //ACK/NAK
            ig_md.addr_1=hdr.ipv4.dst_addr;
            ig_md.addr_2=hdr.ipv4.src_addr;
            ig_md.app_port=hdr.udp.src_port;
        }else{  //DATA
            ig_md.addr_1=hdr.ipv4.src_addr;
            ig_md.addr_2=hdr.ipv4.dst_addr;
            ig_md.app_port=hdr.udp.src_port;
        }

        ig_md.flow_idx = (bit<32>)hash_crc32.get({ ig_md.addr_1, ig_md.addr_2,ig_md.app_port});
        // ig_md.flow_idx = ig_md.flow_idx |+| 1;
        
        ig_md.hash_idx=(bit<12>)(ig_md.flow_idx & 0xfff);

    }
}

control Dst_SwitchIngress(
        inout header_t hdr,
        inout ingress_metadata_t ig_md,
        // inout pair_t pair,
        in ingress_intrinsic_metadata_t ig_intr_md,
        in ingress_intrinsic_metadata_from_parser_t ig_prsr_md,
        inout ingress_intrinsic_metadata_for_deparser_t ig_dprsr_md,
        inout ingress_intrinsic_metadata_for_tm_t ig_tm_md) {
#include "ingress_dst_registers.p4"
#include "ingress_actions.p4"
// #include "ingress_tables.p4"
    table ipv4_host {
        key = { hdr.ipv4.dst_addr : exact; }
        actions = {
            send; drop;
        }
        size = 1024;
    }

    table arp_forward{
        key={
            hdr.arp.target_ip_addr:exact;
        }
        actions={
            send; drop;
        }
        size=1024;
    }

    apply{
        if(hdr.arp.isValid()){
            arp_forward.apply();
            return;
        }
        else if (hdr.ipv4.isValid()) {
            ipv4_host.apply();
            if(!hdr.bth.isValid()){     //Only processing RoCEv2 packets.
                return; 
            }else{
                if(hdr.bth.opcode==0x06 || hdr.bth.opcode==0x07 || hdr.bth.opcode==0x08){   //write_first, write_middle, write_last
                    ig_md.is_WRITE=1;
                }
                if(hdr.bth.opcode==0x11 || ig_md.is_WRITE==1 || hdr.bth.opcode==0x1f){   //ack, write, unfulfilled notification
                    get_flow_idx.apply(hdr,ig_md);

                    /****************************************************************************
                    *                         UNFULFILLED NOTIFICATION
                    ****************************************************************************/
                    if(hdr.bth.opcode==0x1f){
                        // reset_bitmap.execute(ig_md.hash_idx);   //reset bitmap to 0
                        // reset_w0.execute(ig_md.hash_idx);
                        // reset_w1.execute(ig_md.hash_idx);
                        // reset_w2.execute(ig_md.hash_idx);
                        // reset_w3.execute(ig_md.hash_idx);
                        set_state_to_waiting.execute(ig_md.hash_idx);
                        drop();
                    }

                    /****************************************************************************
                    *                                   DATA
                    ****************************************************************************/
                    else if(ig_md.is_WRITE==1){
                        ig_md.psn=hdr.bth.packetSequenceNumber;
                        ig_md.epsn=epsn_get.execute(ig_md.hash_idx);    //get epsn first, then epsn+1 if psn=epsn or epsn=1(first pkt)
                        if(ig_md.epsn==1){ig_md.epsn=ig_md.psn;}    //first pkt
                        // ig_md.dif=ig_md.psn-ig_md.epsn;     //condition too complex, limit of 4 bytes + 12  bits of PHV input exceeded
                        psn_difference();
                        if((bit<12>)ig_md.dif<(bit<12>)MAX_VALUE){  //not overflow: psn > epsn
                            ig_md.psn_compare_result=1;
                        }
                        
                        ig_md.bitmap_pos = (bit<7>)(ig_md.psn);
                        ig_md.bitmap_bit = (bit<5>)(ig_md.psn);

                        if(ig_md.psn_compare_result==1){
                            ig_md.tenary_state=set_state_to_retx.execute(ig_md.hash_idx);   //(compile skill)
                        }

                        // if(ig_md.dif==0){
                        //     ig_md.bitmap_pos = (bit<7>)(ig_md.psn);
                        //     ig_md.bitmap_bit = (bit<5>)(ig_md.psn);
                        //     bitmap_clear_mask_map.apply();
                        //     if(ig_md.bitmap_pos[6:5] == 0){ ig_md.bitmap_4 = clear_w0.execute(ig_md.hash_idx); }
                        //     else if(ig_md.bitmap_pos[6:5] == 1){ ig_md.bitmap_3 = clear_w1.execute(ig_md.hash_idx); }
                        //     else if(ig_md.bitmap_pos[6:5] == 2){ ig_md.bitmap_2 = clear_w2.execute(ig_md.hash_idx); }
                        //     else { ig_md.bitmap_1 = clear_w3.execute(ig_md.hash_idx); }
                        // }
                        // else if(ig_md.psn_compare_result==1){
                        //     ig_md.tenary_state=set_state_to_retx.execute(ig_md.hash_idx);   //(compile skill)
                        //     if(ig_intr_md.ingress_port!=(bit<9>) (OOO_PORT)){   //&& ig_md.tenary_state==1
                        //         // send_to_reordering_pool(); 
                        //         // mark lost bit (unconditional on state: breaks the state<->bitmap dependency cycle)
                        //         ig_md.bitmap_pos = (bit<7>)ig_md.psn;
                        //         ig_md.bitmap_bit = (bit<5>)ig_md.psn;
                        //         bitmap_mask_map.apply();
                        //         if(ig_md.bitmap_pos[6:5] == 0){ set_w0.execute(ig_md.hash_idx); }
                        //         else if(ig_md.bitmap_pos[6:5] == 1){ set_w1.execute(ig_md.hash_idx); }
                        //         else if(ig_md.bitmap_pos[6:5] == 2){ set_w2.execute(ig_md.hash_idx); }
                        //         else { set_w3.execute(ig_md.hash_idx); }
                        //     }
                        // }

                        /* data from reordering pool*/
                        if(ig_intr_md.ingress_port==(bit<9>) (OOO_PORT)){
                            if(ig_md.dif==0){
                                if(ig_md.bitmap_bit == 0){ set_mask_0(); } else if(ig_md.bitmap_bit == 1){ set_mask_1(); } else if(ig_md.bitmap_bit == 2){ set_mask_2(); } else if(ig_md.bitmap_bit == 3){ set_mask_3(); } else if(ig_md.bitmap_bit == 4){ set_mask_4(); } else if(ig_md.bitmap_bit == 5){ set_mask_5(); } else if(ig_md.bitmap_bit == 6){ set_mask_6(); } else if(ig_md.bitmap_bit == 7){ set_mask_7(); } else if(ig_md.bitmap_bit == 8){ set_mask_8(); } else if(ig_md.bitmap_bit == 9){ set_mask_9(); } else if(ig_md.bitmap_bit == 10){ set_mask_10(); } else if(ig_md.bitmap_bit == 11){ set_mask_11(); } else if(ig_md.bitmap_bit == 12){ set_mask_12(); } else if(ig_md.bitmap_bit == 13){ set_mask_13(); } else if(ig_md.bitmap_bit == 14){ set_mask_14(); } else if(ig_md.bitmap_bit == 15){ set_mask_15(); } else if(ig_md.bitmap_bit == 16){ set_mask_16(); } else if(ig_md.bitmap_bit == 17){ set_mask_17(); } else if(ig_md.bitmap_bit == 18){ set_mask_18(); } else if(ig_md.bitmap_bit == 19){ set_mask_19(); } else if(ig_md.bitmap_bit == 20){ set_mask_20(); } else if(ig_md.bitmap_bit == 21){ set_mask_21(); } else if(ig_md.bitmap_bit == 22){ set_mask_22(); } else if(ig_md.bitmap_bit == 23){ set_mask_23(); } else if(ig_md.bitmap_bit == 24){ set_mask_24(); } else if(ig_md.bitmap_bit == 25){ set_mask_25(); } else if(ig_md.bitmap_bit == 26){ set_mask_26(); } else if(ig_md.bitmap_bit == 27){ set_mask_27(); } else if(ig_md.bitmap_bit == 28){ set_mask_28(); } else if(ig_md.bitmap_bit == 29){ set_mask_29(); } else if(ig_md.bitmap_bit == 30){ set_mask_30(); } else { set_mask_31(); } invert_mask();
                                if(ig_md.bitmap_pos[6:5] == 0){
                                    ig_md.bitmap_4 = clear_w0.execute(ig_md.hash_idx);
                                    ig_md.bitmap_1 = get_w3.execute(ig_md.hash_idx);
                                    ig_md.bitmap_2 = get_w2.execute(ig_md.hash_idx);
                                    ig_md.bitmap_3 = get_w1.execute(ig_md.hash_idx);
                                } else if(ig_md.bitmap_pos[6:5] == 1){
                                    ig_md.bitmap_3 = clear_w1.execute(ig_md.hash_idx);
                                    ig_md.bitmap_1 = get_w3.execute(ig_md.hash_idx);
                                    ig_md.bitmap_2 = get_w2.execute(ig_md.hash_idx);
                                    ig_md.bitmap_4 = get_w0.execute(ig_md.hash_idx);
                                } else if(ig_md.bitmap_pos[6:5] == 2){
                                    ig_md.bitmap_2 = clear_w2.execute(ig_md.hash_idx);
                                    ig_md.bitmap_1 = get_w3.execute(ig_md.hash_idx);
                                    ig_md.bitmap_3 = get_w1.execute(ig_md.hash_idx);
                                    ig_md.bitmap_4 = get_w0.execute(ig_md.hash_idx);
                                } else {
                                    ig_md.bitmap_1 = clear_w3.execute(ig_md.hash_idx);
                                    ig_md.bitmap_2 = get_w2.execute(ig_md.hash_idx);
                                    ig_md.bitmap_3 = get_w1.execute(ig_md.hash_idx);
                                    ig_md.bitmap_4 = get_w0.execute(ig_md.hash_idx);
                                }
                            } else {
                                ig_md.bitmap_1 = get_w3.execute(ig_md.hash_idx);
                                ig_md.bitmap_2 = get_w2.execute(ig_md.hash_idx);
                                ig_md.bitmap_3 = get_w1.execute(ig_md.hash_idx);
                                ig_md.bitmap_4 = get_w0.execute(ig_md.hash_idx);
                            }                      

                            ig_md.bitmap[127:96] = ig_md.bitmap_1;
                            ig_md.bitmap[95:64]  = ig_md.bitmap_2;
                            ig_md.bitmap[63:32]  = ig_md.bitmap_3;
                            ig_md.bitmap[31:0]   = ig_md.bitmap_4;

                            ig_md.request_timer=(bit<8>)(ig_intr_md.ingress_mac_tstamp>>10);   //in us
                            ig_md.send_request_result=dst_check_request_timer.execute(ig_md.hash_idx);
                            if(ig_md.send_request_result==1 && ig_md.tenary_state!=2){  //condition: not in waiting state
                                //get ig_md.bitmap first
                                mirror_to_request(REQUEST);
                            }

                            if(ig_md.dif==0){
                                if(ig_md.bitmap_1==0){
                                    if(ig_md.bitmap_2==0){
                                        if(ig_md.bitmap_3==0){
                                            if(ig_md.bitmap_4==0){
                                                ig_md.tenary_state=set_state_from_retx_to_ordered.execute(ig_md.hash_idx); //condition: not waiting state
                                            }
                                        }
                                    }
                                }

                                // ig_md.buffer_occupied=decre_buffer_occupied.execute(ig_md.hash_idx);
                            }else if(ig_md.psn_compare_result==1){
                                // ig_md.tenary_state=state_get.execute(ig_md.hash_idx);    //(compile error)
                                if(ig_md.tenary_state==2){         //release buffer in waiting state
                                    return;
                                }else{
                                    send_to_reordering_pool();
                                }
                                
                            }else{
                                // ig_md.buffer_occupied=decre_buffer_occupied.execute(ig_md.hash_idx);
                                drop();
                            }                                

                        }
                        /* data from In-network*/
                        else{
                            if(ig_md.dif==0){
                                // clear bit psn (age the window)
                                if(ig_md.bitmap_bit == 0){ set_mask_0(); } else if(ig_md.bitmap_bit == 1){ set_mask_1(); } else if(ig_md.bitmap_bit == 2){ set_mask_2(); } else if(ig_md.bitmap_bit == 3){ set_mask_3(); } else if(ig_md.bitmap_bit == 4){ set_mask_4(); } else if(ig_md.bitmap_bit == 5){ set_mask_5(); } else if(ig_md.bitmap_bit == 6){ set_mask_6(); } else if(ig_md.bitmap_bit == 7){ set_mask_7(); } else if(ig_md.bitmap_bit == 8){ set_mask_8(); } else if(ig_md.bitmap_bit == 9){ set_mask_9(); } else if(ig_md.bitmap_bit == 10){ set_mask_10(); } else if(ig_md.bitmap_bit == 11){ set_mask_11(); } else if(ig_md.bitmap_bit == 12){ set_mask_12(); } else if(ig_md.bitmap_bit == 13){ set_mask_13(); } else if(ig_md.bitmap_bit == 14){ set_mask_14(); } else if(ig_md.bitmap_bit == 15){ set_mask_15(); } else if(ig_md.bitmap_bit == 16){ set_mask_16(); } else if(ig_md.bitmap_bit == 17){ set_mask_17(); } else if(ig_md.bitmap_bit == 18){ set_mask_18(); } else if(ig_md.bitmap_bit == 19){ set_mask_19(); } else if(ig_md.bitmap_bit == 20){ set_mask_20(); } else if(ig_md.bitmap_bit == 21){ set_mask_21(); } else if(ig_md.bitmap_bit == 22){ set_mask_22(); } else if(ig_md.bitmap_bit == 23){ set_mask_23(); } else if(ig_md.bitmap_bit == 24){ set_mask_24(); } else if(ig_md.bitmap_bit == 25){ set_mask_25(); } else if(ig_md.bitmap_bit == 26){ set_mask_26(); } else if(ig_md.bitmap_bit == 27){ set_mask_27(); } else if(ig_md.bitmap_bit == 28){ set_mask_28(); } else if(ig_md.bitmap_bit == 29){ set_mask_29(); } else if(ig_md.bitmap_bit == 30){ set_mask_30(); } else { set_mask_31(); } invert_mask();
                                if(ig_md.bitmap_pos[6:5] == 0){ clear_w0.execute(ig_md.hash_idx); }
                                else if(ig_md.bitmap_pos[6:5] == 1){ clear_w1.execute(ig_md.hash_idx); }
                                else if(ig_md.bitmap_pos[6:5] == 2){ clear_w2.execute(ig_md.hash_idx); }
                                else { clear_w3.execute(ig_md.hash_idx); }

                                ig_md.tenary_state=set_state_from_waiting_to_ordered.execute(ig_md.hash_idx);   //when state = waiting
                                if(ig_md.tenary_state==2){
                                    // reset_w0.execute(ig_md.hash_idx);
                                    // reset_w1.execute(ig_md.hash_idx);
                                    // reset_w2.execute(ig_md.hash_idx);
                                    // reset_w3.execute(ig_md.hash_idx);
                                    ;   //reset bitmap   lmf
                                }
                            }else if(ig_md.psn_compare_result==1){
                                // mark lost bit (unconditional on state)   //lmf: condition: not waiting state
                                if(ig_md.bitmap_bit == 0){ set_mask_0(); } else if(ig_md.bitmap_bit == 1){ set_mask_1(); } else if(ig_md.bitmap_bit == 2){ set_mask_2(); } else if(ig_md.bitmap_bit == 3){ set_mask_3(); } else if(ig_md.bitmap_bit == 4){ set_mask_4(); } else if(ig_md.bitmap_bit == 5){ set_mask_5(); } else if(ig_md.bitmap_bit == 6){ set_mask_6(); } else if(ig_md.bitmap_bit == 7){ set_mask_7(); } else if(ig_md.bitmap_bit == 8){ set_mask_8(); } else if(ig_md.bitmap_bit == 9){ set_mask_9(); } else if(ig_md.bitmap_bit == 10){ set_mask_10(); } else if(ig_md.bitmap_bit == 11){ set_mask_11(); } else if(ig_md.bitmap_bit == 12){ set_mask_12(); } else if(ig_md.bitmap_bit == 13){ set_mask_13(); } else if(ig_md.bitmap_bit == 14){ set_mask_14(); } else if(ig_md.bitmap_bit == 15){ set_mask_15(); } else if(ig_md.bitmap_bit == 16){ set_mask_16(); } else if(ig_md.bitmap_bit == 17){ set_mask_17(); } else if(ig_md.bitmap_bit == 18){ set_mask_18(); } else if(ig_md.bitmap_bit == 19){ set_mask_19(); } else if(ig_md.bitmap_bit == 20){ set_mask_20(); } else if(ig_md.bitmap_bit == 21){ set_mask_21(); } else if(ig_md.bitmap_bit == 22){ set_mask_22(); } else if(ig_md.bitmap_bit == 23){ set_mask_23(); } else if(ig_md.bitmap_bit == 24){ set_mask_24(); } else if(ig_md.bitmap_bit == 25){ set_mask_25(); } else if(ig_md.bitmap_bit == 26){ set_mask_26(); } else if(ig_md.bitmap_bit == 27){ set_mask_27(); } else if(ig_md.bitmap_bit == 28){ set_mask_28(); } else if(ig_md.bitmap_bit == 29){ set_mask_29(); } else if(ig_md.bitmap_bit == 30){ set_mask_30(); } else { set_mask_31(); }
                                if(ig_md.bitmap_pos[6:5] == 0){ set_w0.execute(ig_md.hash_idx); }
                                else if(ig_md.bitmap_pos[6:5] == 1){ set_w1.execute(ig_md.hash_idx); }
                                else if(ig_md.bitmap_pos[6:5] == 2){ set_w2.execute(ig_md.hash_idx); }
                                else { set_w3.execute(ig_md.hash_idx); }

                                if(ig_md.tenary_state==1){     
                                    send_to_reordering_pool();
                                    // // mark lost bit (unconditional on state)   //lmf: condition: not waiting state
                                    // if(ig_md.bitmap_bit == 0){ set_mask_0(); } else if(ig_md.bitmap_bit == 1){ set_mask_1(); } else if(ig_md.bitmap_bit == 2){ set_mask_2(); } else if(ig_md.bitmap_bit == 3){ set_mask_3(); } else if(ig_md.bitmap_bit == 4){ set_mask_4(); } else if(ig_md.bitmap_bit == 5){ set_mask_5(); } else if(ig_md.bitmap_bit == 6){ set_mask_6(); } else if(ig_md.bitmap_bit == 7){ set_mask_7(); } else if(ig_md.bitmap_bit == 8){ set_mask_8(); } else if(ig_md.bitmap_bit == 9){ set_mask_9(); } else if(ig_md.bitmap_bit == 10){ set_mask_10(); } else if(ig_md.bitmap_bit == 11){ set_mask_11(); } else if(ig_md.bitmap_bit == 12){ set_mask_12(); } else if(ig_md.bitmap_bit == 13){ set_mask_13(); } else if(ig_md.bitmap_bit == 14){ set_mask_14(); } else if(ig_md.bitmap_bit == 15){ set_mask_15(); } else if(ig_md.bitmap_bit == 16){ set_mask_16(); } else if(ig_md.bitmap_bit == 17){ set_mask_17(); } else if(ig_md.bitmap_bit == 18){ set_mask_18(); } else if(ig_md.bitmap_bit == 19){ set_mask_19(); } else if(ig_md.bitmap_bit == 20){ set_mask_20(); } else if(ig_md.bitmap_bit == 21){ set_mask_21(); } else if(ig_md.bitmap_bit == 22){ set_mask_22(); } else if(ig_md.bitmap_bit == 23){ set_mask_23(); } else if(ig_md.bitmap_bit == 24){ set_mask_24(); } else if(ig_md.bitmap_bit == 25){ set_mask_25(); } else if(ig_md.bitmap_bit == 26){ set_mask_26(); } else if(ig_md.bitmap_bit == 27){ set_mask_27(); } else if(ig_md.bitmap_bit == 28){ set_mask_28(); } else if(ig_md.bitmap_bit == 29){ set_mask_29(); } else if(ig_md.bitmap_bit == 30){ set_mask_30(); } else { set_mask_31(); }
                                    // if(ig_md.bitmap_pos[6:5] == 0){ set_w0.execute(ig_md.hash_idx); }
                                    // else if(ig_md.bitmap_pos[6:5] == 1){ set_w1.execute(ig_md.hash_idx); }
                                    // else if(ig_md.bitmap_pos[6:5] == 2){ set_w2.execute(ig_md.hash_idx); }
                                    // else { set_w3.execute(ig_md.hash_idx); }                                    
                                 }else{ //in waiting state
                                    drop();
                                 }
                            }else{
                                //drop(); //should forward (probably end-to-end timeout retransmission)
                            }
                        }

                    }

                    /****************************************************************************
                    *                                 ACK/NACK
                    ****************************************************************************/
                    else{  //ACK/NAK
                        if(hdr.aeth.opcode == 0){    //ACK
                            ;
                        
                        }else if(hdr.aeth.opcode == 3){     //NAK
                            ;
                        }
                    }
                }
                
                /****************************************************************************
                *                        RoCE pkts with other opcodes
                ****************************************************************************/                    
                else{;
                    //RoCE pkts with other opcodes
                }

            }
        }
    } 
}

control Src_SwitchIngress(
        inout header_t hdr,
        inout ingress_metadata_t ig_md,
        // inout pair_t pair,
        in ingress_intrinsic_metadata_t ig_intr_md,
        in ingress_intrinsic_metadata_from_parser_t ig_prsr_md,
        inout ingress_intrinsic_metadata_for_deparser_t ig_dprsr_md,
        inout ingress_intrinsic_metadata_for_tm_t ig_tm_md) {
#include "ingress_src_registers.p4"
#include "ingress_actions.p4"
// #include "ingress_tables.p4"
    table ipv4_host {
        key = { hdr.ipv4.dst_addr : exact; }
        actions = {
            send; drop;
        }
        size = 1024;
    }

    table arp_forward{
        key={
            hdr.arp.target_ip_addr:exact;
        }
        actions={
            send; drop;
        }
        size=1024;
    }
    table bitmap_mask_map_src {
        key = { ig_md.bitmap_bit : exact; }
        actions = { set_bitmap_mask_src; }
        size = 32;
    }
    table word_and_map {
        key = { ig_md.bitmap_word : exact; }
        actions = { and_w0; and_w1; and_w2; and_w3; }
        size = 4;
    }
    table clear_word_map {
        key = { ig_md.bitmap_word : exact; }
        actions = { src_clear_w0; src_clear_w1; src_clear_w2; src_clear_w3; }
        size = 4;
    }

    apply{
        if(hdr.arp.isValid()){
            arp_forward.apply();
            return;
        }
        else if (hdr.ipv4.isValid()) {
            ipv4_host.apply();
            if(!hdr.bth.isValid()){     //Only processing RoCEv2 packets.
                return; 
            }else{
                if(hdr.bth.opcode==0x06 || hdr.bth.opcode==0x07 || hdr.bth.opcode==0x08 ){   //write_first, write_middle, write_last
                    ig_md.is_WRITE=1;
                }
                if(hdr.bth.opcode==0x11 || ig_md.is_WRITE==1 || hdr.bth.opcode==0x1e){   //ack, write, retrasmission request
                    get_flow_idx.apply(hdr,ig_md);
                    /****************************************************************************
                    *                          RETRANSMISSION REQUEST
                    ****************************************************************************/
                    if(hdr.bth.opcode==0x1e){

                        ig_md.request_timer=(bit<8>)(ig_intr_md.ingress_mac_tstamp>>10);   //in us

                        ig_md.psn=hdr.bth.packetSequenceNumber;
                        ig_md.epsn=src_epsn.execute(ig_md.hash_idx);    //(compile skill)if received from DstToR: set epsn reg; if received from recir port: get epsn reg
                        
                        // ig_md.replica_hit=replica_hit.execute(ig_md.hash_idx);  //(compile skill) return replica_hit and set replica_hit to 0
                        if(ig_intr_md.ingress_port==(bit<9>)(RECIRC_REQUEST_PORT)){         //request processing...
                            // ig_md.reg_op=2;
                            // ig_md.epsn=src_get_epsn.execute(ig_md.hash_idx);    //(compile error)
                            send_to_recir_request_pool();
                            // ig_md.psn=hdr.bth.packetSequenceNumber;
                            ig_md.bitmap=hdr.bitmap_eth.bitmap;
                            // split into 4x32-bit words
                            ig_md.bitmap_1=(bit<32>)(ig_md.bitmap);
                            ig_md.bitmap_2=(bit<32>)(ig_md.bitmap>>32);
                            ig_md.bitmap_3=(bit<32>)(ig_md.bitmap>>64);
                            ig_md.bitmap_4=(bit<32>)(ig_md.bitmap>>96);

                            // bit position = (epsn + flag) mod 128
                            ig_md.bitmap_flag=(bit<8>)hdr.bth.reserved2;
                            calcl_lost_psn();   //it doesn't matter when bitmap_flag is larger then 127 since the type bit<8>
                            ig_md.bitmap_pos=(bit<7>)(ig_md.psn) + (bit<7>)ig_md.bitmap_flag;
                            // ig_md.bitmap_pos=(bit<7>)(ig_md.psn+1);
                            ig_md.bitmap_bit=(bit<5>)ig_md.bitmap_pos;
                            ig_md.bitmap_word=(bit<2>)(ig_md.bitmap_pos >> 5);
                            bitmap_mask_map_src.apply();   // bitmap_mask = 1 << bitmap_bit

                            // check the bit in the right 32-bit word
                            word_and_map.apply();   // bitmap_and = bitmap_word & bitmap_mask
                            if(ig_md.bitmap_and!=0){ig_md.bitmap_is_set=1;}else{ig_md.bitmap_is_set=0;}

                            if(ig_md.bitmap_is_set==0){
                                ig_md.replica_hit_with_lost_psn=check_replica_hit_1.execute(ig_md.hash_idx);
                            }else{
                                ig_md.replica_hit_with_lost_psn=check_replica_hit_2.execute(ig_md.hash_idx);
                            }

                            // ig_md.replica_hit=check_replica_hit.execute(ig_md.hash_idx);    //(compile error), get hit state first, then reset state to 0 if 1
                            // ig_md.replica_hit_with_lost_psn=check_replica_hit.execute(ig_md.hash_idx);
                            ig_md.replica_hit=(bit<8>)(ig_md.replica_hit_with_lost_psn>>24);    //get hit flag
                            
                            if( ig_md.bitmap_and !=0 || ig_md.replica_hit ==1){
                                // hdr.bth.reserved2=hdr.bth.reserved2 + 1;    //error

                                // ig_md.bitmap_flag=ig_md.bitmap_flag+1;
                                update_bitmap_flag();
                                hdr.bth.reserved2=(bit<7>)ig_md.bitmap_flag;
                                // bit=1 (received) or replica found: clear bit + advance
                                clear_word_map.apply();   // clear the bit in the right word //error
                                hdr.bitmap_eth.bitmap=ig_md.bitmap;             //rewrite to hdr (data plane)

                            }else{
                                // set_lost_psn.execute(ig_md.hash_idx);   //blocked until the correspongding replica is found  
                                if(ig_md.bitmap_1==0){
                                    if(ig_md.bitmap_2==0){
                                        if(ig_md.bitmap_3==0){
                                            if(ig_md.bitmap_4==0){
                                                // ig_md.bitmap_is_0=1;
                                                // ig_md.epsn=src_epsn_is_meet.execute(ig_md.hash_idx);    //epsn carried by request has been matched
                                                if(ig_md.epsn==0){  //epsn is found
                                                    //all lost pkts in bitmap are found in replica pool or no lost pkt in bitmap
                                                    reset_request_state.execute(ig_md.hash_idx);    //this request is fulfilled!
                                                    drop();
                                                }
                                                // return;
                                            }
                                        }
                                    }
                                }
                                // if(ig_md.bitmap_1==0 && ig_md.bitmap_2==0 && ig_md.bitmap_3==0 && ig_md.bitmap_4==0){
                                //     if(ig_md.epsn==0){  //epsn is found
                                //         //all lost pkts in bitmap are found in replica pool or no lost pkt in bitmap
                                //         reset_request_state.execute(ig_md.hash_idx);    //this request is fulfilled!
                                //         drop();
                                //     }
                                //     return;
                                // }

                                // ig_md.lost_psn=(bit<24>)ig_md.psn+(bit<24>)hdr.bth.reserved2;        //get lost psn      //error

                                // calcl_lost_psn();   //it doesn't matter when bitmap_flag is larger then 127 since the type bit<8>
                                // set_lost_psn.execute(ig_md.hash_idx);   //blocked until the correspongding replica is found  
                                
                            }   

                            ig_md.send_unfulfilled_result=src_check_request_timer.execute(ig_md.hash_idx);      
                            if(ig_md.send_unfulfilled_result==1){                   //request time out, this request is unfulfilled!
                                mirror_to_unfulfilled_request(UNFULFILLED_REQUEST); //send unfulfilled request
                                reset_request_state.execute(ig_md.hash_idx);        //reset request state to inactive
                                drop();
                            }else{
                                ;//as above
                            }
                        }
                        else{   //receive request from DstToR
                            // ig_md.psn=hdr.bth.packetSequenceNumber;     //get epsn carried in request
                            // ig_md.reg_op=1;
                            // src_set_epsn.execute(ig_md.hash_idx);       //record epsn (compile error)
                            // ig_md.bitmap=hdr.bitmap_eth.bitmap;
                            if(ig_md.epsn == ig_md.psn){    //carry the same epsn value (bitmap is same since the request interval setting)
                                drop(); //just drop the redundant request
                                return;
                            }
                            
                            src_set_request_timer.execute(ig_md.hash_idx);  //record request's timestamp 
                            set_request_state.execute(ig_md.hash_idx);   //set request state to active

                            // reset_bitmap_flag.execute(ig_md.hash_idx);  //reset bitmap location

                            reset_lost_psn.execute(ig_md.hash_idx);     //set lost psn to 0
                            // reset_replica_hit.execute(ig_md.hash_idx);  //((compile error)) set hit flag to 0 (each time hit set to 1, it indicates that a psn in bitmap been found)

                            send_to_recir_request_pool();                              
                        }
                    }
                
                    /****************************************************************************
                    *                                   DATA
                    ****************************************************************************/
                    else if(ig_md.is_WRITE==1 ){
                        /* data from replica pool*/
                        if(ig_intr_md.ingress_port==(bit<9>) (REPLICA_PORT)){
                            ig_md.psn=hdr.bth.packetSequenceNumber;
                            ig_md.epsn=src_get_and_update_epsn.execute(ig_md.hash_idx);     //get request'epsn first, then set the epsn recorded in register to 0 if replica'psn matches it, indicating the request'epsn has been matched
                            ig_md.release_result=compare_with_ack.execute(ig_md.hash_idx);
                            if(ig_md.epsn==0){ig_md.epsn_is_zero=1;}else{ig_md.epsn_is_zero=0;}
                            if(ig_md.epsn_is_zero==1){
                                ig_md.lost_psn=get_lost_psn.execute(ig_md.hash_idx);
                            }
                            // ig_md.lost_psn=get_lost_psn.execute(ig_md.hash_idx);

                            // if(ig_md.epsn==0){
                            //     // ig_md.epsn=get_lost_psn.execute(ig_md.hash_idx);
                            //     if(ig_md.psn == ig_md.epsn){

                            //         // set_replica_hit.execute(ig_md.hash_idx);    // notify replica pool that lost pkt in bitmap is found
                            //     }
                            // }

                            ig_md.request_state=get_request_state.execute(ig_md.hash_idx);
                            if(ig_md.request_state==1){ //request is active
                                // ig_md.reg_op=3;(compile error)
                                // ig_md.epsn=src_get_and_update_epsn.execute(ig_md.hash_idx); //get request'epsn first, then set the epsn recorded in register to 0 if replica'psn matches it, indicating the request'epsn has been matched
                                // if(ig_md.psn == ig_md.epsn){
                                //     ;               // matched -> forward
                                //     decre_all_replica_occupied.execute(0);
                                //     decre_replica_occupied.execute(ig_md.hash_idx);
                                // }else{              //not matched -> keep replica
                                //     send_to_replica_pool();
                                // }                                

                                if(ig_md.epsn!=0){      //try to match epsn
                                    if(ig_md.psn == ig_md.epsn){
                                        ;               // matched -> forward
                                        decre_all_replica_occupied.execute(0);
                                        decre_replica_occupied.execute(ig_md.hash_idx);
                                    }else{              //not matched -> keep replica
                                        send_to_replica_pool();
                                    }
                                }else{                  //epsn carried by request has been matched, try to match request'bitmap (lost psn)
                                    // ig_md.lost_psn=get_lost_psn.execute(ig_md.hash_idx);
                                    if(ig_md.psn == ig_md.lost_psn){
                                        ;               //forward
                                        decre_all_replica_occupied.execute(0);
                                        decre_replica_occupied.execute(ig_md.hash_idx);
                                        // notify replica pool that lost pkt in bitmap is found, then replica pool try to find next lost psn according to bitmap.

                                    }else{
                                        send_to_replica_pool();
                                    }
                                }

                            }else{                      //no request received
                                send_to_replica_pool();
                            }     

                            // ig_md.release_result=compare_with_ack.execute(ig_md.hash_idx);
                            if(ig_md.release_result==1){    //release replica pkt
                                drop();
                                decre_all_replica_occupied.execute(0);
                                decre_replica_occupied.execute(ig_md.hash_idx);
                            }

                        }
                        /* data from server*/
                        else{
                            //todo: replica replacement strategy
                            ig_md.replica_pool_is_full=check_replica_occupied.execute(0);
                            if(ig_md.replica_pool_is_full==1){
                                ; //replica pool is full, don't mirror to replica pool
                            }else{
                                mirror_to_replica_pool(BACKUP);
                                incre_replica_occupied.execute(ig_md.hash_idx); 
                            }

                        }

                    }

                    /****************************************************************************
                    *                                 ACK/NACK
                    ****************************************************************************/
                    else{  //ACK/NAK
                        if(hdr.aeth.opcode == 0){    //ACK
                            ig_md.psn=hdr.bth.packetSequenceNumber;
                            record_ack.execute(ig_md.hash_idx);
                        }else if(hdr.aeth.opcode == 3){     //NAK
                            ;
                        }
                    }
                }

                /****************************************************************************
                *                        RoCE pkts with other opcodes
                ****************************************************************************/                
                else{;
                }
            }
        }
    } 
}

