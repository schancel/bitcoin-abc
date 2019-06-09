#!/bin/bash -e

export BITCOINCLI=./src/bitcoin-cli

for itr in $(seq 1 10); do 
    #for maxtxns in $(seq 152380 1000 152380); do 
        export BITCOIND=./src/bitcoind
        ./test/functional/test_runner.py -l DEBUG big_block_test.py --maxblocksize=31 #${maxtxns}
        export BITCOIND=./src/bitcoin-unchained
        ./test/functional/test_runner.py -l DEBUG big_block_test.py --maxblocksize=31 #${maxtxns}
    #done
done
