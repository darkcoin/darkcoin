#!/usr/bin/env python3
# Copyright (c) 2015-2020 The Dash Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

import time

from test_framework.test_framework import DashTestFramework
from test_framework.util import *

'''
feature_llmq_simplepose.py

Checks simple PoSe system based on LLMQ commitments

'''

class LLMQSimplePoSeTest(DashTestFramework):
    def set_test_params(self):
        self.set_dash_test_params(6, 5, fast_dip3_enforcement=True)
        self.set_dash_llmq_test_params(5, 3)

    def run_test(self):

        self.nodes[0].spork("SPORK_17_QUORUM_DKG_ENABLED", 0)
        self.wait_for_sporks_same()

        # check if mining quorums with all nodes being online succeeds without punishment/banning
        self.test_no_banning(expected_connections=2)

        # Now lets isolate MNs one by one and verify that punishment/banning happens
        self.test_banning(self.isolate_mn, 1)

        self.repair_masternodes(False)

        self.nodes[0].spork("SPORK_21_QUORUM_ALL_CONNECTED", 0)
        self.wait_for_sporks_same()

        self.reset_probe_timeouts()

        # Make sure no banning happens with spork21 enabled
        self.test_no_banning(expected_connections=4)

        # Lets restart masternodes with closed ports and verify that they get banned even though they are connected to other MNs (via outbound connections)
        self.test_banning(self.close_mn_port, 3)

        self.repair_masternodes(True)
        self.reset_probe_timeouts()

        self.test_banning(self.force_old_mn_proto, 3)

    def isolate_mn(self, mn):
        mn.node.setnetworkactive(False)
        wait_until(lambda: mn.node.getconnectioncount() == 0)
        return True

    def close_mn_port(self, mn):
        self.stop_node(mn.node.index)
        self.start_masternode(mn, ["-listen=0", "-nobind"])
        connect_nodes(mn.node, 0)
        # Make sure the to-be-banned node is still connected well via outbound connections
        for mn2 in self.mninfo:
            if mn2 is not mn:
                connect_nodes(mn.node, mn2.node.index)
        self.reset_probe_timeouts()
        return False

    def force_old_mn_proto(self, mn):
        self.stop_node(mn.node.index)
        self.start_masternode(mn, ["-pushversion=70216"])
        connect_nodes(mn.node, 0)
        self.reset_probe_timeouts()
        return False

    def test_no_banning(self, expected_connections):
        for i in range(3):
            self.mine_quorum(expected_connections=expected_connections)
        for mn in self.mninfo:
            assert(not self.check_punished(mn) and not self.check_banned(mn))

    def test_banning(self, invalidate_proc, expected_connections):
        online_mninfos = self.mninfo.copy()
        for i in range(2):
            mn = online_mninfos[len(online_mninfos) - 1]
            online_mninfos.remove(mn)
            expect_contribution_to_fail = invalidate_proc(mn)

            t = time.time()
            while (not self.check_banned(mn)) and (time.time() - t) < 120:
                expected_contributors = len(online_mninfos) + 1
                if expect_contribution_to_fail:
                    expected_contributors -= 1
                # Make sure we do fresh probes
                self.bump_mocktime(50 * 60 + 1)
                # Sleep a couple of seconds to let mn sync tick to happen
                time.sleep(2)
                self.mine_quorum(expected_connections=expected_connections, expected_members=len(online_mninfos), expected_contributions=expected_contributors, expected_complaints=expected_contributors-1, expected_commitments=expected_contributors, mninfos=online_mninfos)

            assert(self.check_banned(mn))

    def repair_masternodes(self, restart):
        # Repair all nodes
        for mn in self.mninfo:
            if self.check_banned(mn) or self.check_punished(mn):
                addr = self.nodes[0].getnewaddress()
                self.nodes[0].sendtoaddress(addr, 0.1)
                self.nodes[0].protx('update_service', mn.proTxHash, '127.0.0.1:%d' % p2p_port(mn.node.index), mn.keyOperator, "", addr)
                self.nodes[0].generate(1)
                assert(not self.check_banned(mn))

                if restart:
                    self.stop_node(mn.node.index)
                    self.start_masternode(mn)
                else:
                    mn.node.setnetworkactive(True)
            connect_nodes(mn.node, 0)
        self.sync_all()

        # Isolate and re-connect all MNs (otherwise there might be open connections with no MNAUTH for MNs which were banned before)
        for mn in self.mninfo:
            mn.node.setnetworkactive(False)
            wait_until(lambda: mn.node.getconnectioncount() == 0)
            mn.node.setnetworkactive(True)
            force_finish_mnsync(mn.node)
            connect_nodes(mn.node, 0)

    def reset_probe_timeouts(self):
        # Make sure all masternodes will reconnect/re-probe
        self.bump_mocktime(50 * 60 + 1)
        # Sleep a couple of seconds to let mn sync tick to happen
        time.sleep(2)
        self.sync_all()

    def check_punished(self, mn):
        info = self.nodes[0].protx('info', mn.proTxHash)
        if info['state']['PoSePenalty'] > 0:
            return True
        return False

    def check_banned(self, mn):
        info = self.nodes[0].protx('info', mn.proTxHash)
        if info['state']['PoSeBanHeight'] != -1:
            return True
        return False

if __name__ == '__main__':
    LLMQSimplePoSeTest().main()
