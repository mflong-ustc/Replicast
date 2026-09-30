/* -*- P4_16 -*- */
#include <core.p4>
#if __TARGET_TOFINO__ == 2
#include <t2na.p4>
#else
#include <tna.p4>
#endif

#include "macro.p4"

action construct_request(){    //mirror pkt -> reqeust pkt
    hdr.bth.opcode=0x1e;    //use reserved opcode for request packet type.
    hdr.bth.packetSequenceNumber=(bit<24>)eg_md.mirror_1;    //carry epsn
    hdr.bth.reserved2=1;    //carry scan flag (initial 1)
    hdr.bitmap_eth.setValid();
    hdr.bitmap_eth.bitmap=eg_md.mirror_2;   //carry bitmap

    //swap ether and ip addr
    eg_md.mac_addr=hdr.ethernet.dst_addr;
    eg_md.ipv4_addr=hdr.ipv4.dst_addr;

    hdr.ipv4.dst_addr=hdr.ipv4.src_addr;
    hdr.ethernet.dst_addr=hdr.ethernet.src_addr;
    hdr.ipv4.src_addr=eg_md.ipv4_addr;
    hdr.ethernet.src_addr=eg_md.mac_addr;
}

action construct_unfulfilled_request(){
    hdr.bth.opcode=0x1f;    ////use reserved opcode for request packet type.
    hdr.bitmap_eth.setInvalid();    //remove bitmap_eth

    //swap ether and ip addr
    eg_md.mac_addr=hdr.ethernet.dst_addr;
    eg_md.ipv4_addr=hdr.ipv4.dst_addr;

    hdr.ipv4.dst_addr=hdr.ipv4.src_addr;
    hdr.ethernet.dst_addr=hdr.ethernet.src_addr;
    hdr.ipv4.src_addr=eg_md.ipv4_addr;
    hdr.ethernet.src_addr=eg_md.mac_addr; 

}