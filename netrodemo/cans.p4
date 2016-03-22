header_type ethernet_t {
    fields {
        dstAddr : 48;
        srcAddr : 48;
        etherType : 16;
    }
}

header_type ipv4_t {
    fields {
        version : 4;
        ihl : 4;
        diffserv : 8;
        totalLen : 16;
        identification : 16;
        flags : 3;
        fragOffset : 13;
        ttl : 8;
        protocol : 8;
        hdrChecksum : 16;
        src : 32;
        dst: 32;
    }
}

header_type udp_t {
    fields {
        srcPort : 16;
        dstPort : 16;
        length_ : 16;
        checksum : 16;
    }
}

// Headers for Paxos

header_type paxos_t {
    fields {
        inst    : 32;
        rnd     : 16;
        vrnd    : 16;
        acpt    : 16;
        msgtype : 16;
        val     : 32;
    }
}


header ethernet_t ethernet;
header ipv4_t ipv4;
header udp_t udp;
header paxos_t paxos;


#define ETHERTYPE_IPV4 0x0800
#define UDP_PROTOCOL 0x11
#define PAXOS_PROTOCOL 0x8888

parser start {
    return parse_ethernet;
}

parser parse_ethernet {
    extract(ethernet);
    return select(latest.etherType) {
        ETHERTYPE_IPV4 : parse_ipv4; 
        default : ingress;
    }
}

parser parse_ipv4 {
    extract(ipv4);
    return select(latest.protocol) {
        UDP_PROTOCOL : parse_udp;
        default : ingress;
    }
}

parser parse_udp {
    extract(udp);
    return select(udp.dstPort) {
        PAXOS_PROTOCOL: parse_paxos;
        default: ingress;
    }
}

parser parse_paxos {
    extract(paxos);
    return ingress;
}

primitive_action seq_func();
primitive_action paxos_phase1a();
primitive_action paxos_phase2a();
primitive_action reset_registers();


action forward(port) {
    modify_field(standard_metadata.egress_spec, port);
}

table mac_tbl {
    reads {
        standard_metadata.ingress_port : exact;
    }
    actions {
        forward;
    }
    size : 8;
}

action _no_op() {
}

action increase_seq() {
    seq_func();
    modify_field(udp.checksum, 0);
}

action handle_phase1a() {
    paxos_phase1a();
    modify_field(udp.checksum, 0);
}

action handle_phase2a() {
    paxos_phase2a();
    modify_field(udp.checksum, 0);
}

action reset_paxos() {
    reset_registers();
}

table paxos_tbl {
    reads {
        paxos.msgtype : exact;
    }
    actions {
        increase_seq;
        handle_phase1a;
        handle_phase2a;
        reset_paxos;
        _no_op;
    }
    size : 8;
}

control ingress {
    apply(mac_tbl);
    if (valid (paxos)) {
        apply(paxos_tbl);
    }
}
