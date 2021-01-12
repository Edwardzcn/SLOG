#pragma once

#include <google/protobuf/any.pb.h>

#include <zmq.hpp>

#include "common/types.h"
#include "proto/internal.pb.h"

namespace slog {

using EnvelopePtr = std::unique_ptr<internal::Envelope>;

/**
 * Sends a pointer to an envelope
 */
inline void SendEnvelope(zmq::socket_t& socket, EnvelopePtr&& envelope) {
  auto env = envelope.release();
  size_t sz = sizeof(env);
  zmq::message_t msg(sz);
  *(msg.data<internal::Envelope*>()) = env;
  socket.send(msg, zmq::send_flags::none);
}

inline EnvelopePtr RecvEnvelope(zmq::socket_t& socket, bool dont_wait = false) {
  zmq::message_t msg;
  auto flag = dont_wait ? zmq::recv_flags::dontwait : zmq::recv_flags::none;
  if (!socket.recv(msg, flag)) {
    return nullptr;
  }
  return EnvelopePtr(*(msg.data<internal::Envelope*>()));
}

inline zmq::message_t SerializeProto(const google::protobuf::Message& proto) {
  google::protobuf::Any any;
  any.PackFrom(proto);

  size_t sz = sizeof(MachineId) + sizeof(Channel) + any.ByteSizeLong();
  zmq::message_t msg(sz);
  auto data = msg.data<char>() + sizeof(MachineId) + sizeof(Channel);
  any.SerializeToArray(data, any.ByteSizeLong());

  return msg;
}

inline void SendAddressedBuffer(zmq::socket_t& socket, zmq::message_t&& msg, MachineId from_machine_id = -1,
                                Channel to_chan = 0) {
  auto data = msg.data<char>();
  memcpy(data, &from_machine_id, sizeof(MachineId));
  data += sizeof(MachineId);
  memcpy(data, &to_chan, sizeof(Channel));

  socket.send(msg, zmq::send_flags::none);
}

/**
 * Serializes and send proto message. The sent buffer contains
 * <sender machine id> <receiver channel> <proto>
 */
inline void SendSerializedProto(zmq::socket_t& socket, const google::protobuf::Message& proto,
                                MachineId from_machine_id = -1, Channel to_chan = 0) {
  SendAddressedBuffer(socket, SerializeProto(proto), from_machine_id, to_chan);
}

inline void SendSerializedProtoWithEmptyDelim(zmq::socket_t& socket, const google::protobuf::Message& proto) {
  socket.send(zmq::message_t{}, zmq::send_flags::sndmore);
  SendSerializedProto(socket, proto);
}

inline bool ParseMachineId(MachineId& id, const zmq::message_t& msg) {
  if (msg.size() < sizeof(MachineId)) {
    return false;
  }
  memcpy(&id, msg.data<char>(), sizeof(MachineId));
  return true;
}

inline bool ParseChannel(Channel& chan, const zmq::message_t& msg) {
  if (msg.size() < sizeof(MachineId) + sizeof(Channel)) {
    return false;
  }
  memcpy(&chan, msg.data<char>() + sizeof(MachineId), sizeof(Channel));
  return true;
}

inline bool DeserializeAny(google::protobuf::Any& any, const zmq::message_t& msg) {
  auto header_sz = sizeof(MachineId) + sizeof(Channel);
  if (msg.size() < header_sz) {
    return false;
  }
  // Skip the machineid and channel part
  auto proto_data = msg.data<char>() + header_sz;
  auto proto_size = msg.size() - header_sz;
  if (!any.ParseFromArray(proto_data, proto_size)) {
    return false;
  }
  return true;
}

template <typename T>
inline bool DeserializeProto(T& out, const zmq::message_t& msg) {
  google::protobuf::Any any;
  if (!DeserializeAny(any, msg)) {
    return false;
  }
  return any.UnpackTo(&out);
}

template <typename T>
inline bool RecvDeserializedProto(zmq::socket_t& socket, T& out, bool dont_wait = false) {
  zmq::message_t msg;
  auto flag = dont_wait ? zmq::recv_flags::dontwait : zmq::recv_flags::none;
  if (!socket.recv(msg, flag)) {
    return false;
  }
  return DeserializeProto(out, msg);
}

template <typename T>
inline bool RecvDeserializedProtoWithEmptyDelim(zmq::socket_t& socket, T& out, bool dont_wait = false) {
  if (zmq::message_t empty; !socket.recv(empty) || !empty.more()) {
    return false;
  }
  return RecvDeserializedProto(socket, out, dont_wait);
}

}  // namespace slog