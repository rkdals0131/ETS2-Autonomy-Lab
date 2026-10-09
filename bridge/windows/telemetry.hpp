#pragma once
#include "geometry.hpp"
#include <limits>

namespace bridge {
inline double channel(const json& state,const char* name) {
    const auto& sdk=state.at("sdk");
    if(!sdk.contains(name) || !sdk.at(name).value("available",false)) return std::numeric_limits<double>::quiet_NaN();
    return sdk.at(name).at("value").get<double>();
}
inline V vector_channel(const json& state,const char* name) {
    const auto& sdk=state.at("sdk");const auto nan=std::numeric_limits<double>::quiet_NaN();
    if(!sdk.contains(name) || !sdk.at(name).value("available",false)) return {nan,nan,nan};
    return mul(transpose(base_to_model),sdk.at(name).at("value").get<V>());
}
}
