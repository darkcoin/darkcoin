// Copyright (c) 2014-2015 The Darkcoin developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef DARKSEND_H
#define DARKSEND_H

#include "core.h"
#include "main.h"
#include "activemasternode.h"
#include "masternodeman.h"

class CTxIn;
class CDarkSendPool;
class CDarkSendSigner;
class CMasterNodeVote;
class CBitcoinAddress;
class CDarksendQueue;
class CDarksendBroadcastTx;
class CActiveMasternode;

#define POOL_MAX_TRANSACTIONS                  3 // wait for X transactions to merge and publish
#define POOL_STATUS_UNKNOWN                    0 // waiting for update
#define POOL_STATUS_IDLE                       1 // waiting for update
#define POOL_STATUS_QUEUE                      2 // waiting in a queue
#define POOL_STATUS_ACCEPTING_ENTRIES          3 // accepting entries
#define POOL_STATUS_FINALIZE_TRANSACTION       4 // master node will broadcast what it accepted
#define POOL_STATUS_SIGNING                    5 // check inputs/outputs, sign final tx
#define POOL_STATUS_TRANSMISSION               6 // transmit transaction
#define POOL_STATUS_ERROR                      7 // error
#define POOL_STATUS_SUCCESS                    8 // success

// status update message constants
#define MASTERNODE_ACCEPTED                    1
#define MASTERNODE_REJECTED                    0
#define MASTERNODE_RESET                       -1

#define DARKSEND_QUEUE_TIMEOUT                 120 // in seconds
#define DARKSEND_SIGNING_TIMEOUT               30 // in seconds

static const int MIN_POOL_PEER_PROTO_VERSION = 70067; // minimum peer version accepted by DarkSendPool

extern CDarkSendPool darkSendPool;
extern CDarkSendSigner darkSendSigner;
extern std::vector<CDarksendQueue> vecDarksendQueue;
extern std::string strMasterNodePrivKey;
extern map<uint256, CDarksendBroadcastTx> mapDarksendBroadcastTxes;
extern CActiveMasternode activeMasternode;

/** Process a DarkSend message using the DarkSend protocol
 * \param pfrom
 * \param strCommand lower case command string; valid values are:
 *        Command  | Description
 *        -------- | -----------------
 *        dsa      | DarkSend Acceptable
 *        dsc      | DarkSend Complete
 *        dsf      | DarkSend Final tx
 *        dsi      | DarkSend vIn
 *        dsq      | DarkSend Queue
 *        dss      | DarkSend Signal Final Tx
 *        dssu     | DarkSend status update
 *        dssub    | DarkSend Subscribe To
 * \param vRecv
 */
void ProcessMessageDarksend(CNode* pfrom, std::string& strCommand, CDataStream& vRecv);

/// Get the DarkSend chain depth for a given input
int GetInputDarksendRounds(CTxIn in, int rounds=0);


/** An input in the DarkSend pool
 */
class CDarkSendEntryVin
{
public:

    // Returns true if Signature is set
    bool isSigSet;

    // Instance of an Inbound Transaction
    CTxIn vin;

    CDarkSendEntryVin()
    {
        isSigSet = false;
        vin = CTxIn();
    }
};

/** A client's transaction in the DarkSend pool
 */
class CDarkSendEntry
{
public:
    bool isSet;
    std::vector<CDarkSendEntryVin> sev;
    int64_t amount;
    CTransaction collateral;
    std::vector<CTxOut> vout;
    CTransaction txSupporting;
    int64_t addedTime; // time in UTC milliseconds

    CDarkSendEntry()
    {
        isSet = false;
        collateral = CTransaction();
        amount = 0;
    }

    /// Add entries to use for DarkSend
    bool Add(const std::vector<CTxIn> vinIn, int64_t amountIn, const CTransaction collateralIn, const std::vector<CTxOut> voutIn)
    {
        if(isSet){return false;}

        BOOST_FOREACH(const CTxIn v, vinIn) {
            CDarkSendEntryVin s = CDarkSendEntryVin();
            s.vin = v;
            sev.push_back(s);
        }
        vout = voutIn;
        amount = amountIn;
        collateral = collateralIn;
        isSet = true;
        addedTime = GetTime();

        return true;
    }

    /// Add Signature
    bool AddSig(const CTxIn& vin)
    {
        BOOST_FOREACH(CDarkSendEntryVin& s, sev) {
            if(s.vin.prevout == vin.prevout && s.vin.nSequence == vin.nSequence){
                if(s.isSigSet){return false;}
                s.vin.scriptSig = vin.scriptSig;
                s.vin.prevPubKey = vin.prevPubKey;
                s.isSigSet = true;

                return true;
            }
        }

        return false;
    }

    /// Is this DarkSend expired?
    bool IsExpired()
    {
        return (GetTime() - addedTime) > DARKSEND_QUEUE_TIMEOUT;// 120 seconds
    }
};

/** A currently in-progress DarkSend merge and denomination information
 */
