#include <core.p4>
#if __TARGET_TOFINO__ == 2
#include <t2na.p4>
#else
#include <tna.p4>
#endif


control Src_SwitchEgress(
        inout header_t hdr,
        inout egress_metadata_t eg_md,
        in egress_intrinsic_metadata_t eg_intr_md,
        in egress_intrinsic_metadata_from_parser_t eg_prsr_md,
        inout egress_intrinsic_metadata_for_deparser_t eg_dprsr_md,
        inout egress_intrinsic_metadata_for_output_port_t eg_oport_md){
#include "egress_actions.p4"
	Register<bit<32>,bit<1>>(1,1250) reg_ecn_marking_threshold; // default = 1250 (100KB) (80 bytes / cell)
	RegisterAction<bit<32>,bit<1>,bit<8>>(reg_ecn_marking_threshold) cmp_ecn_marking_threshold = {
		void apply(inout bit<32> reg_val, out bit<8> rv){
			if((bit<32>)eg_intr_md.deq_qdepth >= reg_val){
				rv = 1;
			}
			else {
				rv = 0;
			}
		}
	};

	Register<bit<16>,bit<1>>(1,1) random_loss_reg; // control plane can modify the loss rate
	RegisterAction<bit<16>,bit<1>,bit<8>>(random_loss_reg) random_loss_cmp = {
		void apply(inout bit<16> reg_val, out bit<8> rv){
			if(eg_md.random_num < reg_val){
				rv = 1;
			}
			else {
				rv = 0;
			}
		}
	};  
    Random<W>() random_seed;

    apply{
        if(eg_md.pkt_type == PKT_TYPE_UNFULFILLED){
            construct_unfulfilled_request();
        }

        eg_md.exceeded_ecn_marking_threshold = cmp_ecn_marking_threshold.execute(0);    //ecn marking
        if(eg_md.exceeded_ecn_marking_threshold ==1 ){
            hdr.ipv4.diffserv = hdr.ipv4.diffserv | 0b11;
        }


        if(eg_intr_md.egress_port == 40){    //random pkt loss here to emulate the in-network pkt loss
            eg_md.random_num=random_seed.get();
            eg_md.random_num=eg_md.random_num>>6;   //transfor to bit<10> type (0~1023)
            eg_md.random_loss=random_loss_cmp.execute(0);
            if(eg_md.random_loss==1){    //set pkt loss ～ 0.1%
                eg_dprsr_md.drop_ctl=1; //disable unicast
            }
        }
    }
}

control Dst_SwitchEgress(
        inout header_t hdr,
        inout egress_metadata_t eg_md,
        in egress_intrinsic_metadata_t eg_intr_md,
        in egress_intrinsic_metadata_from_parser_t eg_prsr_md,
        inout egress_intrinsic_metadata_for_deparser_t eg_dprsr_md,
        inout egress_intrinsic_metadata_for_output_port_t eg_oport_md){
#include "egress_actions.p4"
    apply{
        if(eg_md.pkt_type ==  PKT_TYPE_MIRROR){
            construct_request();
        }
    }
}
