#!/usr/bin/env python3
# Copyright (c) 2018 Nobody
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test perforance of descendant package (chained transactions)"""
import time
import copy
import sys
from test_framework.cdefs import ONE_MEGABYTE
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import *
from test_framework.messages import COIN
from test_framework.blocktools import *
from test_framework.script import OP_FALSE
import json

"""Read optional arguments from command line"""
DEBUG_MODE = False

MAGNETIC_ANOMALY_START_TIME = 2000000000

class BigBlockMineTest(BitcoinTestFramework):
    def generate_small_transactions(self, node, count, utxo_list):
        FEE = 250  # TODO: replace this with node relay fee based calculation
        num_transactions = 0
        mempool_size = 0
        random.shuffle(utxo_list)
        while len(utxo_list) >= 1 and num_transactions < count:
            tx = CTransaction()
            utxo = utxo_list.pop()
            tx.vin.append(CTxIn(COutPoint(int(utxo['txid'], 16), utxo['vout'])))
            input_amount = int(utxo['amount'] * COIN)

            # Split UTXOs so we have more to spend next time.  Otherwise keep the one utxo
            # if we're below having 1 COIN worth so we can continue to pay fees.
            outputs = 2
            # We can pay the fee 1000 times by itself if needed.
            if input_amount < FEE * 1000:
                outputs =  1

            output_amount = (input_amount - FEE) // outputs

            if output_amount <= 0:
                # Sanity check -- if we chose inputs that are too small, skip
                continue

            for _ in range(outputs):
                tx.vout.append(
                    CTxOut(output_amount, hex_str_to_bytes(utxo['scriptPubKey'])))

            # Sign and send the transaction to get into the mempool
            tx_signed_hex = node.signrawtransactionwithwallet(ToHex(tx))['hex']
            node.sendrawtransaction(tx_signed_hex)
            num_transactions += 1
            # Two bytes per hex value
            mempool_size += len(tx_signed_hex)//2
        return num_transactions, mempool_size

    def add_options(self, parser):
        parser.add_argument("--maxblocksize", dest="maxblocksize", default=32, metavar='int', type=int,
                            help="Test double-spend of 1-confirmed transaction")

    def set_test_params(self):
        ''' our test network requires a peer node so that getblocktemplate succeeds '''
        self.num_nodes = 2
        chained_args = [
                        "-magneticanomalyactivationtime={}".format(MAGNETIC_ANOMALY_START_TIME), 
                        "-blockmaxsize={}".format(31 * ONE_MEGABYTE),
                        "-deprecatedrpc=signrawtransaction"
                       ]
        config_node2 = chained_args.copy()
        if DEBUG_MODE:
            chained_args.append(DEBUG_MODE)
        self.extra_args = [chained_args, config_node2]

    def mempool_count(self):
        ''' get count of tx in mempool '''
        mininginfo = self.nodes[0].getmininginfo()
        return mininginfo['pooledtx']

    def run_test(self):
        maxblocksize = self.options.maxblocksize * ONE_MEGABYTE
        self.log.info('Starting Test with {0} Txns'.format(maxblocksize))
        
        # Generate enough blocks go through 10 regtest halvings.
        # Having lots of UTXOs up front will allow us to increas the number of transactions faster.
        for _ in range(10):
            self.nodes[0].generate(150)

        mempool_size = 0
        while mempool_size < maxblocksize:
            # Don't list stuff that's too small to spend
            utxo_list = self.nodes[0].listunspent(None,None,None,None,{"minimumAmount": 1000e-8})
            # Get best UTXOs first?
            utxo_list = sorted(utxo_list, key=lambda utxo: int(utxo['amount'] * COIN), reverse=True)
            print(len(utxo_list))
            sendtx_start = time.perf_counter()
            num_transactions, mempool_size = self.generate_small_transactions(self.nodes[0], len(utxo_list), utxo_list)
            sendtx_stop = time.perf_counter()
            print("Len utxo: {} , num_transactions: {}, maxblocksize: {}".format(len(utxo_list), num_transactions, maxblocksize))
            assert num_transactions > 0, "No new transactions added?"

            # TODO: do not use perf_counter. use timer from -printtoconsole instead
            gbt_start = time.perf_counter()
            # assemble a block and validate all tx in it
            templat = self.nodes[0].getblocktemplate()
            gbt_stop = time.perf_counter()

            with open("big_block.json", "a") as json_file:
                entry = {
                    "bitcoind": os.getenv("BITCOIND", "bitcoind"),
                    "maxblocksize":  maxblocksize,
                    "num_transactions": num_transactions,
                    "mined_txns": len(templat['transactions']),
                    "mempool_size": mempool_size,
                    "sent_tx_time": sendtx_stop - sendtx_start,
                    "gbt_time": gbt_stop - gbt_start
                }
                json.dump(entry, json_file)
                json_file.write("\n")

            # Mine the utxos, so we can generate more transactions on the next block.
            self.nodes[0].generate(1)


if __name__ == '__main__':
    BigBlockMineTest().main()