class CDarksendQueue
{
public:
    CTxIn vin;
    int64_t time;
    int nDenom;
    bool ready; //ready for submit
    std::vector<unsigned char> vchSig;

    CDarksendQueue()
    {
        nDenom = 0;
        vin = CTxIn();
        time = 0;
        vchSig.clear();
        ready = false;
    }

    IMPLEMENT_SERIALIZE
    (
        READWRITE(nDenom);
        READWRITE(vin);
        READWRITE(time);
        READWRITE(ready);
        READWRITE(vchSig);
    )

    /// Check if there is an address
    bool GetAddress(CService &addr)
    {
        CMasternode* pmn = mnodeman.Find(vin);
        if(pmn != NULL)
        {
            addr = pmn->addr;
            return true;
        }
        return false;
    }

    /// Get the protocol version
    bool GetProtocolVersion(int &protocolVersion)
    {
        CMasternode* pmn = mnodeman.Find(vin);
        if(pmn != NULL)
        {
            protocolVersion = pmn->protocolVersion;
            return true;
        }
        return false;
    }

    /** Sign this DarkSend transaction
     *  \return true if all conditions are met:
     *     1) we have an active MasterNode,
     *     2) we have a valid MasterNode private key,
     *     3) we signed the message successfully, and
     *     4) we verified the message successfully
     */
    bool Sign();

    bool Relay();

    /// Is this DarkSend expired?
    bool IsExpired()
    {
        return (GetTime() - time) > DARKSEND_QUEUE_TIMEOUT;// 120 seconds
    }

    /// check if we have a valid MasterNode address
    bool CheckSignature();

};

/** Helper class to store DarkSend transaction (tx) information.
 */
class CDarksendBroadcastTx
{
public:
    CTransaction tx;
    CTxIn vin;
    vector<unsigned char> vchSig;
    int64_t sigTime;
};

/** Helper object for signing and checking signatures
 */
class CDarkSendSigner
{
public:
    bool IsVinAssociatedWithPubkey(CTxIn& vin, CPubKey& pubkey);
    bool SetKey(std::string strSecret, std::string& errorMessage, CKey& key, CPubKey& pubkey);
    bool SignMessage(std::string strMessage, std::string& errorMessage, std::vector<unsigned char>& vchSig, CKey key);
    bool VerifyMessage(CPubKey pubkey, std::vector<unsigned char>& vchSig, std::string strMessage, std::string& errorMessage);
};

/** Empty class - not used (TODO: Delete?)
 */
class CDarksendSession
{

};

/** Used to keep track of current status of DarkSend pool
 */
class CDarkSendPool
{
public:

    // clients entries
    std::vector<CDarkSendEntry> myEntries;
    // masternode entries
    std::vector<CDarkSendEntry> entries;
    // the finalized transaction ready for signing
    CTransaction finalTransaction;

    int64_t lastTimeChanged; /// time in UTC milliseconds
    int64_t lastAutoDenomination; // Note: possibly not used TODO: Delete?

    unsigned int state;
    unsigned int entriesCount;
    unsigned int lastEntryAccepted;
    unsigned int countEntriesAccepted;

    // where collateral should be made out to
    CScript collateralPubKey;

    std::vector<CTxIn> lockedCoins;

    uint256 masterNodeBlockHash;

    std::string lastMessage;
    bool completedTransaction;
    bool unitTest;
    CService submittedToMasternode;

    int sessionID;
    int sessionDenom; //Users must submit an denom matching this
    int sessionUsers; //N Users have said they'll join
    bool sessionFoundMasternode; //If we've found a compatible masternode
    int64_t sessionTotalValue; //used for autoDenom
    std::vector<CTransaction> vecSessionCollateral;

    int cachedLastSuccess;
    int cachedNumBlocks; //used for the overview screen
    int minBlockSpacing; //required blocks between mixes
    CTransaction txCollateral;

    int64_t lastNewBlock;

    //debugging data
    std::string strAutoDenomResult;

    //incremented whenever a DSQ comes through
    int64_t nDsqCount;

    CDarkSendPool()
    {
        /* DarkSend uses collateral addresses to trust parties entering the pool
            to behave themselves. If they don't it takes their money. */

        cachedLastSuccess = 0;
        cachedNumBlocks = 0;
        unitTest = false;
        txCollateral = CTransaction();
        minBlockSpacing = 1;
        nDsqCount = 0;
        lastNewBlock = 0;

        SetNull();
    }

    /// Manage the masternode connections
    void ProcessMasternodeConnections();

    void InitCollateralAddress(){
        std::string strAddress = "";
        if(Params().NetworkID() == CChainParams::MAIN) {
            strAddress = "Xq19GqFvajRrEdDHYRKGYjTsQfpV5jyipF";
        } else {
            strAddress = "y1EZuxhhNMAUofTBEeLqGE1bJrpC2TWRNp";
        }
        SetCollateralAddress(strAddress);
    }

    void SetMinBlockSpacing(int minBlockSpacingIn){
        minBlockSpacing = minBlockSpacingIn;
    }

