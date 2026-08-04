// PDB 符号化工具：
//   pdb_sym.exe <pdb> <rva...>         解析一个或多个 RVA（十六进制，可带 0x）
//   pdb_sym.exe <pdb> --find <子串>     枚举函数并按名字子串过滤（生成函数映射）
// 通过 DIA SDK（msdia140.dll，免注册加载）读取 PDB。
#include <windows.h>
#include <objbase.h>
#include <dia2.h>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#pragma comment(lib, "ole32.lib")

using DllGetClassObjectFn = HRESULT(WINAPI*)(REFCLSID, REFIID, LPVOID*);

static std::wstring find_msdia140() {
    std::vector<std::wstring> candidates;
    wchar_t buf[MAX_PATH] = {};
    if (GetModuleFileNameW(nullptr, buf, MAX_PATH)) {
        std::wstring dir = buf;
        const size_t slash = dir.find_last_of(L"\\\\");
        if (slash != std::wstring::npos) candidates.push_back(dir.substr(0, slash + 1) + L"msdia140.dll");
    }
    const wchar_t* roots[] = {
        L"%ProgramFiles%\\\\Microsoft Visual Studio\\\\2022",
        L"%ProgramFiles(x86)%\\\\Microsoft Visual Studio\\\\2022",
        L"D:\\\\Microsoft Visual Studio",
        L"C:\\\\Program Files\\\\Microsoft Visual Studio",
    };
    for (const wchar_t* root : roots) {
        wchar_t expanded[MAX_PATH] = {};
        ExpandEnvironmentStringsW(root, expanded, MAX_PATH);
        std::wstring base = expanded;
        // 常见版次目录：Community/Professional/Enterprise/chanppin/BuildTools
        const wchar_t* ed[] = {L"Community", L"Professional", L"Enterprise", L"BuildTools", L"Preview"};
        for (const wchar_t* e : ed) {
            std::wstring p = base + L"\\\\" + e + L"\\\\DIA SDK\\\\bin\\\\amd64\\\\msdia140.dll";
            candidates.push_back(p);
        }
    }
    candidates.push_back(L"D:\\\\Microsoft Visual Studio\\\\chanppin\\\\DIA SDK\\\\bin\\\\amd64\\\\msdia140.dll");
    candidates.push_back(L"C:\\\\Program Files\\\\Microsoft Visual Studio\\\\2022\\\\Community\\\\DIA SDK\\\\bin\\\\amd64\\\\msdia140.dll");
    for (const auto& c : candidates) {
        if (GetFileAttributesW(c.c_str()) != INVALID_FILE_ATTRIBUTES) return c;
    }
    return L"msdia140.dll";
}

static HRESULT create_dia_source(IDiaDataSource** out) {
    *out = nullptr;
    // 优先：已注册的 COM（常规安装）；失败则免注册加载 msdia140.dll
    if (SUCCEEDED(CoCreateInstance(CLSID_DiaSource, nullptr, CLSCTX_INPROC_SERVER,
                                   IID_IDiaDataSource, reinterpret_cast<void**>(out)))) {
        return S_OK;
    }
    HMODULE dll = LoadLibraryW(find_msdia140().c_str());
    if (!dll) return E_FAIL;
    auto fn = reinterpret_cast<DllGetClassObjectFn>(GetProcAddress(dll, "DllGetClassObject"));
    if (!fn) return E_FAIL;
    IClassFactory* factory = nullptr;
    HRESULT hr = fn(CLSID_DiaSource, IID_IClassFactory, reinterpret_cast<void**>(&factory));
    if (FAILED(hr) || !factory) return hr;
    hr = factory->CreateInstance(nullptr, IID_IDiaDataSource, reinterpret_cast<void**>(out));
    factory->Release();
    return hr;
}

