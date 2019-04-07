// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2017 The Bitcoin Core developers
// Copyright (c) 2017-2019 The Bitcoin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "blockworks.h"
#include "config.h"
#include "chainparams.h"
#include "consensus/validation.h"
#include "logging.h"
#include "miner.h"
#include "policy/policy.h"
#include "primitives/block.h"
#include "timedata.h"
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

    // These counters do not include coinbase tx.
    nBlockTx = 0;
    nFees = Amount::zero();

    nHeight = 0;
    nLockTimeCutoff = 0;
    nMedianTimePast = 0;
    nMaxGeneratedBlockSize = DEFAULT_MAX_GENERATED_BLOCK_SIZE;
}

std::unique_ptr<CBlock> Blockworks::GetBlock(const CScript &scriptPubKeyIn) {
    const CChainParams &chainparams = config->GetChainParams();
    int64_t nTimeStart = GetTimeMicros();
    std::unique_ptr<CBlock> pblock(new CBlock());

    // Acquire a read lock to the block data
    boost::shared_lock< boost::shared_mutex > lock(rwlock);

    // Add dummy coinbase tx as first transaction. It is updated at the end.
    pblock->vtx.emplace_back(CTransactionRef());
    
    nHeight = pindexPrev->nHeight + 1;

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
