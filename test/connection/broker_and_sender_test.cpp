#include <gtest/gtest.h>

#include <thread>
#include <vector>

#include "common/configuration.h"
#include "common/constants.h"
#include "common/proto_utils.h"
#include "connection/broker.h"
#include "connection/sender.h"
#include "connection/zmq_utils.h"
#include "proto/internal.pb.h"
#include "test/test_utils.h"

using namespace std;
using namespace slog;
using internal::Envelope;
using internal::Request;
using internal::Response;

zmq::socket_t MakePullSocket(zmq::context_t& context, Channel chan) {
  zmq::socket_t socket(context, ZMQ_PULL);
  socket.bind(MakeInProcChannelAddress(chan));
  return socket;
}

EnvelopePtr MakeEchoRequest(const std::string& data) {
  auto env = std::make_unique<internal::Envelope>();
  auto echo = env->mutable_request()->mutable_echo();
  echo->set_data(data);
  return env;
}

EnvelopePtr MakeEchoResponse(const std::string& data) {
  auto env = std::make_unique<internal::Envelope>();
  auto echo = env->mutable_response()->mutable_echo();
  echo->set_data(data);
  return env;
}

TEST(BrokerAndSenderTest, PingPong) {
  const Channel PING = 1;
  const Channel PONG = 2;
  ConfigVec configs = MakeTestConfigurations("pingpong", 1, 2);

  auto ping = thread([&]() {
    auto context = make_shared<zmq::context_t>(1);

    auto socket = MakePullSocket(*context, PING);

    auto broker = make_shared<Broker>(configs[0], context);
    broker->AddChannel(PING);
    broker->StartInNewThread();

    Sender sender(broker);
    // Send ping
    auto ping_req = MakeEchoRequest("ping");
    sender.Send(*ping_req, configs[0]->MakeMachineId(0, 1), PONG);

    // Wait for pong
    auto res = RecvEnvelope(socket);
    ASSERT_TRUE(res != nullptr);
    ASSERT_TRUE(res->has_response());
    ASSERT_EQ("pong", res->response().echo().data());
  });

  auto pong = thread([&]() {
    auto context = std::make_shared<zmq::context_t>(1);

    auto socket = MakePullSocket(*context, PONG);

    auto broker = make_shared<Broker>(configs[1], context);
    broker->AddChannel(PONG);
    broker->StartInNewThread();

    Sender sender(broker);

    // Wait for ping
    auto req = RecvEnvelope(socket);
    ASSERT_TRUE(req != nullptr);
    ASSERT_TRUE(req->has_request());
    ASSERT_EQ("ping", req->request().echo().data());

    // Send pong
    auto pong_res = MakeEchoResponse("pong");
    sender.Send(*pong_res, configs[1]->MakeMachineId(0, 0), PING);
  });

  ping.join();
  pong.join();
}

TEST(BrokerTest, LocalPingPong) {
  const Channel PING = 1;
  const Channel PONG = 2;
  ConfigVec configs = MakeTestConfigurations("local_ping_pong", 1, 1);
  auto context = std::make_shared<zmq::context_t>(1);
  context->set(zmq::ctxopt::blocky, false);
  auto broker = make_shared<Broker>(configs[0], context);
  broker->AddChannel(PING);
  broker->AddChannel(PONG);

  broker->StartInNewThread();

  auto ping = thread([&]() {
    Sender sender(broker);
    auto socket = MakePullSocket(*context, PING);

    // Send ping
    sender.Send(MakeEchoRequest("ping"), PONG);

    // Wait for pong
    auto res = RecvEnvelope(socket);
    ASSERT_TRUE(res != nullptr);
    ASSERT_EQ("pong", res->response().echo().data());
  });

  auto pong = thread([&]() {
    Sender sender(broker);
    auto socket = MakePullSocket(*context, PONG);

    // Wait for ping
    auto req = RecvEnvelope(socket);
    ASSERT_TRUE(req != nullptr);
    ASSERT_EQ("ping", req->request().echo().data());

    // Send pong
    sender.Send(MakeEchoResponse("pong"), PING);
  });

  ping.join();
  pong.join();
}

