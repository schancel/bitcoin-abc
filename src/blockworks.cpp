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
#include "consensus/activation.h"
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
}

bool Blockworks::AddToBlock(const BlockEntry &entry) {
    if(!TestTransaction(entry)) {
        return false;
    }

    candidateBlock->entries.emplace_back(entry.tx, entry.txFee, entry.txSize, entry.txSigOps);
    nBlockSize += entry.txSize;
    ++nBlockTx;
    nBlockSigOps += entry.txSigOps;
    nFees += entry.txFee;
    return true;
}

bool Blockworks::TestTransaction(const BlockEntry &entry) {
    auto blockSizeWithTx = nBlockSize + entry.txSize;
    if (blockSizeWithTx >= nMaxGeneratedBlockSize) {
        return false;
    }

    auto maxBlockSigOps = GetMaxBlockSigOpsCount(blockSizeWithTx);
    if (nBlockSigOps + entry.txSigOps >= maxBlockSigOps) {
        return false;
    }

    // Must check that lock times are still valid. This can be removed once MTP
    // is always enforced as long as reorgs keep the mempool consistent.
    CValidationState state;
    if (!ContextualCheckTransaction(*config, *entry.tx.get(), state, nHeight,
                                    nLockTimeCutoff, nMedianTimePast)) {
        return false;
    }

    return true;
}

void Blockworks::resetBlock() {
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

    candidateBlock.reset(new BlockCandidate());
}

