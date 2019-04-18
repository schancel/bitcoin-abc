// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2016 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "miner.h"

#include "amount.h"
#include "chain.h"
#include "chainparams.h"
#include "coins.h"
#include "config.h"
#include "consensus/activation.h"
#include "consensus/consensus.h"
#include "consensus/merkle.h"
#include "consensus/tx_verify.h"
#include "consensus/validation.h"
#include "hash.h"
#include "net.h"
#include "policy/policy.h"
#include "pow.h"
#include "primitives/transaction.h"
#include "script/standard.h"
#include "timedata.h"
#include "txmempool.h"
#include "util.h"
#include "utilmoneystr.h"
#include "validation.h"
#include "validationinterface.h"

#include <algorithm>
#include <queue>
#include <utility>

//////////////////////////////////////////////////////////////////////////////
//
// BitcoinMiner
//

//
// Unconfirmed transactions in the memory pool often depend on other
// transactions in the memory pool. When we select transactions from the
// pool, we select by highest priority or fee rate, so we might consider
// transactions that depend on transactions that aren't yet in the block.

uint64_t nLastBlockTx = 0;
uint64_t nLastBlockSize = 0;

int64_t UpdateTime(CBlockHeader *pblock, const Config &config,
                   const CBlockIndex *pindexPrev) {
    int64_t nOldTime = pblock->nTime;
    int64_t nNewTime =
        std::max(pindexPrev->GetMedianTimePast() + 1, GetAdjustedTime());

    if (nOldTime < nNewTime) {
        pblock->nTime = nNewTime;
    }

    const Consensus::Params &consensusParams =
        config.GetChainParams().GetConsensus();

    // Updating time can change work required on testnet:
    if (consensusParams.fPowAllowMinDifficultyBlocks) {
        pblock->nBits = GetNextWorkRequired(pindexPrev, pblock, config);
    }

    return nNewTime - nOldTime;
}

uint64_t ComputeMaxGeneratedBlockSize(const Config &config,
                                      const CBlockIndex *pindexPrev) {
    // Block resource limits
    // If -blockmaxsize is not given, limit to DEFAULT_MAX_GENERATED_BLOCK_SIZE
    // If only one is given, only restrict the specified resource.
    // If both are given, restrict both.
    uint64_t nMaxGeneratedBlockSize = DEFAULT_MAX_GENERATED_BLOCK_SIZE;
    if (gArgs.IsArgSet("-blockmaxsize")) {
        nMaxGeneratedBlockSize =
            gArgs.GetArg("-blockmaxsize", DEFAULT_MAX_GENERATED_BLOCK_SIZE);
    }

    // Limit size to between 1K and MaxBlockSize-1K for sanity:
    nMaxGeneratedBlockSize =
        std::max(uint64_t(1000), std::min(config.GetMaxBlockSize() - 1000,
                                          nMaxGeneratedBlockSize));

    return nMaxGeneratedBlockSize;
}

BlockAssembler::BlockAssembler(const Config &_config, const CTxMemPool &mpool)
    : config(&_config), mempool(&mpool) {

    if (gArgs.IsArgSet("-blockmintxfee")) {
        Amount n = Amount::zero();
        ParseMoney(gArgs.GetArg("-blockmintxfee", ""), n);
        blockMinFeeRate = CFeeRate(n);
    } else {
        blockMinFeeRate = CFeeRate(DEFAULT_BLOCK_MIN_TX_FEE_PER_KB);
    }

    LOCK(cs_main);
    nMaxGeneratedBlockSize =
        ComputeMaxGeneratedBlockSize(*config, chainActive.Tip());
}

void BlockAssembler::resetBlock() {
    inBlock.clear();

    // Reserve space for coinbase tx.
    nBlockSize = 1000;
    nBlockSigOps = 100;

    // These counters do not include coinbase tx.
    nBlockTx = 0;
    nFees = Amount::zero();

    lastFewTxs = 0;
}

static const std::vector<uint8_t>
getExcessiveBlockSizeSig(const Config &config) {
    std::string cbmsg = "/EB" + getSubVersionEB(config.GetMaxBlockSize()) + "/";
    const char *cbcstr = cbmsg.c_str();
    std::vector<uint8_t> vec(cbcstr, cbcstr + cbmsg.size());
    return vec;
}

