#!/usr/bin/env python3
# Copyright (c) 2017 The Bitcoin developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

#
# Test HighPriorityTransaction code
#

from test_framework.blocktools import create_confirmed_utxos
from test_framework.messages import COIN
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import satoshi_round, hex_str_to_bytes

DEFAULT_MAX_BLOCK_SIZE = 100000  # 100Kb block
DEFAULT_BLOCK_PRIORITY_PERCENT = 10
LIMIT_FREE_RELAY = 10 * DEFAULT_MAX_BLOCK_SIZE / 1000


class HighPriorityTransactionTest(BitcoinTestFramework):
    def add_options(self, parser):
        parser.add_argument("--percent", dest="block_priority_percent", type=int, default=DEFAULT_BLOCK_PRIORITY_PERCENT,
                            help="What percent of high priority transactions should be tested")
        parser.add_argument("--blocksize", dest="max_block_size", type=int, default=DEFAULT_MAX_BLOCK_SIZE,
                            help="How big of a block to mine in bytes")

    def set_test_params(self):
        # Get two nodes so we can mine stuff
        self.num_nodes = 2
        self.extra_args = [["-blockprioritypercentage={}".format(self.options.block_priority_percent),
                            "-limitfreerelay={}".format(LIMIT_FREE_RELAY),
                            "-blockmaxsize={}".format(self.options.max_block_size)]] * self.num_nodes

    def create_small_transactions(self, node, utxos, num, fee):
        addr = node.getnewaddress()
        txids = []
        for _ in range(num):
            t = utxos.pop()
            inputs = [{"txid": t["txid"], "vout": t["vout"]}]
            outputs = {}
            change = t['amount'] - fee
            outputs[addr] = satoshi_round(change)
            rawtx = node.createrawtransaction(inputs, outputs)
            signresult = node.signrawtransactionwithwallet(
                rawtx, None, "NONE|FORKID")
            txid = node.sendrawtransaction(signresult["hex"], True)
            txids.append(txid)
        return txids

    def run_test(self):
        # this is the priority cut off as defined in AllowFreeThreshold() (see: src/txmempool.h)
        # anything above that value is considered an high priority transaction
        hiprio_threshold = COIN * 144 / 250
        self.relayfee = self.nodes[0].getnetworkinfo()['relayfee']

        # Create enough UTXOs for us to be able to be able to create MAX_BLOCKS_SIZE bytes of high-fee transactions
        # and 2 * self.options.block_priority_percent * self.options.max_block_size / 100 bytes of free transactions
        utxos = create_confirmed_utxos(self.nodes[0], int(
            2 * self.options.max_block_size / 157))
        # Make sure our transactions are aged properly to be hipriority
        self.nodes[0].generate(150)

        # Create 20% of a block worth of non-confirmed free transactions.
        hi_prio_txids = self.create_small_transactions(
            self.nodes[0], utxos, len(utxos)//2, 0)
        # Create 100% of a block worth of non-confirmed expensive transactions.
        self.create_small_transactions(self.nodes[0], utxos, len(
            utxos)//2, satoshi_round(1000 * 10e-8))

        mempool = self.nodes[0].getrawmempool(True)
        # assert that all transactions are the appropriate priority
        for i in hi_prio_txids:
            assert(i in mempool)
            assert(mempool[i]['currentpriority'] > hiprio_threshold)

        # Get a block template, and see if it's correctly generated
        block = self.nodes[0].getblocktemplate()
        # assert that all the priority transactions are still in the mempool.
        free_size = 0
        total_size = 0
        for txn in block['transactions'][1:]:
            txn_size = len(hex_str_to_bytes(txn['data']))
            if txn['fee'] == 0:
                free_size += txn_size
            total_size += txn_size

        # Asser the priority percentages are correct  We allow for a little wiggle room due to finite transactions sizes.
        assert free_size < total_size * (self.options.block_priority_percent + 5) / \
            100, "Too many free transactions: {}%".format(
                free_size/total_size * 100)
        assert free_size > total_size * self.options.block_priority_percent / \
            200, "Not enough free transactions: {}%".format(
                free_size/total_size * 100)


if __name__ == '__main__':
    HighPriorityTransactionTest().main()
