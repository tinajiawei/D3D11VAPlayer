#include "media/decoder_plugin.h"
#include "media/sw_decoder.h"

namespace {
me::IDecoder* create_sw() { return new me::SwDecoder(); }
}  // namespace

extern "C" ME_DECODER_API void me_register_decoders(me::DecoderRegistry* registry) {
    registry->add("sw", &create_sw);
}