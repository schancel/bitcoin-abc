// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2017 The Bitcoin Core developers
// Copyright (c) 2017-2019 The Bitcoin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "blockworks.h"
#include "config.h"
#include "chainparams.h"
#include "consensus/validation.h"
#include "consensus/tx_verify.h"
#include "logging.h"
#include "miner.h"
#include "policy/policy.h"
#include "primitives/block.h"
#include "timedata.h"
#include "txmempool.h"
#include "utiltime.h"
#include "util.h"
#include "validation.h"
#include <boost/multi_index/ordered_index.hpp>
#include <boost/multi_index_container.hpp>

#include <cstdint>
#include <memory>

Blockworks::Blockworks(const Config &_config, CBlockIndex *_pindexPrev)
: config(&_config) {

    // Keep track of what we're mining on top of.
    pindexPrev = _pindexPrev;

    // Reserve space for coinbase tx.
    nBlockSize = 1000;
    nBlockSigOps = 100;

    // How big can we make this block?
    nMaxGeneratedBlockSize = ComputeMaxGeneratedBlockSize(*config, pindexPrev);

    // These counters do not include coinbase tx.
    nBlockTx = 0;
    nFees = Amount::zero();

    nHeight = pindexPrev->nHeight + 1;
    nMedianTimePast = pindexPrev->GetMedianTimePast();
    nLockTimeCutoff = (STANDARD_LOCKTIME_VERIFY_FLAGS & LOCKTIME_MEDIAN_TIME_PAST)
            ? nMedianTimePast
            : GetAdjustedTime();
}

bool Blockworks::AddToBlock(const CTxMemPoolEntry &entry) {
    if(!TestTransaction(entry)) {
        return false;
    }

    entries.emplace_back(entry.GetSharedTx(), entry.GetFee(), entry.GetSigOpCount());
    nBlockSize += entry.GetTxSize();
    ++nBlockTx;
    nBlockSigOps += entry.GetSigOpCount();
    nFees += entry.GetFee();
    return true;
}

bool Blockworks::TestTransaction(const CTxMemPoolEntry &entry) {
    auto blockSizeWithTx = nBlockSize + entry.GetTxSize();
    if (blockSizeWithTx >= nMaxGeneratedBlockSize) {
        return false;
    }
    // Acquire write access to block;
    boost::unique_lock< boost::shared_mutex > lock(rwlock);

    auto maxBlockSigOps = GetMaxBlockSigOpsCount(blockSizeWithTx);
    if (nBlockSigOps + entry.GetSigOpCount() >= maxBlockSigOps) {
        return false;
    }

    // Must check that lock times are still valid. This can be removed once MTP
    // is always enforced as long as reorgs keep the mempool consistent.
    CValidationState state;
    if (!ContextualCheckTransaction(*config, entry.GetTx(), state, nHeight,
                                    nLockTimeCutoff, nMedianTimePast)) {
        return false;
    }

    return true;
}

std::unique_ptr<CBlock> Blockworks::GetBlock(const CScript &scriptPubKeyIn) {
    const CChainParams &chainparams = config->GetChainParams();
    int64_t nTimeStart = GetTimeMicros();
    std::unique_ptr<CBlock> pblock(new CBlock());

    // Acquire a read lock to the block data
    boost::shared_lock< boost::shared_mutex > lock(rwlock);

    // Add dummy coinbase tx as first transaction. It is updated at the end.
    pblock->vtx.emplace_back(CTransactionRef());
    
    pblock->nVersion =
        ComputeBlockVersion(pindexPrev, chainparams.GetConsensus());
    // -regtest only: allow overriding block.nVersion with
    // -blockversion=N to test forking scenarios
    if (chainparams.MineBlocksOnDemand()) {
        pblock->nVersion = gArgs.GetArg("-blockversion", pblock->nVersion);
    }

    pblock->nTime = GetAdjustedTime();
    nMaxGeneratedBlockSize = ComputeMaxGeneratedBlockSize(*config, pindexPrev);

    nMedianTimePast = pindexPrev->GetMedianTimePast();
    nLockTimeCutoff =
        (STANDARD_LOCKTIME_VERIFY_FLAGS & LOCKTIME_MEDIAN_TIME_PAST)
            ? nMedianTimePast
            : pblock->GetBlockTime();

    int64_t nTime1 = GetTimeMicros();

    // Create coinbase transaction.
    CMutableTransaction coinbaseTx;
    coinbaseTx.vin.resize(1);
    coinbaseTx.vin[0].prevout = COutPoint();
    coinbaseTx.vout.resize(1);
    coinbaseTx.vout[0].scriptPubKey = scriptPubKeyIn;
    coinbaseTx.vout[0].nValue =
        nFees + GetBlockSubsidy(nHeight, chainparams.GetConsensus());
    coinbaseTx.vin[0].scriptSig = CScript() << nHeight << OP_0;

    // Make sure the coinbase is big enough.
    uint64_t coinbaseSize =
        ::GetSerializeSize(coinbaseTx, SER_NETWORK, PROTOCOL_VERSION);
    if (coinbaseSize < MIN_TX_SIZE) {
        coinbaseTx.vin[0].scriptSig
            << std::vector<uint8_t>(MIN_TX_SIZE - coinbaseSize - 1);
    }

    pblock->vtx.push_back(MakeTransactionRef(coinbaseTx));

    // TODO: Add transactions to the block here.

    uint64_t nSerializeSize =
        GetSerializeSize(*pblock, SER_NETWORK, PROTOCOL_VERSION);

    LogPrintf("CreateNewBlock(): total size: %u txs: %u fees: %ld sigops %d\n",
              nSerializeSize, nBlockTx, nFees, nBlockSigOps);

    // Fill in header.
    pblock->hashPrevBlock = pindexPrev->GetBlockHash();
    UpdateTime(pblock.get(), *config, pindexPrev);
    pblock->nBits = GetNextWorkRequired(pindexPrev, pblock.get(), *config);
    pblock->nNonce = 0;

    CValidationState state;
    BlockValidationOptions validationOptions(false, false);
    if (!TestBlockValidity(*config, state, *pblock, pindexPrev,
                           validationOptions)) {
        throw std::runtime_error(strprintf("%s: TestBlockValidity failed: %s",
                                           __func__,
                                           FormatStateMessage(state)));
    }
    int64_t nTime2 = GetTimeMicros();

    LogPrint(BCLog::BENCH,
             "CreateNewBlock() packages: %.2fms validity: %.2fms (total %.2fms)\n",
             0.001 * (nTime1 - nTimeStart), 0.001 * (nTime2 - nTime1),
             0.001 * (nTime2 - nTimeStart));

    return pblock;
}

