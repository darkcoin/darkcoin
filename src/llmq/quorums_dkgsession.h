// Copyright (c) 2018 The Dash Core developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef DASH_QUORUMS_DKGSESSION_H
#define DASH_QUORUMS_DKGSESSION_H

#include "consensus/params.h"
#include "net.h"
#include "batchedlogger.h"

#include "bls/bls_ies.h"
#include "bls/bls_worker.h"

#include "evo/deterministicmns.h"
#include "evo/evodb.h"

#include "llmq/quorums_utils.h"

class UniValue;

namespace llmq
{

class CFinalCommitment;
class CDKGSession;
class CDKGSessionManager;

class CDKGLogger : public CBatchedLogger
{
public:
    CDKGLogger(CDKGSession& _quorumDkg, const std::string& _func);
    CDKGLogger(Consensus::LLMQType _llmqType, const uint256& _quorumHash, int _height, bool _areWeMember, const std::string& _func);
};

class CDKGContribution
{
public:
    uint8_t llmqType;
    uint256 quorumHash;
    uint256 proTxHash;
    BLSVerificationVectorPtr vvec;
    std::shared_ptr<CBLSIESMultiRecipientObjects<CBLSSecretKey>> contributions;
    CBLSSignature sig;

public:
    template<typename Stream>
    inline void SerializeWithoutSig(Stream& s) const
    {
        s << llmqType;
        s << quorumHash;
        s << proTxHash;
        s << *vvec;
        s << *contributions;
    }
    template<typename Stream>
    inline void Serialize(Stream& s) const
    {
        SerializeWithoutSig(s);
        s << sig;
    }
    template<typename Stream>
    inline void Unserialize(Stream& s)
    {
        BLSVerificationVector tmp1;
        CBLSIESMultiRecipientObjects<CBLSSecretKey> tmp2;

        s >> llmqType;
        s >> quorumHash;
        s >> proTxHash;
        s >> tmp1;
        s >> tmp2;
        s >> sig;

        vvec = std::make_shared<BLSVerificationVector>(std::move(tmp1));
        contributions = std::make_shared<CBLSIESMultiRecipientObjects<CBLSSecretKey>>(std::move(tmp2));
    }

    uint256 GetSignHash() const
    {
        CHashWriter hw(SER_GETHASH, 0);
        SerializeWithoutSig(hw);
        hw << CBLSSignature();
        return hw.GetHash();
    }
};

class CDKGComplaint
{
public:
    uint8_t llmqType;
    uint256 quorumHash;
    uint256 proTxHash;
    std::vector<bool> complainForMembers;
    CBLSSignature sig;

public:
    CDKGComplaint() {}
    CDKGComplaint(const Consensus::LLMQParams& params);

    ADD_SERIALIZE_METHODS

    template<typename Stream, typename Operation>
    inline void SerializationOp(Stream& s, Operation ser_action)
    {
        READWRITE(llmqType);
        READWRITE(quorumHash);
        READWRITE(proTxHash);
        READWRITE(DYNBITSET(complainForMembers));
        READWRITE(sig);
    }

    uint256 GetSignHash() const
    {
        CDKGComplaint tmp(*this);
        tmp.sig = CBLSSignature();
        return ::SerializeHash(tmp);
    }
};

class CDKGJustification
{
public:
    uint8_t llmqType;
    uint256 quorumHash;
    uint256 proTxHash;
    std::vector<std::pair<uint32_t, CBLSSecretKey>> contributions;
    CBLSSignature sig;

public:
    ADD_SERIALIZE_METHODS

    template<typename Stream, typename Operation>
    inline void SerializationOp(Stream& s, Operation ser_action)
    {
        READWRITE(llmqType);
        READWRITE(quorumHash);
        READWRITE(proTxHash);
        READWRITE(contributions);
        READWRITE(sig);
    }

    uint256 GetSignHash() const
    {
        CDKGJustification tmp(*this);
        tmp.sig = CBLSSignature();
        return ::SerializeHash(tmp);
    }
};

// each member commits to a single set of valid members with this message
// then each node aggregate all received premature commitments
// into a single CFinalCommitment, which is only valid if
// enough (>=minSize) premature commitments were aggregated
class CDKGPrematureCommitment
{
public:
    uint8_t llmqType;
    uint256 quorumHash;
    uint256 proTxHash;
    std::vector<bool> validMembers;

    CBLSPublicKey quorumPublicKey;
    uint256 quorumVvecHash;

    CBLSSignature quorumSig; // threshold sig share of quorumHash+validMembers+pubKeyHash+vvecHash
    CBLSSignature sig; // single member sig of quorumHash+validMembers+pubKeyHash+vvecHash

public:
    CDKGPrematureCommitment() {}
    CDKGPrematureCommitment(const Consensus::LLMQParams& params);

    int CountValidMembers() const
    {
        return (int)std::count(validMembers.begin(), validMembers.end(), true);
    }

public:
    ADD_SERIALIZE_METHODS

