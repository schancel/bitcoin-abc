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
#include <queue>
#include <algorithm>
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

std::unique_ptr<CBlock> Blockworks::GetBlock(std::unique_ptr<CBlock>, const CScript &scriptPubKeyIn) {
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

    AddTransactionsToBlock(g_mempool);

    // TODO Ensure CTOR
    //std::sort(std::begin(pblocktemplate->entries) + 1,
    //      std::end(pblocktemplate->entries),
    //      [](const CBlockTemplateEntry &a, const CBlockTemplateEntry &b)
    //          -> bool { return a.tx->GetId() < b.tx->GetId(); });
    //

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

void Blockworks::AddTransactionsToBlock(CTxMemPool &mempool) {
    std::vector<BlockEntry> txPool;
    txPool.reserve(txPool.size());

    std::unordered_map<TxId, size_t, SaltedTxidHasher> txLocations;
    // Fetch all the transactions in dependency order.
    // Complexity: O(n)
    //
    // We want a copy quickly so we can release the mempool lock ASAP.
    {
        LOCK2(cs_main, mempool.cs);
        txPool.reserve(mempool.mapTx.size());

        size_t idx = 0;
        for(const auto &mempoolEntry : mempool.mapTx.get<insertion_order>()) {
            const CTransactionRef tx = mempoolEntry.GetSharedTx();
            txPool.emplace_back(mempoolEntry.GetSharedTx(), mempoolEntry.GetFee(), mempoolEntry.GetTxSize(), mempoolEntry.GetSigOpCount() );
            txLocations[tx->GetId()] = idx;
            idx += 1;
        }
    }

    // Figure out how many parents transactions have.
    // Complexity: O(n + m) where n is transactions, and m is edges.
    //
    // All the dependencies of a transaction appear before the transaction.
    // So we can iterate forward, and simply add all the parents dependency count to our count.
    // Note: If there is a diamond shaped inheritance, we will multi-count.
    // We don't care.
    // We will still maintain an invarent that children come after parents when sorting by the value we calculate here.
    std::unordered_set<TxId, SaltedTxidHasher> seen;
    for( auto &entry : txPool ) {
        // Clear out seen list, for the next transaction.
        seen.clear();
        for (const auto &txIn : entry.tx->vin ) {
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

            // Add the parent to us.
            entry.AccountForParent(txPool[it->second]);
        }
    }

    // Now sort them by package feeRate.
    // Complexity: O(n log n)
    //
    // We know that we have optimally sorted feeRates, except for the single 
    // case where a parent has two children that are paying a high fee.
    std::sort(txPool.begin(), txPool.end(), [](const BlockEntry &a, const BlockEntry &b) {
        // We don't care about truncation of fees.  If the client doesn't get a one sat of fees counted, oh well.
        // The code for avoiding division is atrocious.
        return a.FeeRate() > b.FeeRate();
    });

    // Update the locations of all the transactions so we can find them again later.
    // Complexity: O(n + m) where n is transactions, and m is edges.
    size_t idx = 0;
    for(const auto &blockEntry : txPool) {
            const CTransactionRef tx = blockEntry.tx;
            txLocations[tx->GetId()] = idx;
            idx += 1;
    }

    std::vector<BlockEntry> blockTxns;
    blockTxns.reserve(txPool.size());
    // Visit all the transactions in package feeRate order, adding them to the block in topological order.
    // Complexity: O(n + m)
    //
    // Note: we only need to visit every transaction one time.
    std::unordered_set<TxId, SaltedTxidHasher> txnsVisited;
    std::unordered_set<TxId, SaltedTxidHasher> txnsInvalid;

    for( const auto &entry : txPool ) {
        TxId txId = entry.tx->GetId();
        if(txnsVisited.count(txId) != 0 || txnsInvalid.count(txId)) {
            // We know this transaction is either in one of the following:
            //  1. Confirmed
            //  2. Added to the block
            //  3. Invalid
            // Therefor skip it.
            continue;
        }
       
        // Visit and include all the parents in the block, checking if they are valid.
        std::stack<std::reference_wrapper<const BlockEntry>> ancestorStack;
        std::stack<std::reference_wrapper<const BlockEntry>> addStack;

        // Check if package size would fit in the block.
        // We won't need to check it for every ancestor below, because their values are included here already
        auto blockSizeWithTx = nBlockSize + entry.packageSize;
        auto maxBlockSigOps = GetMaxBlockSigOpsCount(blockSizeWithTx);
        if (blockSizeWithTx >= nMaxGeneratedBlockSize) {
            goto invalid;
        }

        // Check if package sigOps would fit in the block
        if (nBlockSigOps + entry.packageSigOps >= maxBlockSigOps) {
            goto invalid;
        }

        ancestorStack.push(entry);
        // We do a DFS incase a transaction fails, and we add them to a stack for popping into the block. 
        while (!ancestorStack.empty()){
            const BlockEntry& ancestor = ancestorStack.top();
            ancestorStack.pop();

            for (const auto &txIn : ancestor.tx->vin ) {
                const TxId &parentId = txIn.prevout.GetTxId();
                size_t hasBeenVisited = txnsVisited.count(parentId) != 0;
                size_t isInvalid = txnsInvalid.count(parentId) != 0;
                if(isInvalid) {
                    // We have visited the transaction, but it was invalid when handling a previous package
                    // Goto the invalid txn handling, so that we flag all the children of this transaction
                    // as invalid as well.
                    goto invalid;
                }
                if(hasBeenVisited) {
                    // We know this transaction is already added to the block, or has already been visited
                    // in the current loop already.
                    // Therefor skip it.
                    continue;
                }
                // It's okay to visit here, because we're doing a DFS of parents.
                // If any parent of this transaction fails, we can't add it anyways.
                // Therefor, we can skip it in the future.
                txnsVisited.insert(parentId);

                auto it = txLocations.find(parentId);
                if( it == txLocations.end()) {
                    // Confirmed parent transaction
                    // (We know this because the mempool we have was guaranteed to be consistent)
                    continue;
                }

                const BlockEntry& ancestorParent = txPool[it->second];

                // Must check that lock times are still valid. This can be removed once MTP
                // is always enforced as long as reorgs keep the mempool consistent.
                //
                // Old code checks the transaction in context.  It doesn't make sense to me though...
                CValidationState state;
                if (!ContextualCheckTransaction(*config, *ancestorParent.tx.get(), state, nHeight,
                                                nLockTimeCutoff, nMedianTimePast)) {
                    // A parent is invalid, we will need to skip the rest of the transactions
                    // So we don't include (now) invalid child. We will visit them later.
                    goto invalid;
                }
                addStack.push(ancestorParent);
                ancestorStack.push(ancestorParent);
            }
        }
        // We know we can add the transaction here.
        //Every item in the package has passed requirements.
        while(!addStack.empty()) {
            blockTxns.push_back(addStack.top());
            addStack.pop();
        }
        continue;
invalid:
        while(!addStack.empty()) {
            txnsInvalid.insert(addStack.top().get().tx->GetId());
            addStack.pop();
        }
        continue;
    }

    // TODO:
}
