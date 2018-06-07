#!/usr/bin/env python3
# Copyright (c) 2018 The Dash Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

from test_framework.mininode import *
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import *
from base64 import *

'''
InstantSendTest -- test InstantSend functionality (prevent doublespend for unconfirmed transactions)
'''

MASTERNODE_COLLATERAL = 1000


class InstantSendTest(BitcoinTestFramework):
    def __init__(self):
        super().__init__()
        self.mn_count = 2
        self.node_count = self.mn_count + 2
        self.collaterals = []
        self.setup_clean_chain = True
        self.is_network_split = False
        # get sporkkey from src/chainparams.cpp, RegTestParams()
        self.sporkkey = 'cP4EKFyJsHT39LDqgdcB43Y3YXjNyjb5Fuas1GQSeAtjnZWmZEQK'
        self.sporkaddr = 'yj949n1UH6fDhw6HtVE5VMj2iSTaSWBMcW'

    def create_simple_node(self):
        idx = len(self.nodes)
        self.nodes.append(start_node(idx, self.options.tmpdir,
                                     ["-debug",
                                      "-sporkaddr=%s" % self.sporkaddr,
                                      "-sporkkey=%s" % self.sporkkey]))
        for i in range(0, idx):
            connect_nodes(self.nodes[i], idx)

    def get_mnconf_file(self):
        return os.path.join(self.options.tmpdir, "node0/regtest/masternode.conf")

    def create_masternode(self):
        # this is masternode index
        idx = len(self.nodes)
        key = self.nodes[0].masternode("genkey")
        address = self.nodes[0].getnewaddress()
        txid = self.nodes[0].sendtoaddress(address, MASTERNODE_COLLATERAL)
        txrow = self.nodes[0].getrawtransaction(txid, True)
        collateral_vout = 0
        for vout_idx in range(0, len(txrow["vout"])):
            vout = txrow["vout"][vout_idx]
            if vout["value"] == MASTERNODE_COLLATERAL:
                collateral_vout = vout_idx
        conf = self.get_mnconf_file()
        f = open(conf, 'a')
        f.write("mn%d 127.0.0.1:%d %s %s %d\n" % (idx, p2p_port(idx), key,
                                                     txid, collateral_vout))
        f.close()
        stop_node(self.nodes[0], 0)
        self.nodes[0] = start_node(0, self.options.tmpdir, ["-debug",
                                                            "-sporkaddr=%s" % self.sporkaddr,
                                                            "-sporkkey=%s" % self.sporkkey])
        for i in range(1, idx):
            connect_nodes(self.nodes[i], 0)

        self.nodes.append(start_node(idx, self.options.tmpdir,
                                     ['-debug=masternode', '-externalip=127.0.0.1',
                                      '-masternode=1',
                                      '-masternodeprivkey=%s' % key,
                                      "-sporkaddr=%s" % self.sporkaddr,
                                      "-sporkkey=%s" % self.sporkkey
                                      ]))
        for i in range(0, idx):
            connect_nodes(self.nodes[i], idx)

    def sentinel(self):
        for i in range(1, self.mn_count + 1):
            self.nodes[i].sentinelping("1.1.0")

    def setup_network(self):
        self.nodes = []
        # create faucet node for collateral and transactions
        self.nodes.append(start_node(0, self.options.tmpdir, ["-debug",
                                                            "-sporkaddr=%s" % self.sporkaddr,
                                                            "-sporkkey=%s" % self.sporkkey]))
        required_balance = MASTERNODE_COLLATERAL * self.mn_count + 1
        while self.nodes[0].getbalance() < required_balance:
            set_mocktime(get_mocktime() + 1)
            self.nodes[0].generate(1)
        # create masternodes
        for i in range(0, self.mn_count):
            self.create_masternode()
        # create simple nodes
        for i in range(0, self.node_count - self.mn_count - 1):
            self.create_simple_node()
        # enable InstandSend
        for i in range(0, self.num_nodes):
            self.nodes[i].spork('SPORK_2_INSTANTSEND_ENABLED', 10000)
        # workaround for mempool sync problems
        set_mocktime(get_mocktime() + 1)
        self.nodes[0].generate(1)
        # sync nodes
        self.sync_all()
        set_mocktime(get_mocktime() + 1)
        sync_masternodes(self.nodes)
        print(self.nodes[0].masternode("start-all"))
        sync_masternodes(self.nodes)
        self.sentinel()

    def run_test(self):
        print(self.nodes[0].masternode("list"))
        for i in range(1, self.mn_count + 1):
            print(self.nodes[i].masternode("status"))

        for i in range(0, self.num_nodes):
            print(self.nodes[i].spork('show'))


if __name__ == '__main__':
    InstantSendTest().main()
