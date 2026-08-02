#include "core/av_utils.h"

namespace me {

std::string av_error_string(int ret) {
    char buffer[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(ret, buffer, sizeof(buffer));
    return buffer;
}

Error error_from_av(int ret, std::string_view what) {
    return Error::make(Err::DecodeFailed,
                       std::string(what) + " 失败: " + av_error_string(ret));
}

}  // namespace me