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
from test_framework.util import assert_equal, satoshi_round, hex_str_to_bytes

MAX_BLOCK_SIZE = 1000000 # 1 megabyte
BLOCK_PRIORITY_PERCENT=10
LIMIT_FREE_RELAY = 10 * MAX_BLOCK_SIZE / 1000 

class HighPriorityTransactionTest(BitcoinTestFramework):

    def set_test_params(self):
        self.num_nodes = 1
        self.extra_args = [["-blockprioritypercentage={}".format(BLOCK_PRIORITY_PERCENT), 
                            "-limitfreerelay={}".format(LIMIT_FREE_RELAY),
                            "-blockmaxsize={}".format(MAX_BLOCK_SIZE)]]

    def create_small_transactions(self, node, utxos, num, fee):
        addr = node.getnewaddress()
        txids = []
        for _ in range(num):
            t = utxos.pop()
            inputs = [{"txid": t["txid"], "vout": t["vout"]}]
            outputs = {}
            print(t['amount'])
            change = t['amount'] - fee
            print(change)
            outputs[addr] = satoshi_round(change)
            rawtx = node.createrawtransaction(inputs, outputs)
            signresult = node.signrawtransactionwithwallet(
                rawtx, None, "NONE|FORKID")
            txid = node.sendrawtransaction(signresult["hex"], True)
            txids.append(txid)
        return txids

    def generate_high_priotransactions(self, node, count):
        # create 150 simple one input one output hi prio txns
        hiprio_utxo_count = 150
        age = 250
        # be sure to make this utxo aged enough
        hiprio_utxos = create_confirmed_utxos(node, hiprio_utxo_count, age)

        # Create hiprio_utxo_count number of txns with 0 fee
        txids = self.create_small_transactions(
            node, hiprio_utxos, hiprio_utxo_count, 0)
        return txids

    def run_test(self):
        # this is the priority cut off as defined in AllowFreeThreshold() (see: src/txmempool.h)
        # anything above that value is considered an high priority transaction
        hiprio_threshold = COIN * 144 / 250
        self.relayfee = self.nodes[0].getnetworkinfo()['relayfee']

        # Create enough UTXOs for us to be able to be able to create MAX_BLOCKS_SIZE bytes of high-fee transactions
        # and 2 * BLOCK_PRIORITY_PERCENT * MAX_BLOCK_SIZE / 100 bytes of free transactions
        old_utxos = create_confirmed_utxos(self.nodes[0], int(2 * BLOCK_PRIORITY_PERCENT * MAX_BLOCK_SIZE / 100 / 179))
        # Make sure our transactions are aged properly to be hipriority
        self.nodes[0].generate(150) 
        # Generate some recent UTXOs so we can spend them with a fee
        new_utxos = create_confirmed_utxos(self.nodes[0], int(MAX_BLOCK_SIZE / 179))

        # Create 20% of a block worth of non-confirmed free transactions.
        hi_prio_txids = self.create_small_transactions(self.nodes[0], old_utxos, len(old_utxos), 0)
        # Create 100% of a block worth of non-confirmed expensive transactions.
        self.create_small_transactions(self.nodes[0], new_utxos, len(new_utxos), 1000)

        mempool = self.nodes[0].getrawmempool(True)
        # assert that all the priority transactions are still in the mempool.
        for i in hi_prio_txids:
            assert(i in mempool)
            assert(mempool[i]['currentpriority'] > hiprio_threshold)

        # Get a block template, and see if it's correctly generated
        block = self.nodes[0].getblocktemplate(1)

        mempool = self.nodes[0].getblock(True)
        # assert that all the priority transactions are still in the mempool.
        free_size = 0
        total_size = 0
        for txn in block['transactions'][1:]:
            txn_size = len(hex_str_to_bytes(txn['data']))
            if txn['fee'] == 0:
                free_size += txn_size
            total_size += txn_size

        assert free_size > total_size / 10, "Too many free transactions!"


if __name__ == '__main__':
    HighPriorityTransactionTest().main()