std::unique_ptr<CBlockTemplate>
BlockAssembler::CreateNewBlock(const CScript &scriptPubKeyIn) {
    int64_t nTimeStart = GetTimeMicros();

    resetBlock();

    pblocktemplate.reset(new CBlockTemplate());
    if (!pblocktemplate.get()) {
        return nullptr;
    }

    // Pointer for convenience.
    pblock = &pblocktemplate->block;

    // Add dummy coinbase tx as first transaction.  It is updated at the end.
    pblocktemplate->entries.emplace_back(CTransactionRef(), -SATOSHI, 0, -1);

    LOCK2(cs_main, mempool->cs);
    CBlockIndex *pindexPrev = chainActive.Tip();
    nHeight = pindexPrev->nHeight + 1;

    const CChainParams &chainparams = config->GetChainParams();
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

    addPriorityTxs();
    int nPackagesSelected = 0;
    int nDescendantsUpdated = 0;
    addPackageTxs(nPackagesSelected, nDescendantsUpdated);

    if (IsMagneticAnomalyEnabled(*config, pindexPrev)) {
        // If magnetic anomaly is enabled, we make sure transaction are
        // canonically ordered.
        // FIXME: Use a zipped list. See T479
        std::sort(std::begin(pblocktemplate->entries) + 1,
                  std::end(pblocktemplate->entries),
                  [](const CBlockTemplateEntry &a, const CBlockTemplateEntry &b)
                      -> bool { return a.tx->GetId() < b.tx->GetId(); });
    }

    int64_t nTime1 = GetTimeMicros();

    nLastBlockTx = nBlockTx;
    nLastBlockSize = nBlockSize;

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

    pblocktemplate->entries[0].tx = MakeTransactionRef(coinbaseTx);
    pblocktemplate->entries[0].txFee = -1 * nFees;

    uint64_t nSerializeSize =
        GetSerializeSize(*pblock, SER_NETWORK, PROTOCOL_VERSION);

    LogPrintf("CreateNewBlock(): total size: %u txs: %u fees: %ld sigops %d\n",
              nSerializeSize, nBlockTx, nFees, nBlockSigOps);

    // Fill in header.
    pblock->hashPrevBlock = pindexPrev->GetBlockHash();
    UpdateTime(pblock, *config, pindexPrev);
    pblock->nBits = GetNextWorkRequired(pindexPrev, pblock, *config);
    pblock->nNonce = 0;
    pblocktemplate->entries[0].txSigOps = GetSigOpCountWithoutP2SH(
        *pblocktemplate->entries[0].tx, STANDARD_CHECKDATASIG_VERIFY_FLAGS);

    // Copy all the transactions into the block
    // FIXME: This should be removed as it is significant overhead.
    // See T479
    for (const CBlockTemplateEntry &tx : pblocktemplate->entries) {
        pblock->vtx.push_back(tx.tx);
    }

    CValidationState state;
    BlockValidationOptions validationOptions(false, false);
    //if (!TestBlockValidity(*config, state, *pblock, pindexPrev,
    //                       validationOptions)) {
    //    throw std::runtime_error(strprintf("%s: TestBlockValidity failed: %s",
    //                                       __func__,
    //                                       FormatStateMessage(state)));
    //}
    int64_t nTime2 = GetTimeMicros();

    LogPrint(BCLog::BENCH,
             "CreateNewBlock() packages: %.2fms (%d packages, %d updated "
             "descendants), validity: %.2fms (total %.2fms)\n",
             0.001 * (nTime1 - nTimeStart), nPackagesSelected,
             nDescendantsUpdated, 0.001 * (nTime2 - nTime1),
             0.001 * (nTime2 - nTimeStart));

    return std::move(pblocktemplate);
}

bool BlockAssembler::isStillDependent(CTxMemPool::txiter iter) {
    for (CTxMemPool::txiter parent : mempool->GetMemPoolParents(iter)) {
        if (!inBlock.count(parent->GetTx().GetId())) {
            return true;
        }
    }
    return false;
}

