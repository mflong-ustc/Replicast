from ipaddress import ip_address

host1_1=64  #9
host1_2=56  #10
host1_e=40  #12

host2_1=136 #17
host2_2=144 #18
host2_e=160 #20

replica_pool_port=48    #11
ooo_port=152            #19
recir_req_port=32       #13

# p4 = bfrt.replicast.pipe
p4_src = bfrt.replicast.pipe0    #SrcToR
p4_dst = bfrt.replicast.pipe1    #DstToR
# p4_0=bfrt.replicast.pipe0
# p4_2=bfrt.replicast.pipe2

lpbk_set=bfrt.port.port

# This function can clear all the tables and later on other fixed objects
# once bfrt support is added.
def clear_all(verbose=True, batching=True):
    global p4_src,p4_dst
    global bfrt
    
    def _clear(table, verbose=False, batching=False):
        if verbose:
            print("Clearing table {:<40} ... ".
                  format(table['full_name']), end='', flush=True)
        try:    
            entries = table['node'].get(regex=True, print_ents=False)
            try:
                if batching:
                    bfrt.batch_begin()
                for entry in entries:
                    entry.remove()
            except Exception as e:
                print("Problem clearing table {}: {}".format(
                    table['name'], e.sts))
            finally:
                if batching:
                    bfrt.batch_end()
        except Exception as e:
            if e.sts == 6:
                if verbose:
                    print('(Empty) ', end='')
        finally:
            if verbose:
                print('Done')

        # Optionally reset the default action, but not all tables
        # have that
        try:
            table['node'].reset_default()
        except:
            pass
    
    # The order is important. We do want to clear from the top, i.e.
    # delete objects that use other objects, e.g. table entries use
    # selector groups and selector groups use action profile members
    

    # Clear Match Tables
    for table in p4_src.info(return_info=True, print_info=False):
        if table['type'] in ['MATCH_DIRECT', 'MATCH_INDIRECT_SELECTOR']:
            _clear(table, verbose=verbose, batching=batching)

    # Clear Selectors
    for table in p4_src.info(return_info=True, print_info=False):
        if table['type'] in ['SELECTOR']:
            _clear(table, verbose=verbose, batching=batching)
            
    # Clear Action Profiles
    for table in p4_src.info(return_info=True, print_info=False):
        if table['type'] in ['ACTION_PROFILE']:
            _clear(table, verbose=verbose, batching=batching)

    # Clear Match Tables
    for table in p4_dst.info(return_info=True, print_info=False):
        if table['type'] in ['MATCH_DIRECT', 'MATCH_INDIRECT_SELECTOR']:
            _clear(table, verbose=verbose, batching=batching)

    # Clear Selectors
    for table in p4_dst.info(return_info=True, print_info=False):
        if table['type'] in ['SELECTOR']:
            _clear(table, verbose=verbose, batching=batching)
            
    # Clear Action Profiles
    for table in p4_dst.info(return_info=True, print_info=False):
        if table['type'] in ['ACTION_PROFILE']:
            _clear(table, verbose=verbose, batching=batching)
    
clear_all()

lpbk_set.mod(DEV_PORT=replica_pool_port,LOOPBACK_MODE="BF_LPBK_MAC_NEAR") #11  for replica pool
lpbk_set.mod(DEV_PORT=recir_req_port,LOOPBACK_MODE="BF_LPBK_MAC_NEAR") #13  for retransmission request processing (bitmap processing)
lpbk_set.mod(DEV_PORT=ooo_port,LOOPBACK_MODE="BF_LPBK_MAC_NEAR") #19 for reordering buffer

