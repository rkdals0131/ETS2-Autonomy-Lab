#pragma once
#include "wire.hpp"
#include <set>
namespace bridge {
using Demand=std::set<std::string>;
Packet state_messages(const json& state,const std::string& session,const std::array<double,3>& base);
Packet sensor_messages(std::span<const uint8_t> bundle,const std::string& session,const Demand& demand,uint64_t dropped);
}