std::unique_ptr<BlockCandidate> Blockworks::GetBlock(const CScript &scriptPubKeyIn) {
    const CChainParams &chainparams = config->GetChainParams();
    int64_t nTimeStart = GetTimeMicros();

    // Acquire a read lock to the block data
    boost::unique_lock< boost::shared_mutex > lock(rwlock);

    pblock = &candidateBlock->block;

    resetBlock();
    
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
    
    // Add dummy coinbase tx as first transaction. It is updated at the end.
    pblock->vtx.emplace_back(CTransactionRef());

    // Add dummy coinbase tx as first transaction.  It is updated at the end.
    candidateBlock->entries.emplace_back(CTransactionRef(), -SATOSHI, -1, -1);

    // Create coinbase transaction.
    CMutableTransaction coinbaseTx;
    coinbaseTx.vin.resize(1);
    coinbaseTx.vin[0].prevout = COutPoint();
    coinbaseTx.vout.resize(1);
    coinbaseTx.vout[0].scriptPubKey = scriptPubKeyIn;
    coinbaseTx.vout[0].nValue = nFees;
        nFees + GetBlockSubsidy(nHeight, chainparams.GetConsensus());
    coinbaseTx.vin[0].scriptSig = CScript() << nHeight << OP_0;

    // Make sure the coinbase is big enough.
    uint64_t coinbaseSize =
        ::GetSerializeSize(coinbaseTx, SER_NETWORK, PROTOCOL_VERSION);
    if (coinbaseSize < MIN_TX_SIZE) {
        coinbaseTx.vin[0].scriptSig
            << std::vector<uint8_t>(MIN_TX_SIZE - coinbaseSize - 1);
    }

    // Add our packages to the block!
    AddTransactionsToBlock(g_mempool);

    candidateBlock->entries[0].tx = MakeTransactionRef(coinbaseTx);
    candidateBlock->entries[0].txFee = nFees;

    if (IsMagneticAnomalyEnabled(*config, pindexPrev)) {
        // If magnetic anomaly is enabled, we make sure transaction are
        // canonically ordered.
        // FIXME: Use a zipped list. See T479
        std::sort(std::begin(candidateBlock->entries) + 1,
                  std::end(candidateBlock->entries),
                  [](const BlockEntry &a, const BlockEntry &b)
                      -> bool { return a.tx->GetId() < b.tx->GetId(); });
    }
    

    int64_t nTime1 = GetTimeMicros();

    uint64_t nSerializeSize =
        GetSerializeSize(*pblock, SER_NETWORK, PROTOCOL_VERSION);

    LogPrintf("CreateNewBlock(): total size: %u txs: %u fees: %ld sigops %d\n",
              nSerializeSize, nBlockTx, nFees, nBlockSigOps);

    // Fill in header.
    pblock->hashPrevBlock = pindexPrev->GetBlockHash();
    UpdateTime(pblock, *config, pindexPrev);
    pblock->nBits = GetNextWorkRequired(pindexPrev, pblock, *config);
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

    return std::move(candidateBlock);
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

            // Add the parent data to this transaction
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

    // Visit all the transactions in package feeRate order, adding them to the block in topological order.
    // Complexity: O(n + m) (and some change for visiting diamond dependencies multiple times)
    //      we charge for this as if they were two seperate transactions.
    //
    // Note: we only need to visit every transaction one time.
    std::unordered_set<TxId, SaltedTxidHasher> txnsAdded;
    std::unordered_set<TxId, SaltedTxidHasher> txnsInvalid;

    for( const auto &entry : txPool ) {
        TxId txId = entry.tx->GetId();
        // Don't reprocess transactions
        if(txnsAdded.count(txId) != 0 || txnsInvalid.count(txId) != 0) {
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
            txnsInvalid.insert(txId);
            goto invalid;
        }

        // Check if package sigOps would fit in the block
        if (nBlockSigOps + entry.packageSigOps >= maxBlockSigOps) {
            txnsInvalid.insert(txId);
            goto invalid;
        }

        ancestorStack.push(entry);
        // We do a DFS incase a transaction fails, and we add them to a stack for popping into the block. 
        while (!ancestorStack.empty()){
            const BlockEntry& ancestor = ancestorStack.top();
            const TxId &ancestorId = ancestor.tx->GetId();

            ancestorStack.pop();
            // Add it to the stack of items we will add once we have checked all the parents.
            addStack.push(ancestor);

            // Must check that lock times are still valid. This can be removed once MTP
            // is always enforced as long as reorgs keep the mempool consistent.
            //
            // Old code checks the transaction in context.  It doesn't make sense to me though...
            CValidationState state;
            if (!ContextualCheckTransaction(*config, *ancestor.tx.get(), state, nHeight,
                                            nLockTimeCutoff, nMedianTimePast)) {
                // A parent is invalid, we will need to skip the rest of the transactions
                // So we don't include (now) invalid child. We will visit them later.
                // So we can find all it's children during invalid handling.
                txnsInvalid.insert(ancestorId);
                goto invalid;
            }

            // Push all the ancestors on our DFS stack.
            for (const auto &txIn : ancestor.tx->vin ) {
                const TxId &parentId = txIn.prevout.GetTxId();
                size_t hasBeenAdded = txnsAdded.count(parentId) != 0;
                size_t isInvalid = txnsInvalid.count(parentId) != 0;
                if(isInvalid) {
                    // We must have visited one of the parents before when handling a different package.
                    // It was invalid, and we need to mark this invalid as well.
                    txnsInvalid.insert(ancestorId);
                    goto invalid;
                }
                if(hasBeenAdded) {
                    // Has already been added to the block. We can skip checking it.
                    continue;
                }
                // Don't yet mark the transaction as visited.  We may need to visit it twice in this loop to ensure
                // topological ordering when popping off the stack.

                auto it = txLocations.find(parentId);
                if( it == txLocations.end()) {
                    // Confirmed parent transaction
                    // (We know this because the mempool we have was guaranteed to be consistent)
                    continue;
                }
                const BlockEntry& ancestorParent = txPool[it->second];

                // Record the parent so we can visit it later.
                ancestorStack.push(ancestorParent);
            }
        }
        // We know we can add the transaction here.
        // Every item in the package has passed requirements.
        while(!addStack.empty()) {
            const BlockEntry &entryToAdd = addStack.top();
            const TxId &addTxID = entryToAdd.tx->GetId();
            size_t hasBeenAdded = txnsAdded.count(addTxID) != 0;

            addStack.pop();
            // The addStack can have duplicates, so we need to ensure we don't add
            // the same transaction twice in this loop.
            if(hasBeenAdded) {
                continue;
            }
            txnsAdded.insert(addTxID);
            candidateBlock->entries.emplace_back(entry.tx, entry.txFee, entry.txSize, entry.txSigOps);
            nBlockSize += entry.txSize;
            ++nBlockTx;
            nBlockSigOps += entry.txSigOps;
            nFees += entry.txFee;
        }
        continue;
invalid:
        // Mark all children of the invalid transaction as themselves invalid.
        // Don't add anything in this package to a block.
        while(!addStack.empty()) {
            const BlockEntry &child = addStack.top();
            addStack.pop();
            // If we are here, it means we marked one of the parents of an addStack item invalid.
            // because the stack is in topological order, we can be sure we will mark invalid transactions in order
            // going all the way back up to the root transaction.
            for (const auto &txIn : child.tx->vin ) {
                const TxId &parentId = txIn.prevout.GetTxId();
                size_t isInvalid = txnsInvalid.count(parentId) != 0;
                if(isInvalid) {
                    txnsInvalid.insert(child.tx->GetId());

                    // We can stop here, we only need to find one invalid parent to know this is invalid.
                    break;
                }
            }
        }
        continue;
    }
}