// Keep track of transactions to-be-added added to a block, and some various metadata.
struct BlockEntry {
public:
    CTransactionRef tx;
    //!< Cached to avoid expensive parent-transaction lookups
    Amount fee;
    //!< ... and avoid recomputing tx size
    size_t txSize;
    //!< ... Track number of sigops
    uint64_t sigOps;

    //!< Track the height and time at which tx was final
    LockPoints lockPoints;

    //!< Track information about the package
    uint64_t packageCount;
    Amount packageFee;
    size_t packageSize;
    uint64_t packageSigOps;


    BlockEntry(CTransactionRef _tx, Amount _fees, uint64_t _txSize, int64_t _sigOps) 
    : tx(_tx), nFee(_fees), txSize(_txSize), sigOps(_sigOps),
      packageCount(1), packageSize(_txSize), packageSigOps(_sigOps), packageFee(_fees)
    { }
};

static void BuildBlock(CTxMemPool &mempool) {
    std::vector<std::shared_ptr<BlockEntry>> unaddedPool;
    std::unordered_map<TxId, size_t, SaltedTxidHasher> txLocations;
    {
        // Fetch all the transactions in dependency order. O(n)
        // We want a copy quickly so we can release the mempool lock ASAP.
        LOCK2(cs_main, mempool.cs);
        size_t idx = 0;
        for(const auto &mempoolEntry : mempool.mapTx.get<insertion_order>()) {
            const CTransactionRef tx = mempoolEntry.GetSharedTx();
            unaddedPool.emplace_back(mempoolEntry.GetTx(), mempoolEntry.GetFee(), mempoolEntry.GetTxSize() );
            txLocations[tx->GetId()] = idx;
            idx += 1;
        }
    }

    // Figure out how many parents transactions have.  O(n + m)
    // All the dependencies of a transaction appear before the transaction.
    // So we can iterate forward, and simply add all the parents dependency count to our count.
    // Note: If there is a diamond shaped inheritance, we will multi-count.
    // We don't care.
    // We will still maintain an invarent that children come after parents when sorting by the value we calculate here.
    std::unordered_set<TxId, SaltedTxidHasher> seen;
    for( const auto entry : unaddedPool ) {
        // Clear out seen list, for the next transaction.
        seen.clear();
        for (const auto &txIn : entry->tx->vin ) {
            const TxId &parentId = txIn.prevout.GetTxId();
            if(seen.count(parentId) != 0) {
                continue;
            }
            seen.insert(parentId);
            auto it = txLocations.find(parentId);
            if( it == txLocations.end()) {
                // Confirmed parent transaction
                // (We know this because the mempool we have was guaranteed to be consistent)
                continue;
            }

            // Add our parents to us.
            entry->packageCount += unaddedPool[it->second]->packageCount;
        }
    }

    // Now sort them by package feeRate
    std::sort(unaddedPool.begin(), unaddedPool.end(), [](const BlockEntry& a, const BlockEntry &b) {
        auto aFee = a.packageFee / int64_t(a.packageSize) < a.fee / int64_t(a.packageSize)

        return GetFee<false>(a.packageSize) 
    });
}