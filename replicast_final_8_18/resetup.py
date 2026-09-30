## Reset stateful registers (you can modify loss rate here)

# p4 = bfrt.replicast.pipe
p4_src = bfrt.replicast.pipe0    #SrcToR
p4_dst = bfrt.replicast.pipe1    #DstToR
# p4_0=bfrt.replicast.pipe0
# p4_2=bfrt.replicast.pipe2

##_____________________SrcToR___________________##
ipv4_host_src = p4_src.Src_SwitchIngress.ipv4_host
arp_forward_src = p4_src.Src_SwitchIngress.arp_forward
# reset registers to their "initial_value"
ack_seq_reg=p4_src.Src_SwitchIngress.ack_seq_reg
ack_seq_reg.clear()
request_timer_reg=p4_src.Src_SwitchIngress.request_timer_reg
request_timer_reg.clear()
epsn_reg=p4_src.Src_SwitchIngress.epsn_reg
epsn_reg.clear()
lost_pkt_reg=p4_src.Src_SwitchIngress.lost_pkt_reg
lost_pkt_reg.clear()
request_state_reg=p4_src.Src_SwitchIngress.request_state_reg
request_state_reg.clear()
replica_pool_reg=p4_src.Src_SwitchIngress.replica_pool_reg
replica_pool_reg.clear()
all_replica_pool_reg=p4_src.Src_SwitchIngress.all_replica_pool_reg
all_replica_pool_reg.clear()

loss_rate=10     # /1e3
random_loss_reg=p4_src.Src_SwitchEgress.random_loss_reg
random_loss_reg.mod(REGISTER_INDEX=0,f1=loss_rate)

##_____________________DstToR___________________##
ipv4_host_dst = p4_dst.Dst_SwitchIngress.ipv4_host
arp_forward_dst = p4_dst.Dst_SwitchIngress.arp_forward
# reset registers to their "initial_value"
epsn_reg=p4_dst.Dst_SwitchIngress.epsn_reg
epsn_reg.clear()
state_reg=p4_dst.Dst_SwitchIngress.state_reg
state_reg.clear()
bitmap_w0=p4_dst.Dst_SwitchIngress.bitmap_w0
bitmap_w0.clear()
bitmap_w1=p4_dst.Dst_SwitchIngress.bitmap_w1
bitmap_w1.clear()
bitmap_w2=p4_dst.Dst_SwitchIngress.bitmap_w2
bitmap_w2.clear()
bitmap_w3=p4_dst.Dst_SwitchIngress.bitmap_w3
bitmap_w3.clear()
request_timer_reg=p4_dst.Dst_SwitchIngress.request_timer_reg
request_timer_reg.clear()


bfrt.complete_operations()

# Final programming
print("""
******************* STATEFUL REGISTERS RESET SUCCESS *****************
""")
