// Copyright (c) 2018-2019 The Dash Core developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef DASH_QUORUMS_INIT_H
#define DASH_QUORUMS_INIT_H

class CEvoDB;
class CScheduler;

namespace llmq
{

// If true, we will connect to all new quorums and watch their communication
static const bool DEFAULT_WATCH_QUORUMS = false;

void InitLLMQSystem(CEvoDB& evoDb, CScheduler* scheduler, bool unitTests);
void InterruptLLMQSystem();
void DestroyLLMQSystem();

}

#endif //DASH_QUORUMS_INIT_H
