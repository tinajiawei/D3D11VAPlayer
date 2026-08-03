#include "media/decoder_plugin.h"
#include "media/amf_decoder.h"

namespace {
me::IDecoder* create_amf() { return new me::AmfDecoder(); }
}  // namespace

extern "C" ME_DECODER_API void me_register_decoders(me::DecoderRegistry* registry) {
    registry->add("amf", &create_amf);
}