#include "includes/paxos_headers.p4"
#include "includes/paxos_parser.p4"
#include "l2_control_1.p4"

#define MAJORITY 2

header_type ingress_metadata_t {
    fields {
        majority: 8;
    }
}
metadata ingress_metadata_t paxos_packet_metadata;

register instance_register {
    width : INSTANCE_SIZE;
    instance_count : 1;
}

register majority_register {
    width : INSTANCE_SIZE;
    instance_count : 1;
}

register tmp_register {
    width : 48;
    instance_count : 1;
}

action read_majority(){
    register_read(paxos_packet_metadata.majority, majority_register, 0);
}



// This action changes a request message to a 2A message and writes the current instance number to the packet header.
// Then, it increments the current instance number by 1 and stores the result in a register.
action handle_request() {
    modify_field(paxos.msgtype, PAXOS_2A);
    modify_field(paxos.round, 0);	
    register_read(paxos.instance, instance_register, 0);
    add_to_field(paxos.instance, 1);
    register_write(instance_register, 0, paxos.instance);
    register_write(majority_register, 0, 0);
}

action commit(){
    modify_field(paxos.msgtype, PAXOS_2B); // Create a 2B message
    //register_write(tmp_register,0,ethernet.srcAddr);
    modify_field(ethernet.srcAddr,ethernet.dstAddr);
    //register_read(ethernet.dstAddr,tmp_register,0);
    modify_field(intrinsic_metadata.mcast_grp, 1);
    register_write(majority_register, 0, paxos_packet_metadata.majority+1);
}

action add_and_drop(){
    register_write(majority_register,0,paxos_packet_metadata.majority+1);
    modify_field(intrinsic_metadata.mcast_grp, 0);
    drop();
}

table tbl_sequence {
    reads   { paxos.msgtype : exact; }
    actions { add_and_drop; handle_request; _nop; }
    size : 5;
}

table tbl_commit{
    actions {commit;}
}

table tbl_read{
    actions {read_majority;}
}



control ingress {
    apply(smac);                 /* MAC learning from l2_control.p4... */
    apply(dmac);                 /* ... not doing Paxos logic */
                                 
    if (valid(paxos)) {    /* check if we have a paxos packet */
        if (paxos.msgtype==PAXOS_2C){
            apply(tbl_read);
            if (paxos_packet_metadata.majority+1==MAJORITY){
                apply(tbl_commit);
            }else{
                apply(tbl_sequence);
            }
        }else{
            apply(tbl_sequence);     /* increase paxos instance number */
        }
     }
}