mirror_conf=bfrt.mirror.cfg
# mirror_to_replica_pool
mirror_conf.add_with_normal(sid=2,direction="INGRESS",ucast_egress_port=replica_pool_port,ucast_egress_port_valid=True,session_enable=True)
# mirror_to_request
mirror_conf.add_with_normal(sid=3,direction="INGRESS",ucast_egress_port=host2_e,ucast_egress_port_valid=True,session_enable=True,max_pkt_len=128)
# mirror_to_unfulfilled_request
mirror_conf.add_with_normal(sid=4,direction="INGRESS",ucast_egress_port=host1_e,ucast_egress_port_valid=True,session_enable=True,max_pkt_len=128)

##_____________________SrcToR___________________##
ipv4_host_src = p4_src.Src_SwitchIngress.ipv4_host
ipv4_host_src.add_with_send(dst_addr=ip_address('10.1.1.1'),   port=host1_1)    #9
ipv4_host_src.add_with_send(dst_addr=ip_address('10.1.1.2'),   port=host1_2)    #10
ipv4_host_src.add_with_send(dst_addr=ip_address('10.1.1.3'),   port=host1_e)    #12
ipv4_host_src.add_with_send(dst_addr=ip_address('10.1.1.4'),   port=host1_e)    #12

arp_forward_src = p4_src.Src_SwitchIngress.arp_forward
arp_forward_src.add_with_send(target_ip_addr=ip_address('10.1.1.1'),port=host1_1)
arp_forward_src.add_with_send(target_ip_addr=ip_address('10.1.1.2'),port=host1_2)
arp_forward_src.add_with_send(target_ip_addr=ip_address('10.1.1.3'),port=host1_e)
arp_forward_src.add_with_send(target_ip_addr=ip_address('10.1.1.4'),port=host1_e)

bitmap_mask_map_src = p4_src.Src_SwitchIngress.bitmap_mask_map_src
for i in range(0, 32):
    bitmap_mask_map_src.add_with_set_bitmap_mask_src(bitmap_bit=i, mask=1 << i)

word_and_map = p4_src.Src_SwitchIngress.word_and_map
word_and_map.add_with_and_w0(bitmap_word=0)
word_and_map.add_with_and_w1(bitmap_word=1)
word_and_map.add_with_and_w2(bitmap_word=2)
word_and_map.add_with_and_w3(bitmap_word=3)

clear_word_map = p4_src.Src_SwitchIngress.clear_word_map
clear_word_map.add_with_src_clear_w0(bitmap_word=0)
clear_word_map.add_with_src_clear_w1(bitmap_word=1)
clear_word_map.add_with_src_clear_w2(bitmap_word=2)
clear_word_map.add_with_src_clear_w3(bitmap_word=3)

##_____________________DstToR___________________##
ipv4_host_dst = p4_dst.Dst_SwitchIngress.ipv4_host
ipv4_host_dst.add_with_send(dst_addr=ip_address('10.1.1.1'),   port=host2_e)    #20
ipv4_host_dst.add_with_send(dst_addr=ip_address('10.1.1.2'),   port=host2_e)    #20
ipv4_host_dst.add_with_send(dst_addr=ip_address('10.1.1.3'),   port=host2_1)    #17
ipv4_host_dst.add_with_send(dst_addr=ip_address('10.1.1.4'),   port=host2_2)    #18

arp_forward_dst = p4_dst.Dst_SwitchIngress.arp_forward
arp_forward_dst.add_with_send(target_ip_addr=ip_address('10.1.1.1'),port=host2_e)
arp_forward_dst.add_with_send(target_ip_addr=ip_address('10.1.1.2'),port=host2_e)
arp_forward_dst.add_with_send(target_ip_addr=ip_address('10.1.1.3'),port=host2_1)
arp_forward_dst.add_with_send(target_ip_addr=ip_address('10.1.1.4'),port=host2_2)


bfrt.complete_operations()

# Final programming
print("""
******************* PROGAMMING RESULTS *****************
""")
ipv4_host_src.dump(table=True)
ipv4_host_dst.dump(table=True)

arp_forward_src.dump(table=True)
arp_forward_dst.dump(table=True)

