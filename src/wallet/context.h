// Copyright (c) 2020 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_WALLET_CONTEXT_H
#define BITCOIN_WALLET_CONTEXT_H

#include <sync.h>

#include <functional>
#include <list>
#include <memory>
#include <vector>

class ArgsManager;
class CWallet;
struct NodeContext;
namespace interfaces {
class Chain;
namespace CoinJoin {
class Loader;
} // namspace CoinJoin
class Wallet;
} // namespace interfaces

using LoadWalletFn = std::function<void(std::unique_ptr<interfaces::Wallet> wallet)>;

//! WalletContext struct containing references to state shared between CWallet
//! instances, like the reference to the chain interface, and the list of opened
//! wallets.
//!
//! Future shared state can be added here as an alternative to adding global
//! variables.
//!
//! The struct isn't intended to have any member functions. It should just be a
//! collection of state pointers that doesn't pull in dependencies or implement
//! behavior.
struct WalletContext {
    interfaces::Chain* chain{nullptr};
    ArgsManager* args{nullptr}; // Currently a raw pointer because the memory is not managed by this struct
    Mutex wallets_mutex;
    std::vector<std::shared_ptr<CWallet>> wallets GUARDED_BY(wallets_mutex);
    std::list<LoadWalletFn> wallet_load_fns GUARDED_BY(wallets_mutex);
    // TODO: replace this unique_ptr to a pointer
    // probably possible to do after bitcoin/bitcoin#22219
    const std::unique_ptr<interfaces::CoinJoin::Loader>& coinjoin_loader;
    // Some Dash RPCs rely on WalletContext yet access NodeContext members
    // even though wallet RPCs should refrain from accessing non-wallet
    // capabilities (even though it is a hard ask sometimes). We should get
    // rid of this at some point but until then, here's NodeContext.
    // TODO: Get rid of this. It's not nice.
    NodeContext* node_context{nullptr};

    //! Declare default constructor and destructor that are not inline, so code
    //! instantiating the WalletContext struct doesn't need to #include class
    //! definitions for smart pointer and container members.
    WalletContext(const std::unique_ptr<interfaces::CoinJoin::Loader>& coinjoin_loader);
    ~WalletContext();
};

#endif // BITCOIN_WALLET_CONTEXT_H