    template<typename Stream, typename Operation>
    inline void SerializationOp(Stream& s, Operation ser_action)
    {
        READWRITE(llmqType);
        READWRITE(quorumHash);
        READWRITE(proTxHash);
        READWRITE(DYNBITSET(validMembers));
        READWRITE(quorumPublicKey);
        READWRITE(quorumVvecHash);
        READWRITE(quorumSig);
        READWRITE(sig);
    }

    uint256 GetSignHash() const
    {
        return CLLMQUtils::BuildCommitmentHash(llmqType, quorumHash, validMembers, quorumPublicKey, quorumVvecHash);
    }
};

class CDKGMember
{
public:
    CDKGMember(CDeterministicMNCPtr _dmn, size_t _idx);

    CDeterministicMNCPtr dmn;
    size_t idx;
    CBLSId id;

    std::set<uint256> contributions;
    std::set<uint256> complaints;
    std::set<uint256> justifications;
    std::set<uint256> prematureCommitments;

    std::set<uint256> complaintsFromOthers;

    bool bad{false};
    bool weComplain{false};
    bool someoneComplain{false};
};

class CDKGSession
{
    friend class CDKGSessionHandler;
    friend class CDKGSessionManager;
    friend class CDKGLogger;
    template<typename Message> friend class CDKGMessageHandler;

private:
    const Consensus::LLMQParams& params;

    CEvoDB& evoDb;
    CBLSWorker& blsWorker;
    CBLSWorkerCache cache;
    CDKGSessionManager& dkgManager;

    uint256 quorumHash;
    int height{-1};

private:
    std::vector<std::unique_ptr<CDKGMember>> members;
    std::map<uint256, size_t> membersMap;
    BLSVerificationVectorPtr vvecContribution;
    BLSSecretKeyVector skContributions;

    BLSIdVector memberIds;
    std::vector<BLSVerificationVectorPtr> receivedVvecs;
    // these are not necessarily verified yet. Only trust in what was written to the DB
    BLSSecretKeyVector receivedSkContributions;

    uint256 myProTxHash;
    CBLSId myId;
    size_t myIdx{(size_t)-1};

    // all indexed by msg hash
    // we expect to only receive a single vvec and contribution per member, but we must also be able to relay
    // conflicting messages as otherwise an attacker might be able to broadcast conflicting (valid+invalid) messages
    // and thus split the quorum. Such members are later removed from the quorum.
    mutable CCriticalSection invCs;
    std::map<uint256, CDKGContribution> contributions;
    std::map<uint256, CDKGComplaint> complaints;
    std::map<uint256, CDKGJustification> justifications;
    std::map<uint256, CDKGPrematureCommitment> prematureCommitments;
    std::set<CInv> invSet;
    std::set<CService> participatingNodes;

    std::set<uint256> seenMessages;

    std::vector<size_t> pendingContributionVerifications;

    // filled by ReceivePrematureCommitment and used by FinalizeCommitments
    std::set<uint256> validCommitments;

public:
    CDKGSession(const Consensus::LLMQParams& _params, CEvoDB& _evoDb, CBLSWorker& _blsWorker, CDKGSessionManager& _dkgManager) :
        params(_params), evoDb(_evoDb), blsWorker(_blsWorker), cache(_blsWorker), dkgManager(_dkgManager) {}

    bool Init(int _height, const uint256& _quorumHash, const std::vector<CDeterministicMNCPtr>& mns, const uint256& _myProTxHash);

    // Phase 1: contribution
    void Contribute();
    void SendContributions();
    bool PreVerifyMessage(const uint256& hash, const CDKGContribution& qc, bool& retBan);
    void ReceiveMessage(const uint256& hash, const CDKGContribution& qc, bool& retBan);
    void VerifyPendingContributions();

    // Phase 2: complaint
    void VerifyAndComplain();
    void SendComplaint();
    bool PreVerifyMessage(const uint256& hash, const CDKGComplaint& qc, bool& retBan);
    void ReceiveMessage(const uint256& hash, const CDKGComplaint& qc, bool& retBan);

    // Phase 3: justification
    void VerifyAndJustify();
    void SendJustification(const std::set<uint256>& forMembers);
    bool PreVerifyMessage(const uint256& hash, const CDKGJustification& qj, bool& retBan);
    void ReceiveMessage(const uint256& hash, const CDKGJustification& qj, bool& retBan);

    // Phase 4: commit
    void VerifyAndCommit();
    void SendCommitment();
    bool PreVerifyMessage(const uint256& hash, const CDKGPrematureCommitment& qc, bool& retBan);
    void ReceiveMessage(const uint256& hash, const CDKGPrematureCommitment& qc, bool& retBan);

    // Phase 5: aggregate/finalize
    std::vector<CFinalCommitment> FinalizeCommitments();

    bool AreWeMember() const { return !myProTxHash.IsNull(); }
    void MarkBadMember(size_t idx);

    bool Seen(const uint256& msgHash);
    void AddParticipatingNode(NodeId nodeId);
    void RelayInvToParticipants(const CInv& inv);

public:
    CDKGMember* GetMember(const uint256& proTxHash);
};

}

#endif //DASH_QUORUMS_DKGSESSION_H
