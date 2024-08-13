// Copyright (c) 2013-2020 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <consensus/tx_check.h>
#include <consensus/validation.h>
#include <hash.h>
#include <script/interpreter.h>
#include <script/script.h>
#include <serialize.h>
#include <streams.h>
#include <test/data/sighash.json.h>
#include <test/data/sighash_dip0143.json.h>
#include <test/util/setup_common.h>
#include <util/strencodings.h>
#include <version.h>

#include <boost/test/unit_test.hpp>

#include <univalue.h>

UniValue read_json(const std::string& jsondata);

// Old script.cpp SignatureHash function
uint256 static SignatureHashOld(CScript scriptCode, const CTransaction& txTo, unsigned int nIn, int nHashType)
{
    if (nIn >= txTo.vin.size())
    {
        return uint256::ONE;
    }
    CMutableTransaction txTmp(txTo);

    // In case concatenating two scripts ends up with two codeseparators,
    // or an extra one at the end, this prevents all those possible incompatibilities.
    FindAndDelete(scriptCode, CScript(OP_CODESEPARATOR));

    // Blank out other inputs' signatures
    for (unsigned int i = 0; i < txTmp.vin.size(); i++)
        txTmp.vin[i].scriptSig = CScript();
    txTmp.vin[nIn].scriptSig = scriptCode;

    // Blank out some of the outputs
    if ((nHashType & 0x1f) == SIGHASH_NONE)
    {
        // Wildcard payee
        txTmp.vout.clear();

        // Let the others update at will
        for (unsigned int i = 0; i < txTmp.vin.size(); i++)
            if (i != nIn)
                txTmp.vin[i].nSequence = 0;
    }
    else if ((nHashType & 0x1f) == SIGHASH_SINGLE)
    {
        // Only lock-in the txout payee at same index as txin
        unsigned int nOut = nIn;
        if (nOut >= txTmp.vout.size())
        {
            return uint256::ONE;
        }
        txTmp.vout.resize(nOut+1);
        for (unsigned int i = 0; i < nOut; i++)
            txTmp.vout[i].SetNull();

        // Let the others update at will
        for (unsigned int i = 0; i < txTmp.vin.size(); i++)
            if (i != nIn)
                txTmp.vin[i].nSequence = 0;
    }

    // Blank out other inputs completely, not recommended for open transactions
    if (nHashType & SIGHASH_ANYONECANPAY)
    {
        txTmp.vin[0] = txTmp.vin[nIn];
        txTmp.vin.resize(1);
    }

    // Serialize and hash
    CHashWriter ss(SER_GETHASH, 0);
    ss << txTmp << nHashType;
    return ss.GetHash();
}

void static RandomScript(CScript &script) {
    static const opcodetype oplist[] = {OP_FALSE, OP_1, OP_2, OP_3, OP_CHECKSIG, OP_IF, OP_VERIF, OP_RETURN, OP_CODESEPARATOR};
    script = CScript();
    int ops = (InsecureRandRange(10));
    for (int i=0; i<ops; i++)
        script << oplist[InsecureRandRange(std::size(oplist))];
}

void static RandomTransaction(CMutableTransaction &tx, bool fSingle) {
    tx.nVersion = InsecureRandRange(2) + 1;
    tx.vin.clear();
    tx.vout.clear();
    tx.vExtraPayload.clear();
    tx.nLockTime = (InsecureRandBool()) ? InsecureRand32() : 0;
    int ins = (InsecureRandBits(2)) + 1;
    int outs = fSingle ? ins : (InsecureRandBits(2)) + 1;
    int vExtraPayloadSize = InsecureRandRange(100);
    for (int i = 0; i < vExtraPayloadSize; i++) {
        tx.vExtraPayload.push_back(InsecureRandBits(8));
    }
    for (int in = 0; in < ins; in++) {
        tx.vin.push_back(CTxIn());
        CTxIn &txin = tx.vin.back();
        txin.prevout.hash = InsecureRand256();
        txin.prevout.n = InsecureRandBits(2);
        RandomScript(txin.scriptSig);
        txin.nSequence = (InsecureRandBool()) ? InsecureRand32() : std::numeric_limits<uint32_t>::max();
    }
    for (int out = 0; out < outs; out++) {
        tx.vout.push_back(CTxOut());
        CTxOut &txout = tx.vout.back();
        txout.nValue = InsecureRandRange(100000000);
        RandomScript(txout.scriptPubKey);
    }
}

