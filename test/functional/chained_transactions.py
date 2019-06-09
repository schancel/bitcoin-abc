#!/usr/bin/env python3
# Copyright (c) 2018 Nobody
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test perforance of descendant package (chained transactions)"""
import time
import copy
import sys
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import *
from test_framework.messages import COIN
from test_framework.blocktools import *
import json

"""Read optional arguments from command line"""
TEST_ITERATIONS = 10
DEBUG_MODE = '-printtoconsole'

MAGNETIC_ANOMALY_START_TIME = 2000000000

class ChainedTest(BitcoinTestFramework):
    def add_options(self, parser):
        parser.add_argument("--chainlength", dest="chainlength", default=2000, metavar='int', type=int,
                            help="Test double-spend of 1-confirmed transaction")

    def set_test_params(self):
        ''' our test network requires a peer node so that getblocktemplate succeeds '''
        self.num_nodes = 2
        chained_args = ["-limitancestorcount=20000", "-limitdescendantcount=20000",
                        "-limitancestorsize=1000", "-limitdescendantsize=1000",
                        "-magneticanomalyactivationtime=%d" % MAGNETIC_ANOMALY_START_TIME, 
                        "-deprecatedrpc=signrawtransaction"
                       ]
        config_node2 = chained_args.copy()
        if DEBUG_MODE:
            chained_args.append(DEBUG_MODE)
        self.extra_args = [chained_args, config_node2]

    # Build a transaction that spends parent_txid:vout
    # Return amount sent
    def chain_transaction(self, node, parent_txid, vout, value, fee, num_outputs):
        send_value = satoshi_round((value - fee) / num_outputs)
        inputs = [{'txid': parent_txid, 'vout': vout}]
        outputs = {}
        for i in range(num_outputs):
            outputs[node.getnewaddress()] = send_value
        rawtx = node.createrawtransaction(inputs, outputs)
        signedtx = node.signrawtransaction(rawtx)

        #measure the performance of sending the raw transaction to the node
        sendtx_start = time.perf_counter()
        new_txid = node.sendrawtransaction(signedtx['hex'])
        sendtx_stop = time.perf_counter()
        fulltx = node.getrawtransaction(new_txid, 1)

        #self.log.info('{0} => {1}'.format(parent_txid, fulltx['vout'][0]))

        # make sure we didn't generate a change output
        assert(len(fulltx['vout']) == num_outputs)
        return (new_txid, send_value, sendtx_stop - sendtx_start, fulltx['size'])

    def send_chain_to_node(self, chain_length):
        ''' Generates tx chain and send it to node '''
        for i in range(chain_length):
            (sent_txid, sent_value, this_sendtx, tx_size) = self.chain_transaction(
                self.nodes[0], self.txid, 0, self.value, self.fee, 1)
            if not self.chain_top:
                self.chain_top = sent_txid
            self.txid = sent_txid
            self.value = sent_value
            self.chain.append(sent_txid)
            self.mempool_send += this_sendtx
            self.mempool_size += tx_size

    def mempool_count(self):
        ''' get count of tx in mempool '''
        mininginfo = self.nodes[0].getmininginfo()
        return mininginfo['pooledtx']

    def run_test(self):
        chain_length = self.options.chainlength
        self.log.info('Starting Test with {0} Chained Transactions'.format(chain_length))
        self.chain_top = None

        self.mempool_send = 0
        self.mempool_size = 0
        self.chain = []

        self.send_chain_to_node(chain_length)

        # mempool should have all our tx
        assert(self.mempool_count() == chain_length)
        mempool = self.nodes[0].getrawmempool(True)
        self.log.info('tx at top has {} descendants'.format(mempool[self.chain_top]["descendantcount"]))
        assert(mempool[self.chain_top]["descendantcount"] == chain_length)

        self.runs=[]
        for _ in range(TEST_ITERATIONS):
            # do not use perf_counter. use timer from -printtoconsole instead
            gbt_start = time.perf_counter()
            # assemble a block and validate all tx in it
            templat = self.nodes[0].getblocktemplate()
            gbt_stop = time.perf_counter()
            # make sure all tx got mined
            assert(len(templat['transactions']) == chain_length)
            self.runs.append(gbt_stop - gbt_start)

        with open("data_file.json", "a") as json_file:
            json.dump({
                "bitcoind": os.getenv("BITCOIND", "bitcoind"),
                "chain_length":  chain_length,
                "mempool_size": self.mempool_size,
                "sent_tx_time": self.mempool_send,
                "gbt_times": self.runs,
                "gbt_avg_time": sum(self.runs)/len(self.runs),
                "gbt_time_0": self.runs[0]
            }, json_file)
            json_file.write("\n")
        self.log.info('Mempool size {0}'.format(self.mempool_size))
        self.log.info('Send Tx took {0:.5f}s'.format(self.mempool_send))
        if len(self.runs) > 1:
            self.log.info('run times {}'.format(self.runs))
        self.log.info('GetBlkT took {0:.5f}s'.format(sum(self.runs)/len(self.runs)))

if __name__ == '__main__':
    ChainedTest().main()