void BlockAssembler::onlyUnconfirmed(CTxMemPool::setEntries &testSet) {
    for (CTxMemPool::setEntries::iterator iit = testSet.begin();
         iit != testSet.end();) {
        // Only test txs not already in the block.
        if (inBlock.count((*iit)->GetTx().GetId())) {
            testSet.erase(iit++);
        } else {
            iit++;
        }
    }
}

bool BlockAssembler::TestPackage(uint64_t packageSize,
                                 int64_t packageSigOps) const {
    auto blockSizeWithPackage = nBlockSize + packageSize;
    if (blockSizeWithPackage >= nMaxGeneratedBlockSize) {
        return false;
    }

    if (nBlockSigOps + packageSigOps >=
        GetMaxBlockSigOpsCount(blockSizeWithPackage)) {
        return false;
    }

    return true;
}

/**
 * Perform transaction-level checks before adding to block:
 * - Transaction finality (locktime)
 * - Serialized size (in case -blockmaxsize is in use)
 */
bool BlockAssembler::TestPackageTransactions(
    const CTxMemPool::setEntries &package) {
    uint64_t nPotentialBlockSize = nBlockSize;
    for (const CTxMemPool::txiter it : package) {
        CValidationState state;
        if (!ContextualCheckTransaction(*config, it->GetTx(), state, nHeight,
                                        nLockTimeCutoff, nMedianTimePast)) {
            return false;
        }

        uint64_t nTxSize =
            ::GetSerializeSize(it->GetTx(), SER_NETWORK, PROTOCOL_VERSION);
        if (nPotentialBlockSize + nTxSize >= nMaxGeneratedBlockSize) {
            return false;
        }

        nPotentialBlockSize += nTxSize;
    }

    return true;
}

BlockAssembler::TestForBlockResult
BlockAssembler::TestForBlock(CTxMemPool::txiter it) {
    auto blockSizeWithTx =
        nBlockSize +
        ::GetSerializeSize(it->GetTx(), SER_NETWORK, PROTOCOL_VERSION);
    if (blockSizeWithTx >= nMaxGeneratedBlockSize) {
        if (nBlockSize > nMaxGeneratedBlockSize - 100 || lastFewTxs > 50) {
            return TestForBlockResult::BlockFinished;
        }

        if (nBlockSize > nMaxGeneratedBlockSize - 1000) {
            lastFewTxs++;
        }

        return TestForBlockResult::TXCantFit;
    }

    auto maxBlockSigOps = GetMaxBlockSigOpsCount(blockSizeWithTx);
    if (nBlockSigOps + it->GetSigOpCount() >= maxBlockSigOps) {
        // If the block has room for no more sig ops then flag that the block is
        // finished.
        // TODO: We should consider adding another transaction that isn't very
        // dense in sigops instead of bailing out so easily.
        if (nBlockSigOps > maxBlockSigOps - 2) {
            return TestForBlockResult::BlockFinished;
        }

        // Otherwise attempt to find another tx with fewer sigops to put in the
        // block.
        return TestForBlockResult::TXCantFit;
    }

    // Must check that lock times are still valid. This can be removed once MTP
    // is always enforced as long as reorgs keep the mempool consistent.
    CValidationState state;
    if (!ContextualCheckTransaction(*config, it->GetTx(), state, nHeight,
                                    nLockTimeCutoff, nMedianTimePast)) {
        return TestForBlockResult::TXCantFit;
    }

    return TestForBlockResult::TXFits;
}

void BlockAssembler::AddToBlock(CTxMemPool::txiter iter) {
    pblocktemplate->entries.emplace_back(iter->GetSharedTx(), iter->GetFee(),
                                         iter->GetTxSize(),
                                         iter->GetSigOpCount());
    nBlockSize += iter->GetTxSize();
    ++nBlockTx;
    nBlockSigOps += iter->GetSigOpCount();
    nFees += iter->GetFee();
    inBlock.insert(iter->GetTx().GetId());

    bool fPrintPriority =
        gArgs.GetBoolArg("-printpriority", DEFAULT_PRINTPRIORITY);
    if (fPrintPriority) {
        double dPriority = iter->GetPriority(nHeight);
        Amount dummy;
        mempool->ApplyDeltas(iter->GetTx().GetId(), dPriority, dummy);
        LogPrintf(
            "priority %.1f fee %s txid %s\n", dPriority,
            CFeeRate(iter->GetModifiedFee(), iter->GetTxSize()).ToString(),
            iter->GetTx().GetId().ToString());
    }
}