TEST(BrokerTest, MultiSend) {
  const Channel PING = 1;
  const Channel PONG = 2;
  const int NUM_PONGS = 3;
  ConfigVec configs = MakeTestConfigurations("pingpong", 1, NUM_PONGS + 1);

  auto ping = thread([&]() {
    auto context = make_shared<zmq::context_t>(1);

    auto socket = MakePullSocket(*context, PING);

    auto broker = make_shared<Broker>(configs[0], context);
    broker->AddChannel(PING);
    broker->StartInNewThread();

    Sender sender(broker);
    // Send ping
    auto ping_req = MakeEchoRequest("ping");
    vector<MachineId> dests;
    for (int i = 0; i < NUM_PONGS; i++) {
      dests.push_back(configs[0]->MakeMachineId(0, i + 1));
    }
    sender.Send(*ping_req, dests, PONG);

    // Wait for pongs
    for (int i = 0; i < NUM_PONGS; i++) {
      auto res = RecvEnvelope(socket);
      ASSERT_TRUE(res != nullptr);
      ASSERT_TRUE(res->has_response());
      ASSERT_EQ("pong", res->response().echo().data());
    }
  });

  thread pongs[NUM_PONGS];
  for (int i = 0; i < NUM_PONGS; i++) {
    pongs[i] = thread([&configs, i]() {
      auto context = std::make_shared<zmq::context_t>(1);

      auto socket = MakePullSocket(*context, PONG);

      auto broker = make_shared<Broker>(configs[i + 1], context);
      broker->AddChannel(PONG);
      broker->StartInNewThread();

      Sender sender(broker);

      // Wait for ping
      auto req = RecvEnvelope(socket);
      ASSERT_TRUE(req != nullptr);
      ASSERT_TRUE(req->has_request());
      ASSERT_EQ("ping", req->request().echo().data());

      // Send pong
      auto pong_res = MakeEchoResponse("pong");
      sender.Send(*pong_res, configs[i + 1]->MakeMachineId(0, 0), PING);

      this_thread::sleep_for(200ms);
    });
  }

  ping.join();
  for (int i = 0; i < NUM_PONGS; i++) {
    pongs[i].join();
  }
}

TEST(BrokerTest, CreateRedirection) {
  const Channel PING = 1;
  const Channel PONG = 2;
  const Channel TAG = 11111;
  ConfigVec configs = MakeTestConfigurations("pingpong", 1, 2);

  // Initialize ping machine
  auto ping_context = make_shared<zmq::context_t>(1);
  auto ping_socket = MakePullSocket(*ping_context, PING);
  auto ping_broker = make_shared<Broker>(configs[0], ping_context);
  ping_broker->AddChannel(PING);
  ping_broker->StartInNewThread();
  Sender ping_sender(ping_broker);

  // Establish a redirection from TAG to the PING channel at the ping machine.
  // We do it early so that hopefully the redirection is established by the time
  // we receive the pong message
  {
    auto env = std::make_unique<internal::Envelope>();
    auto redirect = env->mutable_request()->mutable_broker_redirect();
    redirect->set_tag(TAG);
    redirect->set_channel(PING);
    ping_sender.Send(move(env), kBrokerChannel);
  }

  // Initialize pong machine
  auto pong_context = make_shared<zmq::context_t>(1);
  auto pong_socket = MakePullSocket(*pong_context, PONG);
  auto pong_broker = make_shared<Broker>(configs[1], pong_context);
  pong_broker->AddChannel(PONG);
  pong_broker->StartInNewThread();
  Sender pong_sender(pong_broker);

  // Send ping message with a tag of the pong machine.
  {
    auto ping_req = MakeEchoRequest("ping");
    ping_sender.Send(*ping_req, configs[0]->MakeMachineId(0, 1), TAG);
  }

  // The pong machine does not know which channel to forward to yet at this point
  // so the message will be queued up at the broker
  this_thread::sleep_for(5ms);
  ASSERT_EQ(RecvEnvelope(pong_socket, true), nullptr);

  // Establish a redirection from TAG to the PONG channel at the pong machine
  {
    auto env = std::make_unique<internal::Envelope>();
    auto redirect = env->mutable_request()->mutable_broker_redirect();
    redirect->set_tag(TAG);
    redirect->set_channel(PONG);
    pong_sender.Send(move(env), kBrokerChannel);
  }

  // Now we can receive the ping message
  {
    auto ping_req = RecvEnvelope(pong_socket);
    ASSERT_TRUE(ping_req != nullptr);
    ASSERT_TRUE(ping_req->has_request());
    ASSERT_EQ("ping", ping_req->request().echo().data());
  }

  // Send pong
  {
    auto pong_res = MakeEchoResponse("pong");
    pong_sender.Send(*pong_res, configs[1]->MakeMachineId(0, 0), TAG);
  }

  // We should be able to receive pong here since we already establish a redirection at
  // the beginning for the ping machine
  {
    auto pong_res = RecvEnvelope(ping_socket);
    ASSERT_TRUE(pong_res != nullptr);
    ASSERT_TRUE(pong_res->has_response());
    ASSERT_EQ("pong", pong_res->response().echo().data());
  }
}

