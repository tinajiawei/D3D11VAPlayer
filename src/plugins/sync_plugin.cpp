#include "api/sync_plugin.h"

#include <cstdio>

#include "sync/sync_engine.h"

extern "C" {

ME_SYNC_API int me_sync_abi_version(void) {
    return ME_SYNC_ABI_VERSION;
}

ME_SYNC_API void* me_sync_create(int type, char* error_buf, int error_buf_size) {
    if (type != ME_SYNC_TYPE_MASTERCLOCK) {
        if (error_buf && error_buf_size > 0) {
            std::snprintf(error_buf, static_cast<size_t>(error_buf_size),
                          "sync type %d not supported by this plugin", type);
        }
        return nullptr;
    }
    // 返回未初始化实例：宿主（MediaPlayer）通过 ISyncEngine 控制生命周期
    return new me::SyncEngine();
}

ME_SYNC_API void me_sync_destroy(void* sync) {
    if (!sync) return;
    delete static_cast<me::SyncEngine*>(sync);
}

}  // extern "C"