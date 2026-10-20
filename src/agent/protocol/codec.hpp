#pragma once

#include <highcube.pb.h>

#include <string>

namespace highcube::agent {

::highcube::Envelope makeEnvelope(::highcube::MessageKind kind,
                                  const google::protobuf::Message& payload);
bool parseEnvelope(const std::string& bytes, ::highcube::Envelope& out);

}  // namespace highcube::agent
