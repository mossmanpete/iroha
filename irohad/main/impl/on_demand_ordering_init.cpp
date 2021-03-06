/**
 * Copyright Soramitsu Co., Ltd. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0
 */

#include "main/impl/on_demand_ordering_init.hpp"

#include "common/bind.hpp"
#include "common/delay.hpp"
#include "cryptography/crypto_provider/crypto_defaults.hpp"
#include "datetime/time.hpp"
#include "interfaces/common_objects/peer.hpp"
#include "interfaces/common_objects/types.hpp"
#include "ordering/impl/on_demand_common.hpp"
#include "ordering/impl/on_demand_connection_manager.hpp"
#include "ordering/impl/on_demand_ordering_gate.hpp"
#include "ordering/impl/on_demand_ordering_service_impl.hpp"
#include "ordering/impl/on_demand_os_client_grpc.hpp"
#include "ordering/impl/on_demand_os_server_grpc.hpp"
#include "ordering/impl/ordering_gate_cache/on_demand_cache.hpp"

namespace {
  /// match event and call corresponding lambda depending on sync_outcome
  template <typename OnBlocks, typename OnNothing>
  auto matchEvent(const iroha::synchronizer::SynchronizationEvent &event,
                  OnBlocks &&on_blocks,
                  OnNothing &&on_nothing) {
    using iroha::synchronizer::SynchronizationOutcomeType;
    switch (event.sync_outcome) {
      case SynchronizationOutcomeType::kCommit:
        return std::forward<OnBlocks>(on_blocks)(event);
      case SynchronizationOutcomeType::kReject:
      case SynchronizationOutcomeType::kNothing:
        return std::forward<OnNothing>(on_nothing)(event);
      default:
        BOOST_ASSERT_MSG(false, "Unknown value");
    }
  }
}  // namespace

namespace iroha {
  namespace network {

    auto OnDemandOrderingInit::createNotificationFactory(
        std::shared_ptr<network::AsyncGrpcClient<google::protobuf::Empty>>
            async_call,
        std::chrono::milliseconds delay) {
      return std::make_shared<ordering::transport::OnDemandOsClientGrpcFactory>(
          std::move(async_call),
          [] { return std::chrono::system_clock::now(); },
          delay);
    }

