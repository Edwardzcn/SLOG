#pragma once

#include <thread>
#include <unordered_map>

#include <zmq.hpp>

#include "module/module.h"

using std::unordered_map;

namespace slog {

class Server : public Module {
public:
  Server(Channel* listener);


private:
  unordered_map<uint32_t, MMessage> waiting_requests_;
};

} // namespace slog