// TODO: Temporary adapter, delete this eventually.
int BlockAssembler::UpdatePackagesForAdded(const TxIdSet &alreadyAdded,  indexed_modified_transaction_set &mapModifiedTx) {
    CTxMemPool::setEntries entries;
    for (const TxId &id : alreadyAdded) {
        entries.insert(mempool->mapTx.find(id));
    }
    return UpdatePackagesForAdded(entries, mapModifiedTx);
}

int BlockAssembler::UpdatePackagesForAdded(
    const CTxMemPool::setEntries &alreadyAdded,
    indexed_modified_transaction_set &mapModifiedTx) {
    int nDescendantsUpdated = 0;
    for (const CTxMemPool::txiter it : alreadyAdded) {
        CTxMemPool::setEntries descendants;
        mempool->CalculateDescendants(it, descendants);
        // Insert all descendants (not yet in block) into the modified set.
        for (CTxMemPool::txiter desc : descendants) {
            if (alreadyAdded.count(desc)) {
                continue;
            }

            ++nDescendantsUpdated;
            modtxiter mit = mapModifiedTx.find(desc);
            if (mit == mapModifiedTx.end()) {
                CTxMemPoolModifiedEntry modEntry(desc);
                modEntry.nSizeWithAncestors -= it->GetTxSize();
                modEntry.nBillableSizeWithAncestors -= it->GetTxBillableSize();
                modEntry.nModFeesWithAncestors -= it->GetModifiedFee();
                modEntry.nSigOpCountWithAncestors -= it->GetSigOpCount();
                mapModifiedTx.insert(modEntry);
            } else {
                mapModifiedTx.modify(mit, update_for_parent_inclusion(it));
            }
        }
    }

    return nDescendantsUpdated;
}

// Skip entries in mapTx that are already in a block or are present in
// mapModifiedTx (which implies that the mapTx ancestor state is stale due to
// ancestor inclusion in the block). Also skip transactions that we've already
// failed to add. This can happen if we consider a transaction in mapModifiedTx
// and it fails: we can then potentially consider it again while walking mapTx.
// It's currently guaranteed to fail again, but as a belt-and-suspenders check
// we put it in failedTx and avoid re-evaluation, since the re-evaluation would
// be using cached size/sigops/fee values that are not actually correct.
bool BlockAssembler::SkipMapTxEntry(
    CTxMemPool::txiter it, indexed_modified_transaction_set &mapModifiedTx,
    const TxIdSet &failedTx) {
    assert(it != mempool->mapTx.end());
    const TxId &txId = it->GetTx().GetId();
    return mapModifiedTx.count(it) || inBlock.count(txId) || failedTx.count(txId);
}

void BlockAssembler::SortForBlock(
    const CTxMemPool::setEntries &package, CTxMemPool::txiter entry,
    std::vector<CTxMemPool::txiter> &sortedEntries) {
    // Sort package by ancestor count. If a transaction A depends on transaction
    // B, then A's ancestor count must be greater than B's. So this is
    // sufficient to validly order the transactions for block inclusion.
    sortedEntries.clear();
    sortedEntries.insert(sortedEntries.begin(), package.begin(), package.end());
    std::sort(sortedEntries.begin(), sortedEntries.end(),
              CompareTxIterByAncestorCount());
}

/**
 * addPackageTx includes transactions paying a fee by ensuring that
 * the partial ordering of transactions is maintained.  That is to say
 * children come after parents, despite having a potentially larger fee.
 * @param[out] nPackagesSelected    How many packages were selected
 * @param[out] nDescendantsUpdated  Number of descendant transactions updated
 */
