/* -*- P4_16 -*- */
#include <core.p4>
#if __TARGET_TOFINO__ == 2
#include <t2na.p4>
#else
#include <tna.p4>
#endif


#include "macro.p4"
action nop() {}
action send(PortId_t port) {
    hdr.bridged_md.setValid();
    hdr.bridged_md.pkt_type=PKT_TYPE_NORMAL;    

    ig_tm_md.ucast_egress_port = port;
}

action drop() {
    ig_dprsr_md.drop_ctl = 1;
}
action psn_difference(){
    ig_md.dif=ig_md.psn-ig_md.epsn;
}
action send_to_reordering_pool(){
    send((bit<9>)OOO_PORT);
}
action send_to_replica_pool(){
    send((bit<9>)REPLICA_PORT);
}

action send_to_recir_request_pool(){
    send((bit<9>)RECIRC_REQUEST_PORT);
}

action set_bitmap_delta_action(bit<128> delta){
    ig_md.bitmap_delta_after_mapping=delta;
}

action set_bitmap_mask_src(bit<32> mask){
    ig_md.bitmap_mask=mask;
}

action and_w0(){ ig_md.bitmap_and = ig_md.bitmap_1 & ig_md.bitmap_mask; }
action and_w1(){ ig_md.bitmap_and = ig_md.bitmap_2 & ig_md.bitmap_mask; }
action and_w2(){ ig_md.bitmap_and = ig_md.bitmap_3 & ig_md.bitmap_mask; }
action and_w3(){ ig_md.bitmap_and = ig_md.bitmap_4 & ig_md.bitmap_mask; }


action src_clear_w0(){ ig_md.bitmap[31:0] = ig_md.bitmap_1 & ~ig_md.bitmap_mask; }
action src_clear_w1(){ ig_md.bitmap[63:32] = ig_md.bitmap_2 & ~ig_md.bitmap_mask; }
action src_clear_w2(){ ig_md.bitmap[95:64] = ig_md.bitmap_3 & ~ig_md.bitmap_mask; }
action src_clear_w3(){ ig_md.bitmap[127:96] = ig_md.bitmap_4 & ~ig_md.bitmap_mask; }

action update_bitmap_flag(){ig_md.bitmap_flag=ig_md.bitmap_flag+1;}
action calcl_lost_psn(){ig_md.lost_psn=ig_md.psn+(bit<24>)ig_md.bitmap_flag; }


action set_mask_0(){ ig_md.bitmap_mask = 32w0x00000001; }
action set_mask_1(){ ig_md.bitmap_mask = 32w0x00000002; }
action set_mask_2(){ ig_md.bitmap_mask = 32w0x00000004; }
action set_mask_3(){ ig_md.bitmap_mask = 32w0x00000008; }
action set_mask_4(){ ig_md.bitmap_mask = 32w0x00000010; }
action set_mask_5(){ ig_md.bitmap_mask = 32w0x00000020; }
action set_mask_6(){ ig_md.bitmap_mask = 32w0x00000040; }
action set_mask_7(){ ig_md.bitmap_mask = 32w0x00000080; }
action set_mask_8(){ ig_md.bitmap_mask = 32w0x00000100; }
action set_mask_9(){ ig_md.bitmap_mask = 32w0x00000200; }
action set_mask_10(){ ig_md.bitmap_mask = 32w0x00000400; }
action set_mask_11(){ ig_md.bitmap_mask = 32w0x00000800; }
action set_mask_12(){ ig_md.bitmap_mask = 32w0x00001000; }
action set_mask_13(){ ig_md.bitmap_mask = 32w0x00002000; }
action set_mask_14(){ ig_md.bitmap_mask = 32w0x00004000; }
action set_mask_15(){ ig_md.bitmap_mask = 32w0x00008000; }
action set_mask_16(){ ig_md.bitmap_mask = 32w0x00010000; }
action set_mask_17(){ ig_md.bitmap_mask = 32w0x00020000; }
action set_mask_18(){ ig_md.bitmap_mask = 32w0x00040000; }
action set_mask_19(){ ig_md.bitmap_mask = 32w0x00080000; }
action set_mask_20(){ ig_md.bitmap_mask = 32w0x00100000; }
action set_mask_21(){ ig_md.bitmap_mask = 32w0x00200000; }
action set_mask_22(){ ig_md.bitmap_mask = 32w0x00400000; }
action set_mask_23(){ ig_md.bitmap_mask = 32w0x00800000; }
action set_mask_24(){ ig_md.bitmap_mask = 32w0x01000000; }
action set_mask_25(){ ig_md.bitmap_mask = 32w0x02000000; }
action set_mask_26(){ ig_md.bitmap_mask = 32w0x04000000; }
action set_mask_27(){ ig_md.bitmap_mask = 32w0x08000000; }
action set_mask_28(){ ig_md.bitmap_mask = 32w0x10000000; }
action set_mask_29(){ ig_md.bitmap_mask = 32w0x20000000; }
action set_mask_30(){ ig_md.bitmap_mask = 32w0x40000000; }
action set_mask_31(){ ig_md.bitmap_mask = 32w0x80000000; }
action invert_mask(){ ig_md.bitmap_mask = ~ig_md.bitmap_mask; }


action mirror_to_request(MirrorId_t ing_ses){   //ing_ses=REQUEST
    ig_dprsr_md.mirror_type=MIRROR_TYPE_I2E_1;
    ig_md.ing_mir_ses=ing_ses;      //for dprsr
    ig_md.pkt_type=PKT_TYPE_MIRROR; //for dprsr
    ig_md.mirror_1=(bit<32>)ig_md.epsn; //for dprsr
    ig_md.mirror_2=ig_md.bitmap;    //for dprsr
    // ig_md.mirror_type=2;            //for dprsr
}

action mirror_to_unfulfilled_request(MirrorId_t ing_ses){   //ing_ses=UNFULFILLED_REQUEST
    ig_dprsr_md.mirror_type=MIRROR_TYPE_I2E;
    ig_md.ing_mir_ses=ing_ses;      //for dprsr
    ig_md.pkt_type=PKT_TYPE_UNFULFILLED; //for dprsr
}


action mirror_to_replica_pool(MirrorId_t ing_ses){  //ing_ses=BACKUP
    ig_dprsr_md.mirror_type=MIRROR_TYPE_I2E;
    ig_md.ing_mir_ses=ing_ses;      //for dprsr
    ig_md.pkt_type=PKT_TYPE_NORMAL; //pkt_type=normal, only carry bridged_md like normal packet.
}

// action set_lost_psn_action(bit<24> delta){
//     ig_md.lost_psn=ig_md.lost_psn+delta;
// }