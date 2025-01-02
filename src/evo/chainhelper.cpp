// Copyright (c) 2024 The Dash Core developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <evo/chainhelper.h>

#include <consensus/params.h>
#include <evo/specialtxman.h>
#include <llmq/chainlocks.h>
#include <masternode/payments.h>

CChainstateHelper::CChainstateHelper(CCreditPoolManager& cpoolman, CDeterministicMNManager& dmnman, CMNHFManager& mnhfman,
                                     CGovernanceManager& govman, llmq::CQuorumBlockProcessor& qblockman,
                                     const ChainstateManager& chainman, const Consensus::Params& consensus_params,
                                     const CMasternodeSync& mn_sync, const CSporkManager& sporkman,
                                     const llmq::CChainLocksHandler& clhandler, const llmq::CQuorumManager& qman) :
    clhandler{clhandler},
    mn_payments{std::make_unique<CMNPaymentsProcessor>(dmnman, govman, chainman, consensus_params, mn_sync, sporkman)},
    special_tx{std::make_unique<CSpecialTxProcessor>(cpoolman, dmnman, mnhfman, qblockman, chainman, consensus_params,
                                                     clhandler, qman)}
{}

CChainstateHelper::~CChainstateHelper() = default;

/** Passthrough functions to CChainLocksHandler */
bool CChainstateHelper::HasConflictingChainLock(int nHeight, const uint256& blockHash) const
{
    return clhandler.HasConflictingChainLock(nHeight, blockHash);
}

bool CChainstateHelper::HasChainLock(int nHeight, const uint256& blockHash) const
{
    return clhandler.HasChainLock(nHeight, blockHash);
}

int32_t CChainstateHelper::GetBestChainLockHeight() const { return clhandler.GetBestChainLock().getHeight(); }
