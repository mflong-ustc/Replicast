/* -*- P4_16 -*- */
#include <core.p4>
#if __TARGET_TOFINO__ == 2
#include <t2na.p4>
#else
#include <tna.p4>
#endif

#include "include/headers.p4"
#include "include/util.p4"
#include "include/parser.p4"

#include "include/ingress.p4"
#include "include/egress.p4"

Pipeline(SwitchIngressParser(), //SrcToR
         Src_SwitchIngress(),
         SwitchIngressDeparser(),
        //  EmptyEgressParser(),
        //  EmptyEgress(),
        //  EmptyEgressDeparser()
         SwitchEgressParser(),
         Src_SwitchEgress(),
         SwitchEgressDeparser()
         ) pipe0;

Pipeline(SwitchIngressParser(), //DstToR
         Dst_SwitchIngress(),   
         SwitchIngressDeparser(),
        //  EmptyEgressParser(),
        //  EmptyEgress(),
        //  EmptyEgressDeparser()
         SwitchEgressParser(),
         Dst_SwitchEgress(),
         SwitchEgressDeparser()
         ) pipe1;

Pipeline(EmptyIngressParser(), //
         EmptyIngress(),
         EmptyIngressDeparser(),
         EmptyEgressParser(),
         EmptyEgress(),
         EmptyEgressDeparser()
         ) pipe3;

Pipeline(EmptyIngressParser(), //
         EmptyIngress(),
         EmptyIngressDeparser(),
         EmptyEgressParser(),
         EmptyEgress(),
         EmptyEgressDeparser()
         ) pipe2;
      
Switch(pipe0,pipe1,pipe2,pipe3) main;