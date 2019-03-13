// Copyright (c) 2019 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "quorums_chainlocks.h"
#include "quorums_instantsend.h"
#include "quorums_utils.h"

#include "bls/bls_batchverifier.h"
#include "chainparams.h"
#include "coins.h"
#include "txmempool.h"
#include "masternode-sync.h"
#include "net_processing.h"
#include "scheduler.h"
#include "spork.h"
#include "validation.h"

#ifdef ENABLE_WALLET
#include "wallet/wallet.h"
#endif

// needed for nCompleteTXLocks
#include "instantx.h"

#include <boost/algorithm/string/replace.hpp>

namespace llmq
{

static const std::string INPUTLOCK_REQUESTID_PREFIX = "inlock";
static const std::string ISLOCK_REQUESTID_PREFIX = "islock";

CInstantSendManager* quorumInstantSendManager;

uint256 CInstantSendLock::GetRequestId() const
{
    CHashWriter hw(SER_GETHASH, 0);
    hw << ISLOCK_REQUESTID_PREFIX;
    hw << inputs;
    return hw.GetHash();
}

////////////////

void CInstantSendDb::WriteNewInstantSendLock(const uint256& hash, const CInstantSendLock& islock)
{
    CDBBatch batch(db);
    batch.Write(std::make_tuple(std::string("is_i"), hash), islock);
    batch.Write(std::make_tuple(std::string("is_tx"), islock.txid), hash);
    for (auto& in : islock.inputs) {
        batch.Write(std::make_tuple(std::string("is_in"), in), hash);
    }
    db.WriteBatch(batch);

    auto p = std::make_shared<CInstantSendLock>(islock);
    islockCache.insert(hash, p);
    txidCache.insert(islock.txid, hash);
    for (auto& in : islock.inputs) {
        outpointCache.insert(in, hash);
    }
}

void CInstantSendDb::RemoveInstantSendLock(const uint256& hash, CInstantSendLockPtr islock)
{
    if (!islock) {
        islock = GetInstantSendLockByHash(hash);
        if (!islock) {
            return;
        }
    }

    CDBBatch batch(db);
    batch.Erase(std::make_tuple(std::string("is_i"), hash));
    batch.Erase(std::make_tuple(std::string("is_tx"), islock->txid));
    for (auto& in : islock->inputs) {
        batch.Erase(std::make_tuple(std::string("is_in"), in));
    }
    db.WriteBatch(batch);

    islockCache.erase(hash);
    txidCache.erase(islock->txid);
    for (auto& in : islock->inputs) {
        outpointCache.erase(in);
    }
}

CInstantSendLockPtr CInstantSendDb::GetInstantSendLockByHash(const uint256& hash)
{
    CInstantSendLockPtr ret;
    if (islockCache.get(hash, ret)) {
        return ret;
    }

    ret = std::make_shared<CInstantSendLock>();
    bool exists = db.Read(std::make_tuple(std::string("is_i"), hash), *ret);
    if (!exists) {
        ret = nullptr;
    }
    islockCache.insert(hash, ret);
    return ret;
}

CInstantSendLockPtr CInstantSendDb::GetInstantSendLockByTxid(const uint256& txid)
{
    uint256 islockHash;

    bool found = txidCache.get(txid, islockHash);
    if (found && islockHash.IsNull()) {
        return nullptr;
    }

    if (!found) {
        found = db.Read(std::make_tuple(std::string("is_tx"), txid), islockHash);
        txidCache.insert(txid, islockHash);
    }

    if (!found) {
        return nullptr;
    }
    return GetInstantSendLockByHash(islockHash);
}

CInstantSendLockPtr CInstantSendDb::GetInstantSendLockByInput(const COutPoint& outpoint)
{
    uint256 islockHash;
    bool found = outpointCache.get(outpoint, islockHash);
    if (found && islockHash.IsNull()) {
        return nullptr;
    }

    if (!found) {
        found = db.Read(std::make_tuple(std::string("is_in"), outpoint), islockHash);
        outpointCache.insert(outpoint, islockHash);
    }

    if (!found) {
        return nullptr;
    }
    return GetInstantSendLockByHash(islockHash);
}

void CInstantSendDb::WriteLastChainLockBlock(const uint256& hash)
{
    db.Write(std::make_tuple(std::string("is_lcb")), hash);
}

uint256 CInstantSendDb::GetLastChainLockBlock()
{
    uint256 hashBlock;
    db.Read(std::make_tuple(std::string("is_lcb")), hashBlock);
    return hashBlock;
}

////////////////

CInstantSendManager::CInstantSendManager(CScheduler* _scheduler, CDBWrapper& _llmqDb) :
    scheduler(_scheduler),
    db(_llmqDb)
{
}

CInstantSendManager::~CInstantSendManager()
{
}

void CInstantSendManager::RegisterAsRecoveredSigsListener()
{
    quorumSigningManager->RegisterRecoveredSigsListener(this);
}

void CInstantSendManager::UnregisterAsRecoveredSigsListener()
{
    quorumSigningManager->UnregisterRecoveredSigsListener(this);
}

bool CInstantSendManager::ProcessTx(CNode* pfrom, const CTransaction& tx, CConnman& connman, const Consensus::Params& params)
{
    if (!IsNewInstantSendEnabled()) {
        return true;
    }

    auto llmqType = params.llmqForInstantSend;
    if (llmqType == Consensus::LLMQ_NONE) {
        return true;
    }
    if (!fMasternodeMode) {
        return true;
    }

    // Ignore any InstantSend messages until blockchain is synced
    if (!masternodeSync.IsBlockchainSynced()) {
        return true;
    }

    if (IsConflicted(tx)) {
        return false;
    }

    if (!CheckCanLock(tx, true, params)) {
        return false;
    }

    std::vector<uint256> ids;
    ids.reserve(tx.vin.size());

    size_t alreadyVotedCount = 0;
    for (const auto& in : tx.vin) {
        auto id = ::SerializeHash(std::make_pair(INPUTLOCK_REQUESTID_PREFIX, in.prevout));
        ids.emplace_back(id);

        uint256 otherTxHash;
        if (quorumSigningManager->GetVoteForId(llmqType, id, otherTxHash)) {
            if (otherTxHash != tx.GetHash()) {
                LogPrint("instantsend", "CInstantSendManager::%s -- txid=%s: input %s is conflicting with islock %s\n", __func__,
                         tx.GetHash().ToString(), in.prevout.ToStringShort(), otherTxHash.ToString());
                return false;
            }
            alreadyVotedCount++;
        }

        // don't even try the actual signing if any input is conflicting
        if (quorumSigningManager->IsConflicting(llmqType, id, tx.GetHash())) {
            return false;
        }
    }
    if (alreadyVotedCount == ids.size()) {
        return true;
    }

    for (auto& id : ids) {
        inputRequestIds.emplace(id);
        quorumSigningManager->AsyncSignIfMember(llmqType, id, tx.GetHash());
    }

    // We might have received all input locks before we got the corresponding TX. In this case, we have to sign the
    // islock now instead of waiting for the input locks.
    TrySignInstantSendLock(tx);

    return true;
}

bool CInstantSendManager::CheckCanLock(const CTransaction& tx, bool printDebug, const Consensus::Params& params)
{
    if (sporkManager.IsSporkActive(SPORK_16_INSTANTSEND_AUTOLOCKS) && (mempool.UsedMemoryShare() > CInstantSend::AUTO_IX_MEMPOOL_THRESHOLD)) {
        return false;
    }

    int nInstantSendConfirmationsRequired = params.nInstantSendConfirmationsRequired;

    uint256 txHash = tx.GetHash();
    CAmount nValueIn = 0;
    for (const auto& in : tx.vin) {
        CAmount v = 0;
        if (!CheckCanLock(in.prevout, printDebug, &txHash, &v, params)) {
            return false;
        }

        nValueIn += v;
    }

    // TODO decide if we should limit max input values. This was ok to do in the old system, but in the new system
    // where we want to have all TXs locked at some point, this is counterproductive (especially when ChainLocks later
    // depend on all TXs being locked first)
//    CAmount maxValueIn = sporkManager.GetSporkValue(SPORK_5_INSTANTSEND_MAX_VALUE);
//    if (nValueIn > maxValueIn * COIN) {
//        if (printDebug) {
//            LogPrint("instantsend", "CInstantSendManager::%s -- txid=%s: TX input value too high. nValueIn=%f, maxValueIn=%d", __func__,
//                     tx.GetHash().ToString(), nValueIn / (double)COIN, maxValueIn);
//        }
//        return false;
//    }

    return true;
}

bool CInstantSendManager::CheckCanLock(const COutPoint& outpoint, bool printDebug, const uint256* _txHash, CAmount* retValue, const Consensus::Params& params)
{
    int nInstantSendConfirmationsRequired = params.nInstantSendConfirmationsRequired;

    if (IsLocked(outpoint.hash)) {
        // if prevout was ix locked, allow locking of descendants (no matter if prevout is in mempool or already mined)
        return true;
    }

    static uint256 txHashNull;
    const uint256* txHash = &txHashNull;
    if (_txHash) {
        txHash = _txHash;
    }

    auto mempoolTx = mempool.get(outpoint.hash);
    if (mempoolTx) {
        if (printDebug) {
            LogPrint("instantsend", "CInstantSendManager::%s -- txid=%s: parent mempool TX %s is not locked\n", __func__,
                     txHash->ToString(), outpoint.hash.ToString());
        }
        return false;
    }

    Coin coin;
    const CBlockIndex* pindexMined = nullptr;
    {
        LOCK(cs_main);
        if (!pcoinsTip->GetCoin(outpoint, coin) || coin.IsSpent()) {
            if (printDebug) {
                LogPrint("instantsend", "CInstantSendManager::%s -- txid=%s: failed to find UTXO %s\n", __func__,
                         txHash->ToString(), outpoint.ToStringShort());
            }
            return false;
        }
        pindexMined = chainActive[coin.nHeight];
    }

    int nTxAge = chainActive.Height() - coin.nHeight + 1;
    // 1 less than the "send IX" gui requires, in case of a block propagating the network at the time
    int nConfirmationsRequired = nInstantSendConfirmationsRequired - 1;

    if (nTxAge < nConfirmationsRequired) {
        if (!llmq::chainLocksHandler->HasChainLock(pindexMined->nHeight, pindexMined->GetBlockHash())) {
            if (printDebug) {
                LogPrint("instantsend", "CInstantSendManager::%s -- txid=%s: outpoint %s too new and not ChainLocked. nTxAge=%d, nConfirmationsRequired=%d\n", __func__,
                         txHash->ToString(), outpoint.ToStringShort(), nTxAge, nConfirmationsRequired);
            }
            return false;
        }
    }

    if (retValue) {
        *retValue = coin.out.nValue;
    }

    return true;
}

void CInstantSendManager::HandleNewRecoveredSig(const CRecoveredSig& recoveredSig)
{
    if (!IsNewInstantSendEnabled()) {
        return;
    }

    auto llmqType = Params().GetConsensus().llmqForInstantSend;
    if (llmqType == Consensus::LLMQ_NONE) {
        return;
    }
    auto& params = Params().GetConsensus().llmqs.at(llmqType);

    uint256 txid;
    bool isInstantSendLock = false;
    {
        LOCK(cs);
        if (inputRequestIds.count(recoveredSig.id)) {
            txid = recoveredSig.msgHash;
        }
        if (creatingInstantSendLocks.count(recoveredSig.id)) {
            isInstantSendLock = true;
        }
    }
    if (!txid.IsNull()) {
        HandleNewInputLockRecoveredSig(recoveredSig, txid);
    } else if (isInstantSendLock) {
        HandleNewInstantSendLockRecoveredSig(recoveredSig);
    }
}

void CInstantSendManager::HandleNewInputLockRecoveredSig(const CRecoveredSig& recoveredSig, const uint256& txid)
{
    auto llmqType = Params().GetConsensus().llmqForInstantSend;

    CTransactionRef tx;
    uint256 hashBlock;
    if (!GetTransaction(txid, tx, Params().GetConsensus(), hashBlock, true)) {
        return;
    }

    if (LogAcceptCategory("instantsend")) {
        for (auto& in : tx->vin) {
            auto id = ::SerializeHash(std::make_pair(INPUTLOCK_REQUESTID_PREFIX, in.prevout));
            if (id == recoveredSig.id) {
                LogPrint("instantsend", "CInstantSendManager::%s -- txid=%s: got recovered sig for input %s\n", __func__,
                         txid.ToString(), in.prevout.ToStringShort());
                break;
            }
        }
    }

    TrySignInstantSendLock(*tx);
}

void CInstantSendManager::TrySignInstantSendLock(const CTransaction& tx)
{
    auto llmqType = Params().GetConsensus().llmqForInstantSend;

    for (auto& in : tx.vin) {
        auto id = ::SerializeHash(std::make_pair(INPUTLOCK_REQUESTID_PREFIX, in.prevout));
        if (!quorumSigningManager->HasRecoveredSig(llmqType, id, tx.GetHash())) {
            return;
        }
    }

    LogPrint("instantsend", "CInstantSendManager::%s -- txid=%s: got all recovered sigs, creating CInstantSendLock\n", __func__,
            tx.GetHash().ToString());

    CInstantSendLock islock;
    islock.txid = tx.GetHash();
    for (auto& in : tx.vin) {
        islock.inputs.emplace_back(in.prevout);
    }

    auto id = islock.GetRequestId();

    if (quorumSigningManager->HasRecoveredSigForId(llmqType, id)) {
        return;
    }

    {
        LOCK(cs);
        auto e = creatingInstantSendLocks.emplace(id, std::move(islock));
        if (!e.second) {
            return;
        }
        txToCreatingInstantSendLocks.emplace(tx.GetHash(), &e.first->second);
    }

    quorumSigningManager->AsyncSignIfMember(llmqType, id, tx.GetHash());
}

void CInstantSendManager::HandleNewInstantSendLockRecoveredSig(const llmq::CRecoveredSig& recoveredSig)
{
    CInstantSendLock islock;

    {
        LOCK(cs);
        auto it = creatingInstantSendLocks.find(recoveredSig.id);
        if (it == creatingInstantSendLocks.end()) {
            return;
        }

        islock = std::move(it->second);
        creatingInstantSendLocks.erase(it);
        txToCreatingInstantSendLocks.erase(islock.txid);
    }

    if (islock.txid != recoveredSig.msgHash) {
        LogPrint("instantsend", "CInstantSendManager::%s -- txid=%s: islock conflicts with %s, dropping own version", __func__,
                islock.txid.ToString(), recoveredSig.msgHash.ToString());
        return;
    }

    islock.sig = recoveredSig.sig;
    ProcessInstantSendLock(-1, ::SerializeHash(islock), islock);
}

void CInstantSendManager::ProcessMessage(CNode* pfrom, const std::string& strCommand, CDataStream& vRecv, CConnman& connman)
{
    if (!IsNewInstantSendEnabled()) {
        return;
    }

    if (strCommand == NetMsgType::ISLOCK) {
        CInstantSendLock islock;
        vRecv >> islock;
        ProcessMessageInstantSendLock(pfrom, islock, connman);
    }
}

void CInstantSendManager::ProcessMessageInstantSendLock(CNode* pfrom, const llmq::CInstantSendLock& islock, CConnman& connman)
{
    bool ban = false;
    if (!PreVerifyInstantSendLock(pfrom->id, islock, ban)) {
        if (ban) {
            LOCK(cs_main);
            Misbehaving(pfrom->id, 100);
        }
        return;
    }

    auto hash = ::SerializeHash(islock);

    LOCK(cs);
    if (db.GetInstantSendLockByHash(hash) != nullptr) {
        return;
    }
    if (pendingInstantSendLocks.count(hash)) {
        return;
    }

    LogPrint("instantsend", "CInstantSendManager::%s -- txid=%s, islock=%s: received islock, peer=%d\n", __func__,
            islock.txid.ToString(), hash.ToString(), pfrom->id);

    pendingInstantSendLocks.emplace(hash, std::make_pair(pfrom->id, std::move(islock)));

    if (!hasScheduledProcessPending) {
        hasScheduledProcessPending = true;
        scheduler->scheduleFromNow([&] {
            ProcessPendingInstantSendLocks();
        }, 100);
    }
}

bool CInstantSendManager::PreVerifyInstantSendLock(NodeId nodeId, const llmq::CInstantSendLock& islock, bool& retBan)
{
    retBan = false;

    if (islock.txid.IsNull() || !islock.sig.IsValid() || islock.inputs.empty()) {
        retBan = true;
        return false;
    }

    std::set<COutPoint> dups;
    for (auto& o : islock.inputs) {
        if (!dups.emplace(o).second) {
            retBan = true;
            return false;
        }
    }

    return true;
}

void CInstantSendManager::ProcessPendingInstantSendLocks()
{
    auto llmqType = Params().GetConsensus().llmqForInstantSend;

    decltype(pendingInstantSendLocks) pend;

    {
        LOCK(cs);
        hasScheduledProcessPending = false;
        pend = std::move(pendingInstantSendLocks);
    }

    if (!IsNewInstantSendEnabled()) {
        return;
    }

    int tipHeight;
    {
        LOCK(cs_main);
        tipHeight = chainActive.Height();
    }

    CBLSBatchVerifier<NodeId, uint256> batchVerifier(false, true, 8);
    std::unordered_map<uint256, std::pair<CQuorumCPtr, CRecoveredSig>> recSigs;

    for (const auto& p : pend) {
        auto& hash = p.first;
        auto nodeId = p.second.first;
        auto& islock = p.second.second;

        auto id = islock.GetRequestId();

        // no need to verify an ISLOCK if we already have verified the recovered sig that belongs to it
        if (quorumSigningManager->HasRecoveredSig(llmqType, id, islock.txid)) {
            continue;
        }

        auto quorum = quorumSigningManager->SelectQuorumForSigning(llmqType, tipHeight, id);
        if (!quorum) {
            // should not happen, but if one fails to select, all others will also fail to select
            return;
        }
        uint256 signHash = CLLMQUtils::BuildSignHash(llmqType, quorum->quorumHash, id, islock.txid);
        batchVerifier.PushMessage(nodeId, hash, signHash, islock.sig, quorum->quorumPublicKey);

        // We can reconstruct the CRecoveredSig objects from the islock and pass it to the signing manager, which
        // avoids unnecessary double-verification of the signature. We however only do this when verification here
        // turns out to be good (which is checked further down)
        if (!quorumSigningManager->HasRecoveredSigForId(llmqType, id)) {
            CRecoveredSig recSig;
            recSig.llmqType = llmqType;
            recSig.quorumHash = quorum->quorumHash;
            recSig.id = id;
            recSig.msgHash = islock.txid;
            recSig.sig = islock.sig;
            recSigs.emplace(std::piecewise_construct,
                    std::forward_as_tuple(hash),
                    std::forward_as_tuple(std::move(quorum), std::move(recSig)));
        }
    }

    batchVerifier.Verify();

    if (!batchVerifier.badSources.empty()) {
        LOCK(cs_main);
        for (auto& nodeId : batchVerifier.badSources) {
            // Let's not be too harsh, as the peer might simply be unlucky and might have sent us an old lock which
            // does not validate anymore due to changed quorums
            Misbehaving(nodeId, 20);
        }
    }
    for (const auto& p : pend) {
        auto& hash = p.first;
        auto nodeId = p.second.first;
        auto& islock = p.second.second;

        if (batchVerifier.badMessages.count(hash)) {
            LogPrint("instantsend", "CInstantSendManager::%s -- txid=%s, islock=%s: invalid sig in islock, peer=%d\n", __func__,
                     islock.txid.ToString(), hash.ToString(), nodeId);
            continue;
        }

        ProcessInstantSendLock(nodeId, hash, islock);

        // See comment further on top. We pass a reconstructed recovered sig to the signing manager to avoid
        // double-verification of the sig.
        auto it = recSigs.find(hash);
        if (it != recSigs.end()) {
            auto& quorum = it->second.first;
            auto& recSig = it->second.second;
            if (!quorumSigningManager->HasRecoveredSigForId(llmqType, recSig.id)) {
                recSig.UpdateHash();
                LogPrint("instantsend", "CInstantSendManager::%s -- txid=%s, islock=%s: passing reconstructed recSig to signing mgr, peer=%d\n", __func__,
                         islock.txid.ToString(), hash.ToString(), nodeId);
                quorumSigningManager->PushReconstructedRecoveredSig(recSig, quorum);
            }
        }
    }
}

void CInstantSendManager::ProcessInstantSendLock(NodeId from, const uint256& hash, const CInstantSendLock& islock)
{
    {
        LOCK(cs_main);
        g_connman->RemoveAskFor(hash);
    }

    CTransactionRef tx;
    uint256 hashBlock;
    // we ignore failure here as we must be able to propagate the lock even if we don't have the TX locally
    if (GetTransaction(islock.txid, tx, Params().GetConsensus(), hashBlock)) {
        if (!hashBlock.IsNull()) {
            const CBlockIndex* pindexMined;
            {
                LOCK(cs_main);
                pindexMined = mapBlockIndex.at(hashBlock);
            }

            // Let's see if the TX that was locked by this islock is already mined in a ChainLocked block. If yes,
            // we can simply ignore the islock, as the ChainLock implies locking of all TXs in that chain
            if (llmq::chainLocksHandler->HasChainLock(pindexMined->nHeight, pindexMined->GetBlockHash())) {
                LogPrint("instantsend", "CInstantSendManager::%s -- txlock=%s, islock=%s: dropping islock as it already got a ChainLock in block %s, peer=%d\n", __func__,
                         islock.txid.ToString(), hash.ToString(), hashBlock.ToString(), from);
                return;
            }
        }
    }

    {
        LOCK(cs);

        LogPrint("instantsend", "CInstantSendManager::%s -- txid=%s, islock=%s: processsing islock, peer=%d\n", __func__,
                 islock.txid.ToString(), hash.ToString(), from);

        creatingInstantSendLocks.erase(islock.GetRequestId());
        txToCreatingInstantSendLocks.erase(islock.txid);

        CInstantSendLockPtr otherIsLock;
        if (db.GetInstantSendLockByHash(hash)) {
            return;
        }
        otherIsLock = db.GetInstantSendLockByTxid(islock.txid);
        if (otherIsLock != nullptr) {
            LogPrint("instantsend", "CInstantSendManager::%s -- txid=%s, islock=%s: duplicate islock, other islock=%s, peer=%d\n", __func__,
                     islock.txid.ToString(), hash.ToString(), ::SerializeHash(*otherIsLock).ToString(), from);
        }
        for (auto& in : islock.inputs) {
            otherIsLock = db.GetInstantSendLockByInput(in);
            if (otherIsLock != nullptr) {
                LogPrint("instantsend", "CInstantSendManager::%s -- txid=%s, islock=%s: conflicting input in islock. input=%s, other islock=%s, peer=%d\n", __func__,
                         islock.txid.ToString(), hash.ToString(), in.ToStringShort(), ::SerializeHash(*otherIsLock).ToString(), from);
            }
        }

        db.WriteNewInstantSendLock(hash, islock);
    }

    CInv inv(MSG_ISLOCK, hash);
    g_connman->RelayInv(inv);

    RemoveMempoolConflictsForLock(hash, islock);
    RetryLockMempoolTxs(islock.txid);

    UpdateWalletTransaction(islock.txid, tx);
}

void CInstantSendManager::UpdateWalletTransaction(const uint256& txid, const CTransactionRef& tx)
{
#ifdef ENABLE_WALLET
    if (!pwalletMain) {
        return;
    }

    if (pwalletMain->UpdatedTransaction(txid)) {
        // bumping this to update UI
        nCompleteTXLocks++;
        // notify an external script once threshold is reached
        std::string strCmd = GetArg("-instantsendnotify", "");
        if (!strCmd.empty()) {
            boost::replace_all(strCmd, "%s", txid.GetHex());
            boost::thread t(runCommand, strCmd); // thread runs free
        }
    }
#endif

    if (tx) {
        GetMainSignals().NotifyTransactionLock(*tx);
    }
}

void CInstantSendManager::SyncTransaction(const CTransaction& tx, const CBlockIndex* pindex, int posInBlock)
{
    if (!IsNewInstantSendEnabled()) {
        return;
    }

    if (IsLocked(tx.GetHash())) {
        RetryLockMempoolTxs(tx.GetHash());
    }
}

void CInstantSendManager::NotifyChainLock(const CBlockIndex* pindex)
{
    uint256 lastChainLockBlock;
    {
        LOCK(cs);
        db.GetLastChainLockBlock();
    }

    // Let's find all islocks that correspond to TXs which are part of the freshly ChainLocked chain and then delete
    // the islocks. We do this because the ChainLocks imply locking and thus it's not needed to further track
    // or propagate the islocks
    std::unordered_set<uint256> toDelete;
    while (pindex && pindex->GetBlockHash() != lastChainLockBlock) {
        CBlock block;
        {
            LOCK(cs_main);
            if (!ReadBlockFromDisk(block, pindex, Params().GetConsensus())) {
                pindex = pindex->pprev;
                continue;
            }
        }

        LOCK(cs);
        for (const auto& tx : block.vtx) {
            auto islock = db.GetInstantSendLockByTxid(tx->GetHash());
            if (!islock) {
                continue;
            }
            auto hash = ::SerializeHash(*islock);
            LogPrint("instantsend", "CInstantSendManager::%s -- txid=%s, islock=%s: removing islock as it got ChainLocked in block %s\n", __func__,
                     islock->txid.ToString(), hash.ToString(), pindex->GetBlockHash().ToString());
            RemoveFinalISLock(hash, islock);
        }

        pindex = pindex->pprev;
    }

    {
        LOCK(cs);
        db.WriteLastChainLockBlock(pindex ? pindex->GetBlockHash() : uint256());
    }

    RetryLockMempoolTxs(uint256());
}

void CInstantSendManager::RemoveFinalISLock(const uint256& hash, const CInstantSendLockPtr& islock)
{
    AssertLockHeld(cs);

    db.RemoveInstantSendLock(hash, islock);
    
    for (auto& in : islock->inputs) {
        auto inputRequestId = ::SerializeHash(std::make_pair(INPUTLOCK_REQUESTID_PREFIX, in));
        inputRequestIds.erase(inputRequestId);
    }
}

void CInstantSendManager::RemoveMempoolConflictsForLock(const uint256& hash, const CInstantSendLock& islock)
{
    LOCK(mempool.cs);

    std::unordered_map<uint256, CTransactionRef> toDelete;

    for (auto& in : islock.inputs) {
        auto it = mempool.mapNextTx.find(in);
        if (it == mempool.mapNextTx.end()) {
            continue;
        }
        if (it->second->GetHash() != islock.txid) {
            toDelete.emplace(it->second->GetHash(), mempool.get(it->second->GetHash()));

            LogPrint("instantsend", "CInstantSendManager::%s -- txid=%s, islock=%s: mempool TX %s with input %s conflicts with islock\n", __func__,
                     islock.txid.ToString(), hash.ToString(), it->second->GetHash().ToString(), in.ToStringShort());
        }
    }

    for (auto& p : toDelete) {
        mempool.removeRecursive(*p.second, MemPoolRemovalReason::CONFLICT);
    }
}

void CInstantSendManager::RetryLockMempoolTxs(const uint256& lockedParentTx)
{
    // Let's retry all mempool TXs which don't have an islock yet and where the parents got ChainLocked now

    std::unordered_map<uint256, CTransactionRef> txs;

    {
        LOCK(mempool.cs);

        if (lockedParentTx.IsNull()) {
            txs.reserve(mempool.mapTx.size());
            for (auto it = mempool.mapTx.begin(); it != mempool.mapTx.end(); ++it) {
                txs.emplace(it->GetTx().GetHash(), it->GetSharedTx());
            }
        } else {
            auto it = mempool.mapNextTx.lower_bound(COutPoint(lockedParentTx, 0));
            while (it != mempool.mapNextTx.end() && it->first->hash == lockedParentTx) {
                txs.emplace(it->second->GetHash(), mempool.get(it->second->GetHash()));
                ++it;
            }
        }
    }
    for (auto& p : txs) {
        auto& tx = p.second;
        {
            LOCK(cs);
            if (txToCreatingInstantSendLocks.count(tx->GetHash())) {
                // we're already in the middle of locking this one
                continue;
            }
            if (IsLocked(tx->GetHash())) {
                continue;
            }
            if (IsConflicted(*tx)) {
                // should not really happen as we have already filtered these out
                continue;
            }
        }

        // CheckCanLock is already called by ProcessTx, so we should avoid calling it twice. But we also shouldn't spam
        // the logs when retrying TXs that are not ready yet.
        if (LogAcceptCategory("instantsend")) {
            if (!CheckCanLock(*tx, false, Params().GetConsensus())) {
                continue;
            }
            LogPrint("instantsend", "CInstantSendManager::%s -- txid=%s: retrying to lock\n", __func__,
                     tx->GetHash().ToString());
        }

        ProcessTx(nullptr, *tx, *g_connman, Params().GetConsensus());
    }
}

bool CInstantSendManager::AlreadyHave(const CInv& inv)
{
    if (!IsNewInstantSendEnabled()) {
        return true;
    }

    LOCK(cs);
    return db.GetInstantSendLockByHash(inv.hash) != nullptr || pendingInstantSendLocks.count(inv.hash) != 0;
}

bool CInstantSendManager::GetInstantSendLockByHash(const uint256& hash, llmq::CInstantSendLock& ret)
{
    if (!IsNewInstantSendEnabled()) {
        return false;
    }

    LOCK(cs);
    auto islock = db.GetInstantSendLockByHash(hash);
    if (!islock) {
        return false;
    }
    ret = *islock;
    return true;
}

bool CInstantSendManager::IsLocked(const uint256& txHash)
{
    if (!IsNewInstantSendEnabled()) {
        return false;
    }

    LOCK(cs);
    return db.GetInstantSendLockByTxid(txHash) != nullptr;
}

bool CInstantSendManager::IsConflicted(const CTransaction& tx)
{
    LOCK(cs);
    uint256 dummy;
    return GetConflictingTx(tx, dummy);
}

bool CInstantSendManager::GetConflictingTx(const CTransaction& tx, uint256& retConflictTxHash)
{
    if (!IsNewInstantSendEnabled()) {
        return false;
    }

    LOCK(cs);
    for (const auto& in : tx.vin) {
        auto otherIsLock = db.GetInstantSendLockByInput(in.prevout);
        if (!otherIsLock) {
            continue;
        }

        if (otherIsLock->txid != tx.GetHash()) {
            retConflictTxHash = otherIsLock->txid;
            return true;
        }
    }
    return false;
}

bool IsOldInstantSendEnabled()
{
    return sporkManager.IsSporkActive(SPORK_2_INSTANTSEND_ENABLED) && !sporkManager.IsSporkActive(SPORK_20_INSTANTSEND_LLMQ_BASED);
}

bool IsNewInstantSendEnabled()
{
    return sporkManager.IsSporkActive(SPORK_2_INSTANTSEND_ENABLED) && sporkManager.IsSporkActive(SPORK_20_INSTANTSEND_LLMQ_BASED);
}

bool IsInstantSendEnabled()
{
    return sporkManager.IsSporkActive(SPORK_2_INSTANTSEND_ENABLED);
}

}
