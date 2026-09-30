/* -*- P4_16 -*- */
#include <core.p4>
#if __TARGET_TOFINO__ == 2
#include <t2na.p4>
#else
#include <tna.p4>
#endif

#include "macro.p4"
		/*--------------------------------------------------
					Dst-Switch Register arrays              
		--------------------------------------------------*/
Register<bit<32>, bit<12>>(size=1<<12,initial_value=1) epsn_reg;
RegisterAction<bit<32>, bit<12>, bit<24>>(epsn_reg) epsn_get= {
    void apply(inout bit<32> value, out bit<24> result){
        result=(bit<24>)value;	
		if(value==1 || ig_md.psn==(bit<24>)value){
			value=(bit<32>)(ig_md.psn+1);
		}
    }
};

Register<bit<8>, bit<12>>(size=1<<12,initial_value=0) state_reg;         //state machine: 0:ordered; 1:ReTx; 2:waiting;
RegisterAction<bit<8>, bit<12>, bit<2>>(state_reg) set_state_to_retx= {
    void apply(inout bit<8> value, out bit<2> result){
		if(value!=2){
			value=1;
		}
		result=(bit<2>)value;
    }
};
RegisterAction<bit<8>, bit<12>, bit<2>>(state_reg) set_state_from_waiting_to_ordered= {
    void apply(inout bit<8> value, out bit<2> result){
		result=(bit<2>)value;
		if(value==2){
			value=0;
		}
		// result=(bit<2>)value;
    }
};
RegisterAction<bit<8>, bit<12>, bit<2>>(state_reg) set_state_from_retx_to_ordered= {
    void apply(inout bit<8> value, out bit<2> result){
		if(value!=2){
			value=0;
		}
		result=(bit<2>)value;
    }
};
RegisterAction<bit<8>, bit<12>, bit<2>>(state_reg) set_state_to_waiting= {
    void apply(inout bit<8> value, out bit<2> result){
        value=2;
		result=(bit<2>)value;
    }
};


Register<bit<32>, bit<12>>(size=1<<12,initial_value=0) bitmap_w0;         //per-flow bitmap word0 (bits 31:0)
Register<bit<32>, bit<12>>(size=1<<12,initial_value=0) bitmap_w1;         //per-flow bitmap word1 (bits 63:32)
Register<bit<32>, bit<12>>(size=1<<12,initial_value=0) bitmap_w2;         //per-flow bitmap word2 (bits 95:64)
Register<bit<32>, bit<12>>(size=1<<12,initial_value=0) bitmap_w3;         //per-flow bitmap word3 (bits 127:96)

RegisterAction<bit<32>, bit<12>, bit<32>>(bitmap_w0) reset_w0 = {
    void apply(inout bit<32> value) {
        value=0;
    }
};

RegisterAction<bit<32>, bit<12>, bit<32>>(bitmap_w0) reset_w1 = {
    void apply(inout bit<32> value) {
        value=0;
    }
};

RegisterAction<bit<32>, bit<12>, bit<32>>(bitmap_w0) reset_w2 = {
    void apply(inout bit<32> value) {
        value=0;
    }
};

RegisterAction<bit<32>, bit<12>, bit<32>>(bitmap_w0) reset_w3 = {
    void apply(inout bit<32> value) {
        value=0;
    }
};

RegisterAction<bit<32>, bit<12>, bit<32>>(bitmap_w0) set_w0 = {
    void apply(inout bit<32> value, out bit<32> result) {
        value = value | ig_md.bitmap_mask;
        result = value;
    }
};

RegisterAction<bit<32>, bit<12>, bit<32>>(bitmap_w1) set_w1 = {
    void apply(inout bit<32> value, out bit<32> result) {
        value = value | ig_md.bitmap_mask;
        result = value;
    }
};

RegisterAction<bit<32>, bit<12>, bit<32>>(bitmap_w2) set_w2 = {
    void apply(inout bit<32> value, out bit<32> result) {
        value = value | ig_md.bitmap_mask;
        result = value;
    }
};

RegisterAction<bit<32>, bit<12>, bit<32>>(bitmap_w3) set_w3 = {
    void apply(inout bit<32> value, out bit<32> result) {
        value = value | ig_md.bitmap_mask;
        result = value;
    }
};

RegisterAction<bit<32>, bit<12>, bit<32>>(bitmap_w0) clear_w0 = {
    void apply(inout bit<32> value, out bit<32> result) {
        value = value & ig_md.bitmap_mask;
        result = value;
    }
};

RegisterAction<bit<32>, bit<12>, bit<32>>(bitmap_w1) clear_w1 = {
    void apply(inout bit<32> value, out bit<32> result) {
        value = value & ig_md.bitmap_mask;
        result = value;
    }
};

RegisterAction<bit<32>, bit<12>, bit<32>>(bitmap_w2) clear_w2 = {
    void apply(inout bit<32> value, out bit<32> result) {
        value = value & ig_md.bitmap_mask;
        result = value;
    }
};

RegisterAction<bit<32>, bit<12>, bit<32>>(bitmap_w3) clear_w3 = {
    void apply(inout bit<32> value, out bit<32> result) {
        value = value & ig_md.bitmap_mask;
        result = value;
    }
};

RegisterAction<bit<32>, bit<12>, bit<32>>(bitmap_w0) get_w0 = {
    void apply(inout bit<32> value, out bit<32> result) {
        result = value;
    }
};

RegisterAction<bit<32>, bit<12>, bit<32>>(bitmap_w1) get_w1 = {
    void apply(inout bit<32> value, out bit<32> result) {
        result = value;
    }
};

RegisterAction<bit<32>, bit<12>, bit<32>>(bitmap_w2) get_w2 = {
    void apply(inout bit<32> value, out bit<32> result) {
        result = value;
    }
};

RegisterAction<bit<32>, bit<12>, bit<32>>(bitmap_w3) get_w3 = {
    void apply(inout bit<32> value, out bit<32> result) {
        result = value;
    }
};


Register<bit<8>, bit<12>>(size=1<<12,initial_value=0) request_timer_reg;  
RegisterAction<bit<8>, bit<12>, bit<8>>(request_timer_reg) dst_check_request_timer= {
    void apply(inout bit<8> value, out bit<8> result){
		if(value==0){
			value=ig_md.request_timer;
			result=1;
		}else if(ig_md.request_timer-value>REQUEST_INTERVAL){		//setup: request timer value;
			value=ig_md.request_timer;
			result=1;
		}else{
			result=0;
		}

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

