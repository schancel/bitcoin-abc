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
#include "miner.h"

#include <boost/thread/locks.hpp>
#include <boost/thread/shared_mutex.hpp>

#include <memory>
#include <vector>

// Keep track of transactions to-be-added added to a block, and some various metadata.
struct BlockEntry {
public:
    CTransactionRef tx;
    //!< Cached to avoid expensive parent-transaction lookups
    Amount txFee;
    //!< ... and avoid recomputing tx size
    size_t txSize;
    //!< ... Track number of sigops
    uint64_t txSigOps;

    //!< Track the height and time at which tx was final
    LockPoints lockPoints;

    //!< Track information about the package
    uint64_t packageCount;
    Amount packageFee;
    size_t packageSize;
    uint64_t packageSigOps;


    BlockEntry(CTransactionRef _tx, Amount _fees, uint64_t _size, int64_t _sigOps) 
    : tx(_tx), txFee(_fees), txSize(_size), txSigOps(_sigOps),
      packageCount(1), packageFee(_fees), packageSize(_size), packageSigOps(_sigOps)
    { }

    // Calculate the feerate for this transaction.  Use the minimum of the package feerate,
    // or the transaction itself.  Parents TXNs should never end up "paying for" child transactions.
    CFeeRate FeeRate() const {
        return int64_t(txSize) * packageFee < int64_t(packageSize) * txFee ?
            CFeeRate(packageFee, packageSize) :  CFeeRate(txFee, txSize);
    }

private:
    // Include a parent transactions accounting into our own.
    // We assume that this is used in topological order by Blockworks.
    void AccountForParent(BlockEntry &parent) {
        packageCount += parent.packageCount;
        packageFee += parent.packageFee;
        packageSize += parent.packageSize;
        packageSigOps += parent.packageSigOps;
    }

    friend class Blockworks;
};

struct BlockCandidate {
    CBlock block;

    std::vector<BlockEntry> entries;
};

class Blockworks {
private:
    mutable boost::shared_mutex rwlock;

    CBlockIndex *pindexPrev;
    std::unique_ptr<BlockCandidate> candidateBlock;
    CBlock* pblock;

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

    /** Construct a new empty block with coinbase to scriptPubKeyIn */
    std::unique_ptr<BlockCandidate> GetBlock(const CScript &scriptPubKeyIn);


private:
    /** Test to see if a transaction can be validly added to the block */
    bool TestTransaction(const BlockEntry &entry);

    /** Add an individual transaction to the block */
    bool AddToBlock(const BlockEntry &entry);

    /** Add packages to the block */
    void AddTransactionsToBlock(CTxMemPool &mempool);

    /** Reset internal state */
    void resetBlock();
};


#endif