    bool SetCollateralAddress(std::string strAddress);
    /// Reset the DarkSend pool
    void Reset();
    void SetNull(bool clearEverything=false);

    /// Unlock coins after DarkSend fails or succceeds
    void UnlockCoins();

    bool IsNull() const
    {
        return (state == POOL_STATUS_ACCEPTING_ENTRIES && entries.empty() && myEntries.empty());
    }

    int GetState() const
    {
        return state;
    }

    int GetEntriesCount() const
    {
        if(fMasterNode){
            return entries.size();
        } else {
            return entriesCount;
        }
    }

    int GetLastEntryAccepted() const
    {
        return lastEntryAccepted;
    }

    int GetCountEntriesAccepted() const
    {
        return countEntriesAccepted;
    }

    int GetMyTransactionCount() const
    {
        return myEntries.size();
    }

    void UpdateState(unsigned int newState)
    {
        if (fMasterNode && (newState == POOL_STATUS_ERROR || newState == POOL_STATUS_SUCCESS)){
            LogPrintf("CDarkSendPool::UpdateState() - Can't set state to ERROR or SUCCESS as a masternode. \n");
            return;
        }

        LogPrintf("CDarkSendPool::UpdateState() == %d | %d \n", state, newState);
        if(state != newState){
            lastTimeChanged = GetTimeMillis();
            if(fMasterNode) {
                RelayDarkSendStatus(darkSendPool.sessionID, darkSendPool.GetState(), darkSendPool.GetEntriesCount(), MASTERNODE_RESET);
            }
        }
        state = newState;
    }

    int GetMaxPoolTransactions()
    {
        //if we're on testnet, just use two transactions per merge
        if(Params().NetworkID() == CChainParams::TESTNET || Params().NetworkID() == CChainParams::REGTEST) return 2;

        //use the production amount
        return POOL_MAX_TRANSACTIONS;
    }

    /// Do we have enough users to take entries?
    bool IsSessionReady(){
        return sessionUsers >= GetMaxPoolTransactions();
    }

    /// Are these outputs compatible with other client in the pool?
    bool IsCompatibleWithEntries(std::vector<CTxOut> vout);
    /// Is this amount compatible with other client in the pool?
    bool IsCompatibleWithSession(int64_t nAmount, CTransaction txCollateral, std::string& strReason);

    /// Passively run DarkSend in the background according to the configuration in settings (only for QT)
    bool DoAutomaticDenominating(bool fDryRun=false, bool ready=false);
    bool PrepareDarksendDenominate();


    /// Check the Darksend progress and send client updates if a masternode
    void Check();
    /// Charge fees to bad actors (Charge clients a fee if they're abusive)
    void ChargeFees();
    /// Rarely charge fees to pay miners
    void ChargeRandomFees();
    /// Check for various timeouts (queue objects, DarkSend, etc)
    void CheckTimeout();
    /// Check to make sure a signature matches an input in the pool
    bool SignatureValid(const CScript& newSig, const CTxIn& newVin);
    /// If the collateral is valid given by a client
    bool IsCollateralValid(const CTransaction& txCollateral);
    /// Add a clients entry to the pool
    bool AddEntry(const std::vector<CTxIn>& newInput, const int64_t& nAmount, const CTransaction& txCollateral, const std::vector<CTxOut>& newOutput, std::string& error);
    /// Add signature to a vin
    bool AddScriptSig(const CTxIn& newVin);
    /// Check that all inputs are signed. (Are all inputs signed?)
    bool SignaturesComplete();
    /// As a client, send a transaction to a masternode to start the denomination process
    void SendDarksendDenominate(std::vector<CTxIn>& vin, std::vector<CTxOut>& vout, int64_t amount);
    /// Get masternode updates about the progress of DarkSend
    bool StatusUpdate(int newState, int newEntriesCount, int newAccepted, std::string& error, int newSessionID=0);

    /// As a client, check and sign the final transaction
    bool SignFinalTransaction(CTransaction& finalTransactionNew, CNode* node);

    /// Get the last valid block hash for a given modulus
    bool GetLastValidBlockHash(uint256& hash, int mod=1, int nBlockHeight=0);
    /// Process a new block
    void NewBlock();
    /// DarkSend transaction was completed (failed or successful)
    void CompletedTransaction(bool error, std::string lastMessageNew);
    void ClearLastMessage();
    /// Used for liquidity providers
    bool SendRandomPaymentToSelf();
    /// Split up large inputs or make fee sized inputs
    bool MakeCollateralAmounts();
    bool CreateDenominated(int64_t nTotalValue);
    /// Get the denominations for a list of outputs (returns a bitshifted integer)
    int GetDenominations(const std::vector<CTxOut>& vout);
    void GetDenominationsToString(int nDenom, std::string& strDenom);
    /// Get the denominations for a specific amount of darkcoin.
    int GetDenominationsByAmount(int64_t nAmount, int nDenomTarget=0);

    int GetDenominationsByAmounts(std::vector<int64_t>& vecAmount);
};


void ConnectToDarkSendMasterNodeWinner();

void ThreadCheckDarkSendPool();

#endif
