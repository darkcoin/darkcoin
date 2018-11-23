// Copyright (c) 2018 The Dash Core developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "quorums_init.h"

#include "quorums_blockprocessor.h"
#include "quorums_commitment.h"

namespace llmq
{

void InitLLMQSystem(CEvoDB& evoDb)
{
    quorumBlockProcessor = new CQuorumBlockProcessor(evoDb);
}

void DestroyLLMQSystem()
{
    delete quorumBlockProcessor;
    quorumBlockProcessor = nullptr;
}

}