TEST(BrokerTest, RemoveRedirection) {
  const Channel PING = 1;
  const Channel PONG = 2;
  const Channel TAG = 11111;
  ConfigVec configs = MakeTestConfigurations("pingpong", 1, 2);

  // Initialize ping machine
  auto ping_context = make_shared<zmq::context_t>(1);
  auto ping_socket = MakePullSocket(*ping_context, PING);
  auto ping_broker = make_shared<Broker>(configs[0], ping_context);
  ping_broker->AddChannel(PING);
  ping_broker->StartInNewThread();
  Sender ping_sender(ping_broker);

  // Initialize pong machine
  auto pong_context = make_shared<zmq::context_t>(1);
  auto pong_socket = MakePullSocket(*pong_context, PONG);
  auto pong_broker = make_shared<Broker>(configs[1], pong_context);
  pong_broker->AddChannel(PONG);
  pong_broker->StartInNewThread();
  Sender pong_sender(pong_broker);

  // Establish a redirection from TAG to the PONG channel at the pong machine
  {
    auto env = std::make_unique<internal::Envelope>();
    auto redirect = env->mutable_request()->mutable_broker_redirect();
    redirect->set_tag(TAG);
    redirect->set_channel(PONG);
    pong_sender.Send(move(env), kBrokerChannel);
  }

  // Send ping message with a tag of the pong machine.
  {
    auto ping_req = MakeEchoRequest("ping");
    ping_sender.Send(*ping_req, configs[0]->MakeMachineId(0, 1), TAG);
  }

  // Now we can the ping message here
  {
    auto ping_req = RecvEnvelope(pong_socket);
    ASSERT_TRUE(ping_req != nullptr);
    ASSERT_TRUE(ping_req->has_request());
    ASSERT_EQ("ping", ping_req->request().echo().data());
  }

  // Remove the redirection
  {
    auto env = std::make_unique<internal::Envelope>();
    auto redirect = env->mutable_request()->mutable_broker_redirect();
    redirect->set_tag(TAG);
    redirect->set_stop(true);
    pong_sender.Send(move(env), kBrokerChannel);
  }

  // Send ping message again
  {
    auto ping_req = MakeEchoRequest("ping");
    ping_sender.Send(*ping_req, configs[0]->MakeMachineId(0, 1), TAG);
  }

  // The redirection is removed so we shouldn't be able to receive anything here
  // Theoretically, it is possible that the recv function is called before the
  // pong broker removes the redirection, making the assertion to fail. However,
  // it should be unlikely due to the sleep.
  this_thread::sleep_for(5ms);
  ASSERT_EQ(RecvEnvelope(pong_socket, true), nullptr);
}