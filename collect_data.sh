#!/bin/bash

export BITCOINCLI=./src/bitcoin-cli

for itr in $(seq 14 32); do 
    for CHAINLENGTH in $(seq 100 100 2000); do 
        export BITCOIND=./src/bitcoind
        ./test/functional/test_runner.py -l DEBUG chained_transactions.py --chainlength=${CHAINLENGTH}
        export BITCOIND=./src/bitcoin-unchained
        ./test/functional/test_runner.py -l DEBUG chained_transactions.py --chainlength=${CHAINLENGTH}
    done
done