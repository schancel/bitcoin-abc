// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2016 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_MINER_H
#define BITCOIN_MINER_H

#include <primitives/block.h>
#include <txmempool.h>

#include <boost/multi_index/ordered_index.hpp>
#include <boost/multi_index_container.hpp>

#include <cstdint>
#include <memory>

class CBlockIndex;
class CChainParams;
class Config;
class CReserveKey;
class CScript;
class CWallet;

static const bool DEFAULT_PRINTPRIORITY = false;

struct CBlockTemplateEntry {
    CTransactionRef tx;
    //!< Total real fees paid by the transaction and cached to avoid parent
    //!< lookup
    Amount txFee;
    //!< Modified fees from mempool.  Set by the prioritisetransaction rpc.
    //!< We need this for ordering, but the above txFee for proper coinbase
    //!< calculations.
    Amount txModFee;
    //!< Cached total size of the transaction to avoid reserializing transaction
    size_t txSize;
    //!< Cached total number of SigOps
    uint64_t txSigOps;

    //!< Track the "order" of a transaction in a package. (Order is >= 0) Larger
    //!< number means it has more dependencies.  It is roughly the number of
    //!< dependencies this transaction has.
    uint64_t packageOrder;
    //!< Estimated package fees (This is guaranteed to be >= real fees)
    Amount packageFee;
    //!< Estimated package size (This is guaranteed to be >= real size)
    size_t packageSize;
    //!< Estimated package sigops (This is guaranteed to be >= real sigops)
    uint64_t packageSigOps;

    Amount maxPackageFee;
    size_t maxPackageSize;

    CBlockTemplateEntry(CTransactionRef _tx, Amount _realFees, Amount _modFees,
                        uint64_t _size, int64_t _sigOps)
        : tx(_tx), txFee(_realFees), txModFee(_modFees), txSize(_size),
          txSigOps(_sigOps), packageOrder(0), packageFee(_modFees),
          packageSize(_size), packageSigOps(_sigOps),
          maxPackageFee(Amount::zero()), maxPackageSize(0) {}

    // Calculate the feerate for this transaction.  Use the minimum of the
    // package feerate, or the transaction itself.  Parents TXNs should never
    // end up "paying for" child transactions.
    CFeeRate FeeRate() const {
        // If we are order 0, we know we can include the entire package for this
        // fee rate. As there is no other package with a higher feerate, that we
        // depend upon.
        if (GetOrder() == 0) {
            return CFeeRate(packageFee, packageSize);
        }

        return CFeeRate(txModFee, txSize);
    }

    size_t GetOrder() const {
        // If we're the maximum fee of the package, set our maximum to our fee,
        // and reset our ordeer.
        if (int64_t(packageSize) * maxPackageFee <
            int64_t(maxPackageSize) * packageFee) {
            // This package, is the highest feerate package.  Nothing that this
            // depends on could go in first.
            return 0;
        }
        return packageOrder;
    }

private:
    void AccountForParent(CBlockTemplateEntry &parent) {
        packageOrder = std::max(parent.packageOrder + 1, packageOrder);
        packageFee += parent.packageFee;
        packageSize += parent.packageSize;
        packageSigOps += parent.packageSigOps;

        // Propagate the maximum package fee, if it's bigger than anything we've
        // already found. We know that AT LEAST this package would get in before
        // us if we simply sort by package order. NOTE: Less than or equal to is
        // important here, both sides are equal to zero upon first pass.
        if (int64_t(parent.packageSize) * maxPackageFee <=
            int64_t(maxPackageSize) * parent.packageFee) {
            maxPackageFee = parent.packageFee;
            maxPackageSize = parent.packageSize;
        }
    }

    friend class BlockAssembler;
};

struct CBlockTemplate {
    CBlock block;

    std::vector<CBlockTemplateEntry> entries;
};

/** Generate a new block, without valid proof-of-work */
class BlockAssembler {
private:
    typedef std::unordered_set<TxId, SaltedTxidHasher> TxIdSet;

    // The constructed block template
    std::unique_ptr<CBlockTemplate> pblocktemplate;
    // A convenience pointer that always refers to the CBlock in pblocktemplate
    CBlock *pblock;

    // Configuration parameters for the block size
    uint64_t nMaxGeneratedBlockSize;
    CFeeRate blockMinFeeRate;

    // Information on the current status of the block
    uint64_t nBlockSize;
    uint64_t nBlockTx;
    uint64_t nBlockSigOps;
    Amount nFees;
    TxIdSet inBlock;

    // Chain context for the block
    int nHeight;
    int64_t nLockTimeCutoff;
    int64_t nMedianTimePast;

    const Config *config;
    const CTxMemPool *mempool;

    // Variables used for addPriorityTxs
    int lastFewTxs;

public:
    BlockAssembler(const Config &_config, const CTxMemPool &mempool);
    /** Construct a new block template with coinbase to scriptPubKeyIn */
    std::unique_ptr<CBlockTemplate>
    CreateNewBlock(const CScript &scriptPubKeyIn);

    uint64_t GetMaxGeneratedBlockSize() const { return nMaxGeneratedBlockSize; }

private:
    // utility functions
    /** Clear the block's state and prepare for assembling a new block */
    void resetBlock();
    /** Add a tx to the block */
    void AddToBlock(CTxMemPool::txiter iter);

    // Methods for how to add transactions to a block.
    /** Add transactions based on tx "priority" */
    void addPriorityTxs();
    /** Add transactions based on feerate including unconfirmed ancestors
     * Increments nPackagesSelected / nDescendantsUpdated with corresponding
     * statistics from the package selection (for logging statistics). */
    void addPackageTxs(int &nPackagesSelected, int &nDescendantsUpdated);

    /** Enum for the results from TestForBlock */
    enum class TestForBlockResult : uint8_t {
        TXFits = 0,
        TXCantFit = 1,
        BlockFinished = 3,
    };

    // helper function for addPriorityTxs
    /** Test if tx will still "fit" in the block */
    TestForBlockResult TestForBlock(CTxMemPool::txiter iter);
    /** Test if tx still has unconfirmed parents not yet in block */
    bool isStillDependent(CTxMemPool::txiter iter);
};

/** Modify the extranonce in a block */
void IncrementExtraNonce(const Config &config, CBlock *pblock,
                         const CBlockIndex *pindexPrev,
                         unsigned int &nExtraNonce);
int64_t UpdateTime(CBlockHeader *pblock, const Config &config,
                   const CBlockIndex *pindexPrev);
#endif // BITCOIN_MINER_H