static void print_symbol(IDiaSession* session, DWORD rva) {
    IDiaSymbol* sym = nullptr;
    if (FAILED(session->findSymbolByRVA(rva, SymTagNull, &sym)) || !sym) {
        std::printf("0x%08X  (无符号)\n", rva);
        return;
    }
    BSTR name = nullptr;
    sym->get_name(&name);
    BSTR uname = nullptr;
    sym->get_undecoratedName(&uname);
    std::printf("0x%08X  %ls  [%ls]\n", rva,
                uname && *uname ? uname : (name ? name : L"?"),
                name ? name : L"");
    if (name) SysFreeString(name);
    if (uname) SysFreeString(uname);
    sym->Release();

    IDiaEnumLineNumbers* lines = nullptr;
    if (SUCCEEDED(session->findLinesByRVA(rva, 1, &lines)) && lines) {
        IDiaLineNumber* line = nullptr;
        ULONG got = 0;
        if (SUCCEEDED(lines->Next(1, &line, &got)) && got && line) {
            DWORD ln = 0;
            line->get_lineNumber(&ln);
            IDiaSourceFile* file = nullptr;
            if (SUCCEEDED(line->get_sourceFile(&file)) && file) {
                BSTR fn = nullptr;
                file->get_fileName(&fn);
                std::printf("    行 %u  %ls\n", ln, fn ? fn : L"?");
                if (fn) SysFreeString(fn);
                file->Release();
            }
            line->Release();
        }
        lines->Release();
    }
}

static void find_by_name(IDiaSession* session, const std::string& needle) {
    IDiaSymbol* global = nullptr;
    if (FAILED(session->get_globalScope(&global)) || !global) return;
    std::wstring wneedle(needle.begin(), needle.end());
    IDiaEnumSymbols* en = nullptr;
    global->findChildren(SymTagFunction, nullptr, nsNone, &en);
    if (!en) { global->Release(); return; }
    ULONG got = 0;
    IDiaSymbol* sym = nullptr;
    while (SUCCEEDED(en->Next(1, &sym, &got)) && got && sym) {
        BSTR name = nullptr;
        sym->get_name(&name);
        BSTR uname = nullptr;
        sym->get_undecoratedName(&uname);
        const wchar_t* show = uname && *uname ? uname : (name ? name : L"");
        if (show && wcsstr(show, wneedle.c_str())) {
            DWORD rva = 0;
            sym->get_relativeVirtualAddress(&rva);
            std::printf("0x%08X  %ls\n", rva, show);
        }
        if (name) SysFreeString(name);
        if (uname) SysFreeString(uname);
        sym->Release();
    }
    en->Release();
    global->Release();
}

int wmain(int argc, wchar_t** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "用法: pdb_sym.exe <pdb> <rva...> | pdb_sym.exe <pdb> --find <子串>\n");
        return 1;
    }
    CoInitialize(nullptr);
    IDiaDataSource* src = nullptr;
    if (FAILED(create_dia_source(&src))) { std::fprintf(stderr, "创建 DIA 数据源失败\n"); return 1; }
    if (FAILED(src->loadDataFromPdb(argv[1]))) { std::fprintf(stderr, "加载 PDB 失败: %ls\n", argv[1]); return 1; }
    IDiaSession* session = nullptr;
    if (FAILED(src->openSession(&session))) { std::fprintf(stderr, "打开 DIA 会话失败\n"); return 1; }

    if (wcscmp(argv[2], L"--find") == 0 && argc >= 4) {
        const int len = WideCharToMultiByte(CP_UTF8, 0, argv[3], -1, nullptr, 0, nullptr, nullptr);
        std::string needle(static_cast<size_t>(len - 1), 0);
        if (len > 1) WideCharToMultiByte(CP_UTF8, 0, argv[3], -1, &needle[0], len, nullptr, nullptr);
        find_by_name(session, needle);
    } else {
        for (int i = 2; i < argc; ++i) {
            const DWORD rva = static_cast<DWORD>(wcstoul(argv[i], nullptr, 0));
            print_symbol(session, rva);
        }
    }
    session->Release();
    src->Release();
    CoUninitialize();
    return 0;
}