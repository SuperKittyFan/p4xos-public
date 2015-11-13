#!/usr/bin/env python
"""Paxos Client"""
__author__ = "Tu Dang"

from twisted.internet.protocol import DatagramProtocol
from twisted.internet import reactor
import argparse
import ConfigParser
import random
import string
import struct
import binascii
import os
from paxoscore.proposer import Proposer

VALUE_SIZE = 64 
PHASE_2A = 3

THIS_DIR=os.path.dirname(os.path.realpath(__file__))

class Proposer(object):
    def __init__(self, config, conn, proposer_id):
        self.config = config
        self.conn = conn
        self.rnd = proposer_id

    def submitRandomValue(self):
        msg = ''.join(random.SystemRandom().choice(string.ascii_uppercase + \
                string.digits) for _ in range(VALUE_SIZE))
        self.submit(msg)

    def submit(self, msg):
        self.conn.send(self.rnd, msg)

    def deliver(self, msg):
        print msg
        return msg


class Pserver(DatagramProtocol):

    def __init__(self, config):
        self.dst = (config.get('learner', 'addr'), \
                    config.getint('learner', 'port'))

    def startProtocol(self):
        """
        Called after protocol has started listening.
        """
        pass

    def send(self, rnd, msg):
        values = (PHASE_2A, 0, rnd, rnd, msg)
        packer = struct.Struct('>' + 'B H B B {0}s'.format(VALUE_SIZE))
        packed_data = packer.pack(*values)
        self.transport.write(packed_data, self.dst)

    def addDeliverHandler(self, deliver):
        self.deliver = deliver

    def datagramReceived(self, datagram, address):
        self.deliver(datagram)


if __name__ == '__main__':
    config = ConfigParser.ConfigParser()
    config.read('%s/paxos.cfg' % THIS_DIR)
    conn = Pserver(config)
    proposer = Proposer(config, conn, 0)
    conn.addDeliverHandler(proposer.deliver)
    reactor.listenUDP(config.getint('client', 'port'), conn)
    reactor.callLater(1,proposer.submitRandomValue)
    reactor.run()