void BlockAssembler::addPackageTxs(int &nPackagesSelected,
                                   int &nDescendantsUpdated) {
    std::vector<CBlockTemplateEntry> txPool;
    txPool.reserve(txPool.size());

    std::unordered_map<TxId, size_t, SaltedTxidHasher> txLocations;
    // Fetch all the transactions in dependency order.
    // Complexity: O(n)
    txPool.reserve(mempool->mapTx.size());
    size_t idx = 0;
    for (const auto &mempoolEntry : mempool->mapTx.get<insertion_order>()) {
        const CTransactionRef tx = mempoolEntry.GetSharedTx();
        txPool.emplace_back(mempoolEntry.GetSharedTx(),
                            mempoolEntry.GetFee(), mempoolEntry.GetTxSize(),
                            mempoolEntry.GetSigOpCount());
        txLocations[tx->GetId()] = idx;
        idx += 1;
    }
    

    // Figure out how many parents transactions have.
    // Complexity: O(n + m) where n is transactions, and m is edges.
    //
    // All the dependencies of a transaction appear before the transaction.
    // So we can iterate forward, and simply add all the parents dependency
    // count to our count. Note: If there is a diamond shaped inheritance, we
    // will multi-count. We don't care. We will still maintain an invarent that
    // children come after parents when sorting by the value we calculate here.
    // And in fact, it's good to double count here, because we will need to
    // visit them twice.
    std::unordered_set<TxId, SaltedTxidHasher> seen;
    for (auto &entry : txPool) {
        // Clear out seen list, for the next transaction.
        seen.clear();
        for (const auto &txIn : entry.tx->vin) {
            const TxId &parentId = txIn.prevout.GetTxId();
            // Do not count a direct ancestor twice.
            // We will count further ancestors twice though.
            if (seen.count(parentId) != 0) {
                continue;
            }
            seen.insert(parentId);
            auto it = txLocations.find(parentId);
            if (it == txLocations.end()) {
                // Confirmed parent transaction
                // (We know this because the mempool we have was guaranteed to
                // be consistent)
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
    std::sort(txPool.begin(), txPool.end(),
              [](const CBlockTemplateEntry &a, const CBlockTemplateEntry &b) {
                  // We don't care about truncation of fees.  If the client
                  // doesn't get a one sat of fees counted, oh well. The code
                  // for avoiding division is atrocious.
                  return a.FeeRate() > b.FeeRate();
              });

    // Update the locations of all the transactions so we can find them again
    // later. Complexity: O(n + m) where n is transactions, and m is edges.
    idx = 0;
    for (const auto &blockEntry : txPool) {
        const CTransactionRef tx = blockEntry.tx;
        txLocations[tx->GetId()] = idx;
        idx += 1;
    }

    // Visit all the transactions in package feeRate order, adding them to the
    // block in topological order. Complexity: O(n + m) (and some change for
    // visiting diamond dependencies multiple times)
    //      we charge for this as if they were two seperate transactions.
    //
    // Note: we only need to visit every transaction one time.
    std::unordered_set<TxId, SaltedTxidHasher> txnsInvalid;
    for (const auto &entry : txPool) {
        TxId txId = entry.tx->GetId();
        // Don't reprocess transactions
        if (inBlock.count(txId) != 0 || txnsInvalid.count(txId) != 0) {
            continue;
        }

        // Visit and include all the parents in the block, checking if they are
        // valid.
        std::stack<std::reference_wrapper<const CBlockTemplateEntry>> ancestorStack;
        std::stack<std::reference_wrapper<const CBlockTemplateEntry>> addStack;

        // Check if package size would fit in the block.
        // We won't need to check it for every ancestor below, because their
        // values are included here already
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
        // We do a DFS incase a transaction fails, and we add them to a stack
        // for popping into the block.
        while (!ancestorStack.empty()) {
            const CBlockTemplateEntry &ancestor = ancestorStack.top();
            const TxId &ancestorId = ancestor.tx->GetId();

            ancestorStack.pop();
            // Add it to the stack of items we will add once we have checked all
            // the parents.
            addStack.push(ancestor);

            // Must check that lock times are still valid. This can be removed
            // once MTP is always enforced as long as reorgs keep the mempool
            // consistent.
            //
            // Old code checks the transaction in context.  It doesn't make
            // sense to me though...
            CValidationState state;
            if (!ContextualCheckTransaction(*config, *ancestor.tx.get(), state,
                                            nHeight, nLockTimeCutoff,
                                            nMedianTimePast)) {
                // A parent is invalid, we will need to skip the rest of the
                // transactions So we don't include (now) invalid child. We will
                // visit them later. So we can find all it's children during
                // invalid handling.
                txnsInvalid.insert(ancestorId);
                goto invalid;
            }

            // Push all the ancestors on our DFS stack.
            for (const auto &txIn : ancestor.tx->vin) {
                const TxId &parentId = txIn.prevout.GetTxId();
                size_t hasBeenAdded = inBlock.count(parentId) != 0;
                size_t isInvalid = txnsInvalid.count(parentId) != 0;
                if (isInvalid) {
                    // We must have visited one of the parents before when
                    // handling a different package. It was invalid, and we need
                    // to mark this invalid as well.
                    txnsInvalid.insert(ancestorId);
                    goto invalid;
                }
                if (hasBeenAdded) {
                    // Has already been added to the block. We can skip checking
                    // it.
                    continue;
                }
                // Don't yet mark the transaction as visited.  We may need to
                // visit it twice in this loop to ensure topological ordering
                // when popping off the stack.

                auto it = txLocations.find(parentId);
                if (it == txLocations.end()) {
                    // Confirmed parent transaction
                    // (We know this because the mempool we have was guaranteed
                    // to be consistent)
                    continue;
                }
                const CBlockTemplateEntry &ancestorParent = txPool[it->second];

                // Record the parent so we can visit it later.
                ancestorStack.push(ancestorParent);
            }
        }
        // We know we can add the transaction here.
        // Every item in the package has passed requirements.
        while (!addStack.empty()) {
            const CBlockTemplateEntry &entryToAdd = addStack.top();
            const TxId &addTxID = entryToAdd.tx->GetId();
            size_t hasBeenAdded = inBlock.count(addTxID) != 0;

            addStack.pop();
            // The addStack can have duplicates, so we need to ensure we don't
            // add the same transaction twice in this loop.
            if (hasBeenAdded) {
                continue;
            }
            std::cout << "Adding... " << addTxID.GetHex() << std::endl;
            inBlock.insert(addTxID);
            pblocktemplate->entries.emplace_back(entryToAdd.tx, entryToAdd.txFee,
                                                 entryToAdd.txSize, entryToAdd.txSigOps);
            nBlockSize += entryToAdd.txSize;
            ++nBlockTx;
            nBlockSigOps += entryToAdd.txSigOps;
            nFees += entryToAdd.txFee;
        }
        ++nPackagesSelected;
        continue;
    invalid:
        // Mark all children of the invalid transaction as themselves invalid.
        // Don't add anything in this package to a block.
        while (!addStack.empty()) {
            const CBlockTemplateEntry &child = addStack.top();
            addStack.pop();
            // If we are here, it means we marked one of the parents of an
            // addStack item invalid. because the stack is in topological order,
            // we can be sure we will mark invalid transactions in order going
            // all the way back up to the root transaction.
            for (const auto &txIn : child.tx->vin) {
                const TxId &parentId = txIn.prevout.GetTxId();
                size_t isInvalid = txnsInvalid.count(parentId) != 0;
                if (isInvalid) {
                    txnsInvalid.insert(child.tx->GetId());

                    // We can stop here, we only need to find one invalid parent
                    // to know this is invalid.
                    break;
                }
            }
        }
        continue;
    }
}

void BlockAssembler::addPriorityTxs() {
    // How much of the block should be dedicated to high-priority transactions,
    // included regardless of the fees they pay.
    if (config->GetBlockPriorityPercentage() == 0) {
        return;
    }

    uint64_t nBlockPrioritySize =
        nMaxGeneratedBlockSize * config->GetBlockPriorityPercentage() / 100;

    // This vector will be sorted into a priority queue:
    std::vector<TxCoinAgePriority> vecPriority;
    TxCoinAgePriorityCompare pricomparer;
    std::map<CTxMemPool::txiter, double, CTxMemPool::CompareIteratorByHash>
        waitPriMap;
    typedef std::map<CTxMemPool::txiter, double,
                     CTxMemPool::CompareIteratorByHash>::iterator waitPriIter;
    double actualPriority = -1;

    vecPriority.reserve(mempool->mapTx.size());
    for (CTxMemPool::indexed_transaction_set::iterator mi =
             mempool->mapTx.begin();
         mi != mempool->mapTx.end(); ++mi) {
        double dPriority = mi->GetPriority(nHeight);
        Amount dummy;
        mempool->ApplyDeltas(mi->GetTx().GetId(), dPriority, dummy);
        vecPriority.push_back(TxCoinAgePriority(dPriority, mi));
    }
    std::make_heap(vecPriority.begin(), vecPriority.end(), pricomparer);

    CTxMemPool::txiter iter;

    // Add a tx from priority queue to fill the part of block reserved to
    // priority transactions.
    while (!vecPriority.empty()) {
        iter = vecPriority.front().second;
        actualPriority = vecPriority.front().first;
        std::pop_heap(vecPriority.begin(), vecPriority.end(), pricomparer);
        vecPriority.pop_back();

        // If tx already in block, skip.
        if (inBlock.count(iter->GetTx().GetId())) {
            // Shouldn't happen for priority txs.
            assert(false);
            continue;
        }

        // If tx is dependent on other mempool txs which haven't yet been
        // included then put it in the waitSet.
        if (isStillDependent(iter)) {
            waitPriMap.insert(std::make_pair(iter, actualPriority));
            continue;
        }

        TestForBlockResult testResult = TestForBlock(iter);
        // Break if the block is completed
        if (testResult == TestForBlockResult::BlockFinished) {
            break;
        }

        // If this tx does not fit in the block, skip to next transaction.
        if (testResult != TestForBlockResult::TXFits) {
            continue;
        }

        std::cout << "Why are we here?" << std::endl;
        AddToBlock(iter);

        // If now that this txs is added we've surpassed our desired priority
        // size, then we're done adding priority transactions.
        if (nBlockSize >= nBlockPrioritySize) {
            break;
        }

        // if we have dropped below the AllowFreeThreshold, then we're done
        // adding priority transactions.
        if (!AllowFree(actualPriority)) {
            break;
        }

        // This tx was successfully added, so add transactions that depend
        // on this one to the priority queue to try again.
        for (CTxMemPool::txiter child : mempool->GetMemPoolChildren(iter)) {
            waitPriIter wpiter = waitPriMap.find(child);
            if (wpiter == waitPriMap.end()) {
                continue;
            }

            vecPriority.push_back(TxCoinAgePriority(wpiter->second, child));
            std::push_heap(vecPriority.begin(), vecPriority.end(), pricomparer);
            waitPriMap.erase(wpiter);
        }
    }
}

void IncrementExtraNonce(const Config &config, CBlock *pblock,
                         const CBlockIndex *pindexPrev,
                         unsigned int &nExtraNonce) {
    // Update nExtraNonce
    static uint256 hashPrevBlock;
    if (hashPrevBlock != pblock->hashPrevBlock) {
        nExtraNonce = 0;
        hashPrevBlock = pblock->hashPrevBlock;
    }

    ++nExtraNonce;
    // Height first in coinbase required for block.version=2
    unsigned int nHeight = pindexPrev->nHeight + 1;
    CMutableTransaction txCoinbase(*pblock->vtx[0]);
    txCoinbase.vin[0].scriptSig =
        (CScript() << nHeight << CScriptNum(nExtraNonce)
                   << getExcessiveBlockSizeSig(config)) +
        COINBASE_FLAGS;

    // Make sure the coinbase is big enough.
    uint64_t coinbaseSize =
        ::GetSerializeSize(txCoinbase, SER_NETWORK, PROTOCOL_VERSION);
    if (coinbaseSize < MIN_TX_SIZE) {
        txCoinbase.vin[0].scriptSig
            << std::vector<uint8_t>(MIN_TX_SIZE - coinbaseSize - 1);
    }

    assert(txCoinbase.vin[0].scriptSig.size() <= MAX_COINBASE_SCRIPTSIG_SIZE);
    assert(::GetSerializeSize(txCoinbase, SER_NETWORK, PROTOCOL_VERSION) >=
           MIN_TX_SIZE);

    pblock->vtx[0] = MakeTransactionRef(std::move(txCoinbase));
    pblock->hashMerkleRoot = BlockMerkleRoot(*pblock);
}
