#include "module/forwarder.h"

#include <glog/logging.h>

#include <algorithm>

#include "common/constants.h"
#include "common/monitor.h"
#include "common/proto_utils.h"

using std::move;
using std::sort;
using std::string;
using std::unique;

namespace slog {

using internal::Envelope;
using internal::Request;
using internal::Response;
namespace {

uint32_t ChooseRandomPartition(const Transaction& txn, std::mt19937& rg) {
  std::uniform_int_distribution<> idx(0, txn.internal().involved_partitions_size() - 1);
  return txn.internal().involved_partitions(idx(rg));
}

}  // namespace

Forwarder::Forwarder(const ConfigurationPtr& config, const shared_ptr<Broker>& broker,
                     const shared_ptr<LookupMasterIndex<Key, Metadata>>& lookup_master_index,
                     milliseconds batch_timeout, milliseconds poll_timeout)
    : NetworkedModule("Forwarder", broker, kForwarderChannel, poll_timeout),
      config_(config),
      lookup_master_index_(lookup_master_index),
      batch_timeout_(batch_timeout),
      rg_(std::random_device()()) {
  partitioned_lookup_request_.resize(config_->num_partitions());
}

void Forwarder::HandleInternalRequest(EnvelopePtr&& env) {
  switch (env->request().type_case()) {
    case Request::kForwardTxn:
      ProcessForwardTxn(move(env));
      break;
    case Request::kLookupMaster:
      ProcessLookUpMasterRequest(move(env));
      break;
    default:
      LOG(ERROR) << "Unexpected request type received: \"" << CASE_NAME(env->request().type_case(), Request) << "\"";
  }
}

void Forwarder::ProcessForwardTxn(EnvelopePtr&& env) {
  auto txn = env->mutable_request()->mutable_forward_txn()->mutable_txn();

  TRACE(txn->mutable_internal(), TransactionEvent::ENTER_FORWARDER);

  vector<uint32_t> involved_partitions;
  bool need_remote_lookup = false;

  // This function will be called on the read and write set of the current txn. It returns true if there is no error
  auto LocalMasterLookupFn = [this, txn, &involved_partitions,
                              &need_remote_lookup](const google::protobuf::Map<string, string>& keys) -> bool {
    auto txn_metadata = txn->mutable_internal()->mutable_master_metadata();
    for (auto& pair : keys) {
      const auto& key = pair.first;
      uint32_t partition = 0;

      try {
        partition = config_->partition_of_key(key);
      } catch (std::invalid_argument& e) {
        LOG(ERROR) << "Only numeric keys are allowed while running in Simple Partitioning mode";
        return false;
      }

      involved_partitions.push_back(partition);

      // If this is a local partition, lookup the master info from the local storage
      if (partition == config_->local_partition()) {
        auto& new_metadata = (*txn_metadata)[key];
        Metadata metadata;
        if (lookup_master_index_->GetMasterMetadata(key, metadata)) {
          new_metadata.set_master(metadata.master);
          new_metadata.set_counter(metadata.counter);
        } else {
          new_metadata.set_master(DEFAULT_MASTER_REGION_OF_NEW_KEY);
          new_metadata.set_counter(0);
        }
      } else {
        // Otherwise, add the key to the appropriate remote lookup master request
        partitioned_lookup_request_[partition].mutable_request()->mutable_lookup_master()->add_keys(key);
        need_remote_lookup = true;
      }
    }
    return true;
  };

  if (!LocalMasterLookupFn(txn->read_set())) {
    return;
  }
  if (!LocalMasterLookupFn(txn->write_set())) {
    return;
  }

  // Deduplicate involved partition list and set it to the txn
  sort(involved_partitions.begin(), involved_partitions.end());
  auto last = unique(involved_partitions.begin(), involved_partitions.end());
  *txn->mutable_internal()->mutable_involved_partitions() = {involved_partitions.begin(), last};

  // If there is no need to look master info from remote partitions,
  // forward the txn immediately
  if (!need_remote_lookup) {
    auto txn_type = SetTransactionType(*txn);
    VLOG(3) << "Determine txn " << txn->internal().id() << " to be " << ENUM_NAME(txn_type, TransactionType)
            << " without remote master lookup";
    DCHECK(txn_type != TransactionType::UNKNOWN);
    Forward(move(env));
    return;
  }

  VLOG(3) << "Remote master lookup needed to determine type of txn " << txn->internal().id();
  for (auto p : txn->internal().involved_partitions()) {
    partitioned_lookup_request_[p].mutable_request()->mutable_lookup_master()->add_txn_ids(txn->internal().id());
  }
  pending_transactions_.insert_or_assign(txn->internal().id(), move(env));

  // If the request is not scheduled to be sent yet, schedule one now
  if (!lookup_request_scheduled_) {
    NewTimedCallback(batch_timeout_, [this]() {
      auto local_rep = config_->local_replica();
      auto num_partitions = config_->num_partitions();
      for (uint32_t part = 0; part < num_partitions; part++) {
        if (part != config_->local_partition()) {
          Send(partitioned_lookup_request_[part], config_->MakeMachineId(local_rep, part), kForwarderChannel);
          partitioned_lookup_request_[part].Clear();
        }
      }
      lookup_request_scheduled_ = false;
    });
    lookup_request_scheduled_ = true;
  }
}

void Forwarder::ProcessLookUpMasterRequest(EnvelopePtr&& env) {
  const auto& lookup_master = env->request().lookup_master();
  Envelope lookup_env;
  auto lookup_response = lookup_env.mutable_response()->mutable_lookup_master();
  auto metadata_map = lookup_response->mutable_master_metadata();

  lookup_response->mutable_txn_ids()->CopyFrom(lookup_master.txn_ids());
  for (int i = 0; i < lookup_master.keys_size(); i++) {
    const auto& key = lookup_master.keys(i);

    if (config_->key_is_in_local_partition(key)) {
      Metadata metadata;
      if (lookup_master_index_->GetMasterMetadata(key, metadata)) {
        // If key exists, add the metadata of current key to the response
        auto& response_metadata = (*metadata_map)[key];
        response_metadata.set_master(metadata.master);
        response_metadata.set_counter(metadata.counter);
      } else {
        // Otherwise, assign it to the default region for new key
        auto& new_metadata = (*metadata_map)[key];
        new_metadata.set_master(DEFAULT_MASTER_REGION_OF_NEW_KEY);
        new_metadata.set_counter(0);
      }
    }
  }
  Send(lookup_env, env->from(), kForwarderChannel);
}

void Forwarder::HandleInternalResponse(EnvelopePtr&& env) {
  // The forwarder only cares about lookup master responses
  if (env->response().type_case() != Response::kLookupMaster) {
    LOG(ERROR) << "Unexpected response type received: \"" << CASE_NAME(env->response().type_case(), Response) << "\"";
  }

  const auto& lookup_master = env->response().lookup_master();
  for (auto txn_id : lookup_master.txn_ids()) {
    auto pending_txn_it = pending_transactions_.find(txn_id);
    if (pending_txn_it == pending_transactions_.end()) {
      continue;
    }

    // Transfer master info from the lookup response to its intended transaction
    auto& pending_env = pending_txn_it->second;
    auto txn = pending_env->mutable_request()->mutable_forward_txn()->mutable_txn();
    auto txn_master_metadata = txn->mutable_internal()->mutable_master_metadata();
    for (const auto& pair : txn->read_set()) {
      auto it = lookup_master.master_metadata().find(pair.first);
      if (it != lookup_master.master_metadata().end()) {
        txn_master_metadata->insert(*it);
      }
    }
    for (const auto& pair : txn->write_set()) {
      auto it = lookup_master.master_metadata().find(pair.first);
      if (it != lookup_master.master_metadata().end()) {
        txn_master_metadata->insert(*it);
      }
    }

    auto txn_type = SetTransactionType(*txn);
    if (txn_type != TransactionType::UNKNOWN) {
      VLOG(3) << "Determine txn " << txn->internal().id() << " to be " << ENUM_NAME(txn_type, TransactionType);
      Forward(move(pending_env));
      pending_transactions_.erase(txn_id);
    }
  }
}

void Forwarder::Forward(EnvelopePtr&& env) {
  auto txn = env->mutable_request()->mutable_forward_txn()->mutable_txn();
  auto txn_internal = txn->mutable_internal();
  auto txn_id = txn_internal->id();
  auto txn_type = txn_internal->type();
  auto& master_metadata = txn_internal->master_metadata();

  if (txn_type == TransactionType::SINGLE_HOME) {
    // If this current replica is its home, forward to the sequencer of the same machine
    // Otherwise, forward to the sequencer of a random machine in its home region
    auto home_replica = master_metadata.begin()->second.master();
    if (home_replica == config_->local_replica()) {
      VLOG(3) << "Current region is home of txn " << txn_id;

      TRACE(txn_internal, TransactionEvent::EXIT_FORWARDER_TO_SEQUENCER);

      Send(move(env), kSequencerChannel);
    } else {
      auto partition = ChooseRandomPartition(*txn, rg_);
      auto random_machine_in_home_replica = config_->MakeMachineId(home_replica, partition);

      VLOG(3) << "Forwarding txn " << txn_id << " to its home region (rep: " << home_replica << ", part: " << partition
              << ")";

      TRACE(txn_internal, TransactionEvent::EXIT_FORWARDER_TO_SEQUENCER);

      Send(*env, random_machine_in_home_replica, kSequencerChannel);
    }
  } else if (txn_type == TransactionType::MULTI_HOME) {
    VLOG(3) << "Txn " << txn_id << " is a multi-home txn. Sending to the orderer.";

    PopulateInvolvedReplicas(txn);

    TRACE(txn_internal, TransactionEvent::EXIT_FORWARDER_TO_MULTI_HOME_ORDERER);

    if (config_->bypass_mh_orderer()) {
      // Send the txn to sequencers of involved replicas to generate lock-only txns
      auto part = ChooseRandomPartition(*txn, rg_);
      vector<MachineId> destinations;
      for (auto rep : txn_internal->involved_replicas()) {
        destinations.push_back(config_->MakeMachineId(rep, part));
      }
      Send(*env, destinations, kSequencerChannel);

      // Send the txn to schedulers of involved partitions
      destinations.clear();
      for (auto part : txn->internal().involved_partitions()) {
        for (uint32_t rep = 0; rep < config_->num_replicas(); rep++) {
          destinations.push_back(config_->MakeMachineId(rep, part));
        }
      }
      Send(move(env), destinations, kSchedulerChannel);
    } else {
      auto mh_orderer =
          config_->MakeMachineId(config_->local_replica(), config_->leader_partition_for_multi_home_ordering());
      Send(*env, mh_orderer, kMultiHomeOrdererChannel);
    }
  }
}

}  // namespace slog