static void TestSigHash(SigVersion sigVersion)
{
    const bool isDip0143 = sigVersion == SigVersion::DIP0143;
    #if defined(PRINT_SIGHASH_JSON)
    std::cout << "[\n";
    if (isDip0143) {
        std::cout << "\t[\"raw_transaction, script, input_index, amount, hashType, signature_hash (result)\"],\n";
    } else {
        std::cout << "\t[\"raw_transaction, script, input_index, hashType, signature_hash (result)\"],\n";
    }
    int nRandomTests = 500;
    #else
    int nRandomTests = 50000;
    #endif
    for (int i=0; i<nRandomTests; i++) {
        int nHashType = InsecureRand32();
        CMutableTransaction txTo;
        RandomTransaction(txTo, (nHashType & 0x1f) == SIGHASH_SINGLE);
        CScript scriptCode;
        RandomScript(scriptCode);
        int nIn = InsecureRandRange(txTo.vin.size());
        CAmount amount = InsecureRandRange(MAX_MONEY);

        uint256 sh, sho;
        sho = SignatureHashOld(scriptCode, CTransaction(txTo), nIn, nHashType);
        sh = SignatureHash(scriptCode, txTo, nIn, nHashType, amount, sigVersion);
        #if defined(PRINT_SIGHASH_JSON)
        CDataStream ss(SER_NETWORK, PROTOCOL_VERSION);
        ss << txTo;

        std::cout << "\t[\"" ;
        std::cout << HexStr(ss) << "\", \"";
        std::cout << HexStr(scriptCode) << "\", ";
        std::cout << nIn << ", ";
        if(isDip0143) {
            std::cout << amount << ", ";
        }
        std::cout << nHashType << ", \"";
        std::cout << (isDip0143 ? sh.GetHex() : sho.GetHex()) << "\"]";
        if (i+1 != nRandomTests) {
          std::cout << ",";
        }
        std::cout << "\n";
        #endif
        if (!isDip0143) {
            BOOST_CHECK(sh == sho);
        }
    }
    #if defined(PRINT_SIGHASH_JSON)
    std::cout << "]\n";
    #endif
}

static void TestSigHashFromData(SigVersion sigVersion)
{
    const bool isDip0143 = sigVersion == SigVersion::DIP0143;
    UniValue tests;
    if (isDip0143) {
        tests = read_json(std::string(json_tests::sighash_dip0143, json_tests::sighash_dip0143 + sizeof(json_tests::sighash_dip0143)));
    } else {
        tests = read_json(std::string(json_tests::sighash, json_tests::sighash + sizeof(json_tests::sighash)));
    }
    for (unsigned int idx = 0; idx < tests.size(); idx++) {
        UniValue test = tests[idx];
        std::string strTest = test.write();
        if (test.size() < 1) // Allow for extra stuff (useful for comments)
        {
            BOOST_ERROR("Bad test: " << strTest);
            continue;
        }
        if (test.size() == 1) continue; // comment

        std::string raw_tx, raw_script, sigHashHex;
        int nIn, nHashType;
        CAmount amount;
        uint256 sh;
        CTransactionRef tx;
        CScript scriptCode = CScript();

        try {
            // deserialize test data
            raw_tx = test[0].get_str();
            raw_script = test[1].get_str();
            nIn = test[2].get_int();
            amount = isDip0143 ? test[3].get_int64() : 0;
            size_t offset = isDip0143 ? 1 : 0;
            nHashType = test[3 + offset].get_int();
            sigHashHex = test[4 + offset].get_str();

            CDataStream stream(ParseHex(raw_tx), SER_NETWORK, PROTOCOL_VERSION);
            stream >> tx;

            TxValidationState state;
            BOOST_CHECK_MESSAGE(CheckTransaction(*tx, state), strTest);
            BOOST_CHECK(state.IsValid());

            std::vector<unsigned char> raw = ParseHex(raw_script);
            scriptCode.insert(scriptCode.end(), raw.begin(), raw.end());
        } catch (...) {
            BOOST_ERROR("Bad test, couldn't deserialize data: " << strTest);
            continue;
        }

        sh = SignatureHash(scriptCode, *tx, nIn, nHashType, amount, sigVersion);
        BOOST_CHECK_MESSAGE(sh.GetHex() == sigHashHex, strTest);
    }
}

BOOST_FIXTURE_TEST_SUITE(sighash_tests, BasicTestingSetup)

BOOST_AUTO_TEST_CASE(sighash_test)
{
    for (const auto sigVersion : {SigVersion::BASE, SigVersion::DIP0143}) {
        TestSigHash(sigVersion);
    }
}

// Goal: check that SignatureHash generates correct hash
BOOST_AUTO_TEST_CASE(sighash_from_data)
{
    for (const auto sigVersion : {SigVersion::BASE, SigVersion::DIP0143}) {
        TestSigHashFromData(sigVersion);
    }
}
BOOST_AUTO_TEST_SUITE_END()