    auto OnDemandOrderingInit::createConnectionManager(
        std::shared_ptr<ametsuchi::PeerQueryFactory> peer_query_factory,
        std::shared_ptr<network::AsyncGrpcClient<google::protobuf::Empty>>
            async_call,
        std::chrono::milliseconds delay,
        std::vector<shared_model::interface::types::HashType> initial_hashes) {
      // since top block will be the first in notifier observable, hashes of
      // two previous blocks are prepended
      const size_t kBeforePreviousTop = 0, kPreviousTop = 1;

      // flat map hashes from committed blocks
      auto all_hashes = notifier.get_observable()
                            .flat_map([](auto commit) {
                              return commit.synced_blocks.map(
                                  [](auto block) { return block->hash(); });
                            })
                            // prepend hashes for the first two rounds
                            .start_with(initial_hashes.at(kBeforePreviousTop),
                                        initial_hashes.at(kPreviousTop));

      // emit last k + 1 hashes, where k is the delay parameter
      // current implementation assumes k = 2
      // first hash is used for kCurrentRound
      // second hash is used for kNextRound
      // third hash is used for kRoundAfterNext
      auto latest_hashes =
          all_hashes.zip(all_hashes.skip(1), all_hashes.skip(2));

      auto map_peers = [this, peer_query_factory](auto &&latest_data)
          -> ordering::OnDemandConnectionManager::CurrentPeers {
        auto &latest_commit = std::get<0>(latest_data);
        auto &current_hashes = std::get<1>(latest_data);

        consensus::Round current_round = latest_commit.round;

        auto on_blocks = [this,
                          peer_query_factory,
                          current_hashes,
                          &current_round](const auto &commit) {
          current_round = ordering::nextCommitRound(current_round);

          // retrieve peer list from database
          // TODO lebdron 08.11.2018 IR-1853 Refactor PeerQuery without
          // database access and optional
          peer_query_factory->createPeerQuery() | [](auto &&query) {
            return query->getLedgerPeers();
          } | [this](auto &&peers) { current_peers_ = std::move(peers); };

          // generate permutation of peers list from corresponding round
          // hash
          auto generate_permutation = [&](auto round) {
            auto &hash = std::get<round()>(current_hashes);
            log_->debug("Using hash: {}", hash.toString());
            auto &permutation = permutations_[round()];

            std::seed_seq seed(hash.blob().begin(), hash.blob().end());
            gen_.seed(seed);

            permutation.resize(current_peers_.size());
            std::iota(permutation.begin(), permutation.end(), 0);

            std::shuffle(permutation.begin(), permutation.end(), gen_);
          };

          generate_permutation(RoundTypeConstant<kCurrentRound>{});
          generate_permutation(RoundTypeConstant<kNextRound>{});
          generate_permutation(RoundTypeConstant<kRoundAfterNext>{});
        };
        auto on_nothing = [&current_round](const auto &) {
          current_round = ordering::nextRejectRound(current_round);
        };

        matchEvent(latest_commit, on_blocks, on_nothing);

        auto getOsPeer = [this, &current_round](auto block_round_advance,
                                                auto reject_round) {
          auto &permutation = permutations_[block_round_advance];
          // since reject round can be greater than number of peers, wrap it
          // with number of peers
          auto &peer =
              current_peers_[permutation[reject_round % permutation.size()]];
          log_->debug(
              "For {}, using OS on peer: {}",
              consensus::Round{current_round.block_round + block_round_advance,
                               reject_round},
              *peer);
          return peer;
        };

        using ordering::OnDemandConnectionManager;
        OnDemandConnectionManager::CurrentPeers peers;
        /*
         * See detailed description in
         * irohad/ordering/impl/on_demand_connection_manager.cpp
         *
         *   0 1 2
         * 0 o x v
         * 1 x v .
         * 2 v . .
         *
         * v, round 0 - kCurrentRoundRejectConsumer
         * v, round 1 - kNextRoundRejectConsumer
         * v, round 2 - kNextRoundCommitConsumer
         * o, round 0 - kIssuer
         */
        peers.peers.at(OnDemandConnectionManager::kCurrentRoundRejectConsumer) =
            getOsPeer(kCurrentRound,
                      ordering::currentRejectRoundConsumer(
                          current_round.reject_round));
        peers.peers.at(OnDemandConnectionManager::kNextRoundRejectConsumer) =
            getOsPeer(kNextRound, ordering::kNextRejectRoundConsumer);
        peers.peers.at(OnDemandConnectionManager::kNextRoundCommitConsumer) =
            getOsPeer(kRoundAfterNext, ordering::kNextCommitRoundConsumer);
        peers.peers.at(OnDemandConnectionManager::kIssuer) =
            getOsPeer(kCurrentRound, current_round.reject_round);
        return peers;
      };

      auto peers = notifier.get_observable()
                       .with_latest_from(latest_hashes)
                       .map(map_peers);

      return std::make_shared<ordering::OnDemandConnectionManager>(
          createNotificationFactory(std::move(async_call), delay), peers);
    }

