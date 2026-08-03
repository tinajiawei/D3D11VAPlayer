#include "media/decoder_factory.h"

#include <mutex>
#include <vector>
#include <cstdlib>

#include <windows.h>

#include "core/log.h"
#include "media/decoder_plugin.h"

namespace me {

namespace {

struct Backend {
    std::string name;
    IDecoder* (*creator)();
};

std::vector<Backend>& backends() {
    static std::vector<Backend> registry;
    return registry;
}

std::string to_utf8(const std::wstring& w) {
    if (w.empty()) return {};
    const int len = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string s(len > 0 ? len - 1 : 0, '\0');
    if (len > 0) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, s.data(), len, nullptr, nullptr);
    return s;
}

void registry_add(const char* name, IDecoder* (*creator)()) {
    if (name && creator) {
        backends().push_back(Backend{std::string(name), creator});
    }
}

void load_plugins_impl() {
    // 插件目录：exe 旁的 plugin\1\（版本号=接口版本，升接口时换目录，旧插件不加载）
    wchar_t exe_path[MAX_PATH] = {};
    if (GetModuleFileNameW(nullptr, exe_path, MAX_PATH) == 0) {
        ME_LOG_WARN("[decoder] 获取 exe 路径失败，跳过插件加载");
        return;
    }
    std::wstring dir(exe_path);
    const size_t pos = dir.find_last_of(L"\\/");
    dir = (pos == std::wstring::npos) ? L"" : dir.substr(0, pos + 1);
    dir += L"plugin\\1\\";

    WIN32_FIND_DATAW fd = {};
    const HANDLE hfind = FindFirstFileW((dir + L"*.dll").c_str(), &fd);
    if (hfind == INVALID_HANDLE_VALUE) {
        ME_LOG_WARN("[decoder] 未找到解码器插件目录: ", to_utf8(dir));
        return;
    }
    do {
        const std::wstring path = dir + fd.cFileName;
        // 指定搜索 DLL 自身目录 + 默认目录（FFmpeg DLL 在 exe 旁，可被找到）
        const HMODULE mod = LoadLibraryExW(
            path.c_str(), nullptr,
            LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        if (!mod) {
            ME_LOG_WARN("[decoder] 插件加载失败: ", to_utf8(fd.cFileName), " err=", GetLastError());
            continue;
        }
        auto reg = reinterpret_cast<void (*)(DecoderRegistry*)>(
            GetProcAddress(mod, "me_register_decoders"));
        if (!reg) {
            FreeLibrary(mod);
            ME_LOG_WARN("[decoder] 插件缺少 me_register_decoders: ", to_utf8(fd.cFileName));
            continue;
        }
        DecoderRegistry registry{&registry_add};
        reg(&registry);
        ME_LOG_INFO("[decoder] 已加载插件: ", to_utf8(fd.cFileName));
        // 不保存 HMODULE：进程生命周期内不卸载，便于后续做热重载（需额外线程安全处理）
    } while (FindNextFileW(hfind, &fd));
    FindClose(hfind);
}

void load_plugins_once() {
    static std::once_flag flag;
    std::call_once(flag, load_plugins_impl);
}

std::unique_ptr<IDecoder> open_named(const std::string& name, const AVCodecParameters& params) {
    for (auto& backend : backends()) {
        if (backend.name != name) continue;
        std::unique_ptr<IDecoder> decoder(backend.creator());
        if (!decoder) return nullptr;
        Error err = decoder->open(params);
        if (err.ok()) return decoder;
        ME_LOG_WARN("[decoder] 后端 ", backend.name, " 初始化失败: ", err.message());
        return nullptr;
    }
    return nullptr;
}

}  // namespace

void DecoderFactory::register_backend(std::string_view name, Creator creator) {
    backends().push_back(Backend{std::string(name), creator});
}

std::unique_ptr<IDecoder> DecoderFactory::create(const AVCodecParameters& params, bool prefer_hw) {
    load_plugins_once();

    // 硬解优先：尝试所有注册的硬件后端
    // 硬解优先：依次尝试 d3d11va -> amf，可用 ME_HW_BACKEND 强制指定
    if (prefer_hw) {
        const char* forced = std::getenv("ME_HW_BACKEND");
        if (forced && *forced) {
            auto forced_decoder = open_named(forced, params);
            if (forced_decoder) {
                ME_LOG_INFO("解码器选择: ", forced_decoder->name());
                return forced_decoder;
            }
            ME_LOG_WARN("强制后端 ", forced, " 初始化失败");
        }
        for (const char* hw_name : {"d3d11va", "amf"}) {
            auto decoder = open_named(hw_name, params);
            if (decoder) {
                ME_LOG_INFO("解码器选择: ", decoder->name());
                return decoder;
            }
            ME_LOG_WARN("硬解后端 ", hw_name, " 初始化失败，尝试下一个");
        }
    }
    // 软解兜底
    auto decoder = open_named("sw", params);
    if (decoder) {
        ME_LOG_INFO("解码器选择: ", decoder->name());
        return decoder;
    }
    ME_LOG_ERROR("没有可用的解码器插件（plugin\\1\\*.dll）");
    return nullptr;
}

std::string DecoderFactory::available_backends() {
    load_plugins_once();
    std::string result;
    for (auto& backend : backends()) {
        if (!result.empty()) result += ", ";
        result += backend.name;
    }
    return result;
}

}  // namespace me
