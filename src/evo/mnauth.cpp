// Copyright (c) 2019 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "mnauth.h"

#include "activemasternode.h"
#include "evo/deterministicmns.h"
#include "masternode-sync.h"
#include "net.h"
#include "net_processing.h"
#include "netmessagemaker.h"
#include "validation.h"

#include <unordered_set>

void CMNAuth::PushMNAUTH(CNode* pnode, CConnman& connman)
{
    if (!fMasternodeMode || activeMasternodeInfo.proTxHash.IsNull()) {
        return;
    }

    uint256 signHash;
    {
        LOCK(pnode->cs_mnauth);
        if (pnode->receivedMNAuthChallenge.IsNull()) {
            return;
        }
        // We include fInbound in signHash to forbid interchanging of challenges by a man in the middle. This way
        // we protect ourself against MITM in this form:
        //   node1 <- Eve -> node2
        // It does not protect against:
        //   node1 -> Eve -> node2
        // This is ok as we only use MNAUTH as a DoS protection and not for sensitive stuff
        signHash = ::SerializeHash(std::make_tuple(*activeMasternodeInfo.blsPubKeyOperator, pnode->receivedMNAuthChallenge, pnode->fInbound));
    }

    CMNAuth mnauth;
    mnauth.proRegTxHash = activeMasternodeInfo.proTxHash;
    mnauth.sig = activeMasternodeInfo.blsKeyOperator->Sign(signHash);

    LogPrint("net", "CMNAuth::%s -- Sending MNAUTH, peer=%d\n", __func__, pnode->id);

    connman.PushMessage(pnode, CNetMsgMaker(pnode->GetSendVersion()).Make(NetMsgType::MNAUTH, mnauth));
}

void CMNAuth::ProcessMessage(CNode* pnode, const std::string& strCommand, CDataStream& vRecv, CConnman& connman)
{
    if (!fMasternodeMode) {
        return;
    }

    if (!masternodeSync.IsBlockchainSynced()) {
        // we can't really verify MNAUTH messages when we don't have the latest MN list
        return;
    }

    if (strCommand == NetMsgType::MNAUTH) {
        CMNAuth mnauth;
        vRecv >> mnauth;

        {
            LOCK(pnode->cs_mnauth);
            // only one MNAUTH allowed
            if (!pnode->verifiedProRegTxHash.IsNull()) {
                LOCK(cs_main);
                Misbehaving(pnode->id, 100);
                return;
            }
        }

        if (mnauth.proRegTxHash.IsNull() || !mnauth.sig.IsValid()) {
            LOCK(cs_main);
            Misbehaving(pnode->id, 100);
            return;
        }

        auto mnList = deterministicMNManager->GetListAtChainTip();
        auto dmn = mnList.GetValidMN(mnauth.proRegTxHash);
        if (!dmn) {
            LOCK(cs_main);
            Misbehaving(pnode->id, 10);
            // in case he was unlucky and not up to date, let him retry the whole verification process
            pnode->fDisconnect = true;
            return;
        }

        uint256 signHash;
        {
            LOCK(pnode->cs_mnauth);
            // See comment in PushMNAUTH (fInbound is negated here as we're on the other side of the connection)
            signHash = ::SerializeHash(std::make_tuple(dmn->pdmnState->pubKeyOperator, pnode->sentMNAuthChallenge, !pnode->fInbound));
        }

        if (!mnauth.sig.VerifyInsecure(dmn->pdmnState->pubKeyOperator, signHash)) {
            LOCK(cs_main);
            Misbehaving(pnode->id, 10);
            // in case he was unlucky and not up to date, let him retry the whole verification process
            pnode->fDisconnect = true;
            return;
        }

        connman.ForEachNode([&](CNode* pnode2) {
            if (pnode2->verifiedProRegTxHash == mnauth.proRegTxHash) {
                LogPrint("net", "CMNAuth::ProcessMessage -- Masternode %s has already verified as peer %d, dropping old connection. peer=%d\n",
                        mnauth.proRegTxHash.ToString(), pnode2->id, pnode->id);
                pnode2->fDisconnect = true;
            }
        });

        {
            LOCK(pnode->cs_mnauth);
            pnode->verifiedProRegTxHash = mnauth.proRegTxHash;
            pnode->verifiedPubKeyHash = dmn->pdmnState->pubKeyOperator.GetHash();
        }

        LogPrint("net", "CMNAuth::%s -- Valid MNAUTH for %s, peer=%d\n", __func__, mnauth.proRegTxHash.ToString(), pnode->id);
    }
}

void CMNAuth::NotifyMasternodeListChanged(const CDeterministicMNList& newList)
{
    std::unordered_set<uint256> pubKeys;
    g_connman->ForEachNode([&](const CNode* pnode) {
        LOCK(pnode->cs_mnauth);
        if (!pnode->verifiedProRegTxHash.IsNull()) {
            pubKeys.emplace(pnode->verifiedPubKeyHash);
        }
    });
    newList.ForEachMN(true, [&](const CDeterministicMNCPtr& dmn) {
        pubKeys.erase(dmn->pdmnState->pubKeyOperator.GetHash());
    });
    g_connman->ForEachNode([&](CNode* pnode) {
        LOCK(pnode->cs_mnauth);
        if (!pnode->verifiedProRegTxHash.IsNull() && pubKeys.count(pnode->verifiedPubKeyHash)) {
            LogPrint("net", "CMNAuth::NotifyMasternodeListChanged -- Disconnecting MN %s due to key changed/removed, peer=%d\n",
                    pnode->verifiedProRegTxHash.ToString(), pnode->id);
            pnode->fDisconnect = true;
        }
    });
}
