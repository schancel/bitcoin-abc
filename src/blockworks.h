// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2017 The Bitcoin Core developers
// Copyright (c) 2017-2019 The Bitcoin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_BLOCKWORKS_H
#define BITCOIN_BLOCKWORKS_H

#include "chain.h"
#include "config.h"
#include "primitives/block.h"
#include "primitives/transaction.h"

#include <boost/thread/locks.hpp>
#include <boost/thread/shared_mutex.hpp>

#include <memory>
#include <vector>

class Blockworks {
private:
    mutable boost::shared_mutex rwlock;

    CBlockIndex *pindexPrev;

    // Configuration parameters for the block size
    uint64_t nMaxGeneratedBlockSize;

    // Information on the current status of the block
    uint64_t nBlockSize;
    uint64_t nBlockTx;
    uint64_t nBlockSigOps;
    Amount nFees;

    // Chain context for the block
    int nHeight;
    int64_t nLockTimeCutoff;
    int64_t nMedianTimePast;

    const Config *config;
public:
    Blockworks(const Config &_config, CBlockIndex *pindexPrev);

    /** Construct a new block template with coinbase to scriptPubKeyIn */
    std::unique_ptr<CBlock> GetBlock(const CScript &scriptPubKeyIn);
};


#endif