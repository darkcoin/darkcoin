// Copyright (c) 2014-2017 The Dash Core developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef MASTERNODEMAN_H
#define MASTERNODEMAN_H

#include "masternode.h"
#include "sync.h"

#include "evo/deterministicmns.h"

class CMasternodeMan;
class CConnman;

extern CMasternodeMan mnodeman;

class CMasternodeMan
{
public:
    typedef std::pair<arith_uint256, const CDeterministicMNCPtr> score_pair_t;
    typedef std::vector<score_pair_t> score_pair_vec_t;
    typedef std::pair<int, const CDeterministicMNCPtr> rank_pair_t;
    typedef std::vector<rank_pair_t> rank_pair_vec_t;

private:
    static const std::string SERIALIZATION_VERSION_STRING;

    static const int DSEG_UPDATE_SECONDS        = 3 * 60 * 60;

    static const int LAST_PAID_SCAN_BLOCKS;

    static const int MIN_POSE_PROTO_VERSION     = 70203;
    static const int MAX_POSE_CONNECTIONS       = 10;
    static const int MAX_POSE_RANK              = 10;
    static const int MAX_POSE_BLOCKS            = 10;

    // critical section to protect the inner data structures
    mutable CCriticalSection cs;

    // Keep track of current block height
    int nCachedBlockHeight;

    // map to hold all MNs
    std::map<COutPoint, CMasternode> mapMasternodes;

    /// Set when masternodes are added, cleared when CGovernanceManager is notified
    bool fMasternodesAdded;

    /// Set when masternodes are removed, cleared when CGovernanceManager is notified
    bool fMasternodesRemoved;

    std::vector<uint256> vecDirtyGovernanceObjectHashes;

    friend class CMasternodeSync;
    /// Find an entry
    CMasternode* Find(const COutPoint& outpoint);

    bool GetMasternodeScores(const uint256& nBlockHash, score_pair_vec_t& vecMasternodeScoresRet);

public:
    // keep track of dsq count to prevent masternodes from gaming privatesend queue
    int64_t nDsqCount;


    ADD_SERIALIZE_METHODS;

    template <typename Stream, typename Operation>
    inline void SerializationOp(Stream& s, Operation ser_action) {
        LOCK(cs);
        std::string strVersion;
        if(ser_action.ForRead()) {
            READWRITE(strVersion);
        }
        else {
            strVersion = SERIALIZATION_VERSION_STRING; 
            READWRITE(strVersion);
        }

        READWRITE(mapMasternodes);
        READWRITE(nDsqCount);

        if(ser_action.ForRead() && (strVersion != SERIALIZATION_VERSION_STRING)) {
            Clear();
        }
    }

    CMasternodeMan();

    bool IsValidForMixingTxes(const COutPoint &outpoint);
    bool AllowMixing(const COutPoint &outpoint);
    bool DisallowMixing(const COutPoint &outpoint);
    int64_t GetLastDsq(const COutPoint &outpoint);

    /// Clear Masternode vector
    void Clear();

    bool GetMasternodeRank(const COutPoint &outpoint, int& nRankRet, uint256& blockHashRet, int nBlockHeight = -1);

    void ProcessMasternodeConnections(CConnman& connman);

    std::string ToString() const;

    void AddDirtyGovernanceObjectHash(const uint256& nHash)
    {
        LOCK(cs);
        vecDirtyGovernanceObjectHashes.push_back(nHash);
    }

    std::vector<uint256> GetAndClearDirtyGovernanceObjectHashes()
    {
        LOCK(cs);
        std::vector<uint256> vecTmp = vecDirtyGovernanceObjectHashes;
        vecDirtyGovernanceObjectHashes.clear();
        return vecTmp;
    }

    bool AddGovernanceVote(const COutPoint& outpoint, uint256 nGovernanceObjectHash);
    void RemoveGovernanceObject(uint256 nGovernanceObjectHash);

    void UpdatedBlockTip(const CBlockIndex *pindex);

    /**
     * Called to notify CGovernanceManager that the masternode index has been updated.
     * Must be called while not holding the CMasternodeMan::cs mutex
     */
    void NotifyMasternodeUpdates(CConnman& connman, bool forceAddedChecks = false, bool forceRemovedChecks = false);

    void DoMaintenance(CConnman &connman);
};

#endif
