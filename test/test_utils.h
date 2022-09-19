#pragma once

#include <glog/logging.h>

#include <optional>
#include <unordered_set>
#include <vector>

#include "common/debug.h"
#include "common/configuration.h"
#include "connection/broker.h"
#include "connection/sender.h"
#include "connection/zmq_utils.h"
#include "module/base/module.h"
#include "module/scheduler_components/txn_holder.h"
#include "proto/internal.pb.h"
#ifdef MYSQL_DEBUG
#include "storage/mysql_storage.h"
#else
#include "storage/mem_only_storage.h"
#endif
#include "storage/metadata_initializer.h"

using std::pair;
using std::shared_ptr;
using std::string;
using std::unique_ptr;
namespace slog {

const auto kTestModuleTimeout = std::chrono::milliseconds(5);

using ConfigVec = std::vector<ConfigurationPtr>;

ConfigVec MakeTestConfigurations(string&& prefix, int num_replicas, int num_partitions,
                                 internal::Configuration common_config = {});

Transaction* MakeTestTransaction(const ConfigurationPtr& config, TxnId id, const std::vector<KeyMetadata>& keys,
                                 const std::vector<std::vector<std::string>> code = {{}},
                                 std::optional<int> remaster = std::nullopt, MachineId coordinator = 0);

TxnHolder MakeTestTxnHolder(const ConfigurationPtr& config, TxnId id, const std::vector<KeyMetadata>& keys,
                            const std::vector<std::vector<std::string>> code = {{}},
                            std::optional<int> remaster = std::nullopt);

ValueEntry TxnValueEntry(const Transaction& txn, const std::string& key);

using ModuleRunnerPtr = unique_ptr<ModuleRunner>;

/**
 * This is a fake SLOG system where we can only add a subset
 * of modules to test them in isolation.
 */
class TestSlog {
 public:
  TestSlog(const ConfigurationPtr& config);
  void Data(Key&& key, Record&& record);
  void AddServerAndClient();
  void AddForwarder();
  void AddSequencer();
  void AddInterleaver();
  void AddScheduler();
  void AddLocalPaxos();
  void AddGlobalPaxos();
  void AddMultiHomeOrderer();

  void AddOutputSocket(Channel channel);
  zmq::pollitem_t GetPollItemForOutputSocket(Channel channel, bool inproc = true);
  EnvelopePtr ReceiveFromOutputSocket(Channel channel, bool inproc = true);

  unique_ptr<Sender> NewSender();

  void StartInNewThreads();
  void SendTxn(Transaction* txn);
  Transaction RecvTxnResult();

  const ConfigurationPtr& config() const { return config_; }
  const SharderPtr& sharder() const { return sharder_; }
  const std::shared_ptr<MetadataInitializer>& metadata_initializer() const { return metadata_initializer_; }

 private:
  ConfigurationPtr config_;
  SharderPtr sharder_;
  #ifdef MYSQL_DEBUG
  shared_ptr<MySQLStorage> storage_;
  #else
  shared_ptr<MemOnlyStorage> storage_;
  #endif

  shared_ptr<MetadataInitializer> metadata_initializer_;
  shared_ptr<Broker> broker_;
  ModuleRunnerPtr server_;
  ModuleRunnerPtr forwarder_;
  ModuleRunnerPtr sequencer_;
  ModuleRunnerPtr interleaver_;
  ModuleRunnerPtr scheduler_;
  ModuleRunnerPtr local_paxos_;
  ModuleRunnerPtr global_paxos_;
  ModuleRunnerPtr multi_home_orderer_;

  std::unordered_map<Channel, zmq::socket_t> inproc_sockets_;
  std::unordered_map<Channel, zmq::socket_t> outproc_sockets_;

  zmq::context_t client_context_;
  zmq::socket_t client_socket_;
};

}  // namespace slog