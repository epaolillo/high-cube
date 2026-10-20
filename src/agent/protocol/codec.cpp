#include "protocol/codec.hpp"

namespace highcube::agent {

::highcube::Envelope makeEnvelope(::highcube::MessageKind kind,
                                  const google::protobuf::Message& payload) {
  ::highcube::Envelope envelope;
  envelope.set_kind(kind);
  envelope.set_payload(payload.SerializeAsString());
  return envelope;
}

bool parseEnvelope(const std::string& bytes, ::highcube::Envelope& out) {
  return out.ParseFromString(bytes);
}

}  // namespace highcube::agent
