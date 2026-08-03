#include "media/decoder_plugin.h"
#include "media/d3d11va_decoder.h"

namespace {
me::IDecoder* create_d3d11va() { return new me::D3D11vaDecoder(); }
}  // namespace

extern "C" ME_DECODER_API void me_register_decoders(me::DecoderRegistry* registry) {
    registry->add("d3d11va", &create_d3d11va);
}