    auto OnDemandOrderingInit::createGate(
        std::shared_ptr<ordering::OnDemandOrderingService> ordering_service,
        std::shared_ptr<ordering::transport::OdOsNotification> network_client,
        std::shared_ptr<ordering::cache::OrderingGateCache> cache,
        std::shared_ptr<shared_model::interface::UnsafeProposalFactory>
            proposal_factory,
        std::shared_ptr<ametsuchi::TxPresenceCache> tx_cache,
        consensus::Round initial_round,
        std::function<std::chrono::seconds(
            const synchronizer::SynchronizationEvent &)> delay_func) {

      auto map = [](auto commit) {
        return matchEvent(
            commit,
            [](const auto &commit)
                -> ordering::OnDemandOrderingGate::BlockRoundEventType {
              ordering::cache::OrderingGateCache::HashesSetType hashes;
              commit.synced_blocks.as_blocking().subscribe(
                  [&hashes](const auto &block) {
                    const auto &committed = block->transactions();
                    std::transform(committed.begin(),
                                   committed.end(),
                                   std::inserter(hashes, hashes.end()),
                                   [](const auto &transaction) {
                                     return transaction.hash();
                                   });
                    const auto &rejected =
                        block->rejected_transactions_hashes();
                    std::copy(rejected.begin(),
                              rejected.end(),
                              std::inserter(hashes, hashes.end()));
                  });
              return ordering::OnDemandOrderingGate::BlockEvent{
                  ordering::nextCommitRound(commit.round), hashes};
            },
            [](const auto &nothing)
                -> ordering::OnDemandOrderingGate::BlockRoundEventType {
              return ordering::OnDemandOrderingGate::EmptyEvent{
                  ordering::nextRejectRound(nothing.round)};
            });
      };

      return std::make_shared<ordering::OnDemandOrderingGate>(
          std::move(ordering_service),
          std::move(network_client),
          notifier.get_observable()
              .lift<iroha::synchronizer::SynchronizationEvent>(
                  iroha::makeDelay<iroha::synchronizer::SynchronizationEvent>(
                      delay_func, rxcpp::identity_current_thread()))
              .map(map),
          std::move(cache),
          std::move(proposal_factory),
          std::move(tx_cache),
          initial_round);
    }

    auto OnDemandOrderingInit::createService(
        size_t max_size,
        std::shared_ptr<shared_model::interface::UnsafeProposalFactory>
            proposal_factory,
        std::shared_ptr<ametsuchi::TxPresenceCache> tx_cache) {
      return std::make_shared<ordering::OnDemandOrderingServiceImpl>(
          max_size, std::move(proposal_factory), std::move(tx_cache));
    }

    OnDemandOrderingInit::~OnDemandOrderingInit() {
      notifier.get_subscriber().unsubscribe();
    }

    std::shared_ptr<iroha::network::OrderingGate>
    OnDemandOrderingInit::initOrderingGate(
        size_t max_size,
        std::chrono::milliseconds delay,
        std::vector<shared_model::interface::types::HashType> initial_hashes,
        std::shared_ptr<ametsuchi::PeerQueryFactory> peer_query_factory,
        std::shared_ptr<
            ordering::transport::OnDemandOsServerGrpc::TransportFactoryType>
            transaction_factory,
        std::shared_ptr<shared_model::interface::TransactionBatchParser>
            batch_parser,
        std::shared_ptr<shared_model::interface::TransactionBatchFactory>
            transaction_batch_factory,
        std::shared_ptr<network::AsyncGrpcClient<google::protobuf::Empty>>
            async_call,
        std::shared_ptr<shared_model::interface::UnsafeProposalFactory>
            proposal_factory,
        std::shared_ptr<ametsuchi::TxPresenceCache> tx_cache,
        consensus::Round initial_round,
        std::function<std::chrono::seconds(
            const synchronizer::SynchronizationEvent &)> delay_func) {
      auto ordering_service =
          createService(max_size, proposal_factory, tx_cache);
      service = std::make_shared<ordering::transport::OnDemandOsServerGrpc>(
          ordering_service,
          std::move(transaction_factory),
          std::move(batch_parser),
          std::move(transaction_batch_factory));
      return createGate(ordering_service,
                        createConnectionManager(std::move(peer_query_factory),
                                                std::move(async_call),
                                                delay,
                                                std::move(initial_hashes)),
                        std::make_shared<ordering::cache::OnDemandCache>(),
                        std::move(proposal_factory),
                        std::move(tx_cache),
                        initial_round,
                        std::move(delay_func));
    }

  }  // namespace network
}  // namespace iroha
