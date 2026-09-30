/* -*- P4_16 -*- */
#include <core.p4>
#if __TARGET_TOFINO__ == 2
#include <t2na.p4>
#else
#include <tna.p4>
#endif

#include "macro.p4"
		/*--------------------------------------------------
					Src-Switch Register arrays              
		--------------------------------------------------*/
Register<bit<32>, bit<12>>(size=1<<12,initial_value=0) ack_seq_reg;
RegisterAction<bit<32>, bit<12>, bit<32>>(ack_seq_reg) record_ack= {
    void apply(inout bit<32> value){
        value=(bit<32>)ig_md.psn;
    }
};
RegisterAction<bit<32>, bit<12>, bit<8>>(ack_seq_reg) compare_with_ack= {
    void apply(inout bit<32> value,out bit<8> result){
        if(ig_md.psn>(bit<24>)value){
            result=0;
        }else{
            result=1;
        }
    }
};


Register<bit<8>, bit<12>>(size=1<<12,initial_value=0) request_timer_reg;  
RegisterAction<bit<8>, bit<12>, bit<8>>(request_timer_reg) src_set_request_timer= {
    void apply(inout bit<8> value){
		  value=ig_md.request_timer;
    }
};
RegisterAction<bit<8>, bit<12>, bit<8>>(request_timer_reg) src_check_request_timer= {
    void apply(inout bit<8> value, out bit<8> result){
		if(ig_md.request_timer-value>REQUEST_TIMEOUT){	//setup: request timer value;
			value=0;
			result=1;
		}else{
			result=0;
		}
    }
};


Register<bit<32>, bit<12>>(size=1<<12,initial_value=1) epsn_reg;
RegisterAction<bit<32>, bit<12>, bit<24>>(epsn_reg) src_epsn= {
    void apply(inout bit<32> value, out bit<24> result){
		result=(bit<24>)value;
		if(ig_intr_md.ingress_port!=(bit<9>)(RECIRC_REQUEST_PORT)){
			value=(bit<32>)ig_md.psn;	//record request's psn
		}
		
    }
};
RegisterAction<bit<32>, bit<12>, bit<24>>(epsn_reg) src_get_and_update_epsn= {		//for replica pool foundation
    void apply(inout bit<32> value, out bit<24> result){
		result=(bit<24>)value;
		if(value == (bit<32>)ig_md.psn){
			value=0;
		}
    }
};

Register<bit<32>, bit<12>>(size=1<<12,initial_value=0) lost_pkt_reg;	//find lost pkt according to bitmap
RegisterAction<bit<32>, bit<12>, bit<32>>(lost_pkt_reg) reset_lost_psn= {
    void apply(inout bit<32> value){
		value=0;
    }
};
RegisterAction<bit<32>, bit<12>, bit<24>>(lost_pkt_reg) get_lost_psn= {
    void apply(inout bit<32> value, out bit<24> result){
		result=(bit<24>)value;
        // if(ig_md.psn==(bit<24>)value && ig_md.epsn_is_zero==1){
        if(ig_md.psn==(bit<24>)value){   //epsn must be meet first, otherwise psn=epsn set hit flag
            
            value=value | 0x01000000;   // notify replica pool that lost pkt in bitmap is found, then replica pool try to find next lost psn according to bitmap.

        }
    }
};
// RegisterAction<bit<32>, bit<12>, bit<32>>(lost_pkt_reg) check_replica_hit= { //conpile error: could not fit within a single input crossbar in an MAU stage (IXBAR)
//     void apply(inout bit<32> value,out bit<32> result){
// 		// result=value[31:24];  //get flag, compile error: can't slice memory
//         result=value;

//         if(value <= 0x00ffffff && ig_md.bitmap_is_set==0){    // hit flag = 0 && bitmap[pos] == 0
//             value=(bit<32>)ig_md.lost_psn;
//         }else{
//             value=value & 0x00ffffff;   //00000000,[11111111,11111111,11111111]  reset flag to 0
//         }
//     }
// };
RegisterAction<bit<32>, bit<12>, bit<32>>(lost_pkt_reg) check_replica_hit_1= {
    void apply(inout bit<32> value,out bit<32> result){
		// result=value[31:24];  //get flag, compile error: can't slice memory
        result=value;
		
        // value=value & 0x00ffffff;   //00000000,[11111111,11111111,11111111]  reset flag to 0
        if(value <= 0x00ffffff){    // hit flag = 0 && bitmap[pos] == 0
        // if(value <= 0x00ffffff){    // hit flag = 0 && bitmap[pos] == 0
            value=(bit<32>)ig_md.lost_psn;
        }else{
            value=value & 0x00ffffff;   //00000000,[11111111,11111111,11111111]  reset flag to 0
        }
    }
};
RegisterAction<bit<32>, bit<12>, bit<32>>(lost_pkt_reg) check_replica_hit_2= {
    void apply(inout bit<32> value,out bit<32> result){
		// result=value[31:24];  //get flag, compile error: can't slice memory
        result=value;
		
        value=value & 0x00ffffff;   //00000000,[11111111,11111111,11111111]  reset flag to 0
    }
};

Register<bit<8>, bit<12>>(size=1<<12,initial_value=0) request_state_reg;	//find lost pkt according to bitmap
RegisterAction<bit<8>, bit<12>, bit<8>>(request_state_reg) set_request_state= {
    void apply(inout bit<8> value){
      value=1;
    }
};
RegisterAction<bit<8>, bit<12>, bit<8>>(request_state_reg) reset_request_state= {
    void apply(inout bit<8> value){
      value=0;
    }
};
RegisterAction<bit<8>, bit<12>, bit<8>>(request_state_reg) get_request_state= {
    void apply(inout bit<8> value, out bit<8> result){
      result=value;
    }
};

Register<bit<16>, bit<12>>(size=1<<12,initial_value=0) replica_pool_reg;         //
RegisterAction<bit<16>, bit<12>, bit<16>>(replica_pool_reg) get_replica_occupied= {
    void apply(inout bit<16> value, out bit<16> result){
        result=value;
    }
};
RegisterAction<bit<16>, bit<12>, bit<16>>(replica_pool_reg) incre_replica_occupied= {
    void apply(inout bit<16> value, out bit<16> result){
        value=value+1;
        result=value;
    }
};
RegisterAction<bit<16>, bit<12>, bit<16>>(replica_pool_reg) decre_replica_occupied= {
    void apply(inout bit<16> value, out bit<16> result){
        value=value-1;
        result=value;
    }
};

Register<bit<16>, bit<1>>(size=1,initial_value=0) all_replica_pool_reg;         //
RegisterAction<bit<16>, bit<1>, bit<8>>(all_replica_pool_reg) check_replica_occupied= {
    void apply(inout bit<16> value, out bit<8> result){
    	if(value < REPLICA_POOL_SIZE){
			value=value+1;
			result=0;
        }else{
			result=1;
		}
    }
};
RegisterAction<bit<16>, bit<1>, bit<16>>(all_replica_pool_reg) decre_all_replica_occupied= {
    void apply(inout bit<16> value){
        value=value-1;
    }
};
		/*--------------------------------------------------
					        debug registers             
		--------------------------------------------------*/

// Register<bit<1>,bit<1>>(size=1,initial_value=0) error_reg;
// RegisterAction<bit<1>,bit<1>,bit<1>>(error_reg) error_reg_action={
//     void apply(inout bit<1>value,out bit<1> result){
//         value=1;
//         result=value;
//     }
// };

