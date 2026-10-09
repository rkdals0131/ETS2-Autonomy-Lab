#pragma once
#include "messages.hpp"
#include "geometry.hpp"
#include <fastcdr/Cdr.h>
#include <fastcdr/FastBuffer.h>

namespace bridge {
using eprosima::fastcdr::Cdr;
inline void stamp(Cdr& c,uint64_t us) {c<<static_cast<int32_t>(us/1000000)<<static_cast<uint32_t>((us%1000000)*1000);}
inline void header(Cdr& c,uint64_t us,const std::string& frame) {stamp(c,us);c<<frame;}
inline void pose(Cdr& c,V p,Q q) {c.serialize_array(p.data(),3);c.serialize_array(q.data(),4);}
template<class F> void append_cdr(Packet& packet,const std::string& topic,size_t capacity,F write) {
    const auto offset=packet.data.size();
    // Value initialization also clears CDR alignment padding before it crosses
    // the process boundary. Serialize into the final packet, without a copy.
    packet.data.resize(offset+capacity);
    eprosima::fastcdr::FastBuffer buffer(reinterpret_cast<char*>(packet.data.data()+offset),capacity);
    Cdr c(buffer,Cdr::LITTLE_ENDIANNESS,eprosima::fastcdr::CdrVersion::XCDRv1);
    c.set_encoding_flag(eprosima::fastcdr::EncodingAlgorithmFlag::PLAIN_CDR);
    c.serialize_encapsulation();write(c);
    const auto length=c.get_serialized_data_length();packet.data.resize(offset+length);
    packet.meta["messages"].push_back({{"topic",topic},{"offset",offset},{"length",length}});
}
}
