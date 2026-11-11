// Windows mpv cplugin: OLE drops are dispatched to our Lua companion before
// mpv can replace the playing video. Only public libmpv and Win32 APIs are used.
#define MPV_CPLUGIN_DYNAMIC_SYM
#include "mpv/client.h"
#include <windows.h>
#include <ole2.h>
#include <shellapi.h>
#include <atomic>
#include <mutex>
#include <string>
#include <vector>

namespace {
mpv_handle* client{};
std::mutex clientMutex;
HHOOK hook{};
std::atomic<HWND> target{};
std::atomic<HWND> installed{};
const UINT installMessage = RegisterWindowMessageW(L"DanmaX.XmlDrop.Install.v1");
std::string utf8(const std::wstring& value) {
    const int n = WideCharToMultiByte(CP_UTF8, 0, value.data(), int(value.size()), nullptr, 0, nullptr, nullptr);
    std::string text(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(), int(value.size()), text.data(), n, nullptr, nullptr);
    return text;
}
void dispatch(const std::vector<std::string>& fields) {
    std::lock_guard lock(clientMutex);
    if (!client) return;
    std::vector<const char*> args{"script-message-to", "danmax_drop"};
    for (const auto& field : fields) args.push_back(field.c_str());
    args.push_back(nullptr);
    // The video thread must never block on the playback core.
    mpv_command_async(client, 0, args.data());
}
std::vector<std::string> files(IDataObject* object) {
    if (!object) return {};
    FORMATETC format{CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
    STGMEDIUM medium{};
    std::vector<std::string> result;
    if (SUCCEEDED(object->GetData(&format, &medium))) {
        const auto drop = static_cast<HDROP>(medium.hGlobal);
        const UINT count = DragQueryFileW(drop, 0xffffffff, nullptr, 0);
        if (count <= 128) for (UINT i = 0; i < count; ++i) {
            const UINT n = DragQueryFileW(drop, i, nullptr, 0);
            if (n > 32767) { result.clear(); break; }
            std::wstring path(n + 1, L'\0'); DragQueryFileW(drop, i, path.data(), n + 1); path.resize(n);
            result.push_back(utf8(path));
        }
        ReleaseStgMedium(&medium); return result;
    }
    format.cfFormat = CF_UNICODETEXT;
    if (SUCCEEDED(object->GetData(&format, &medium))) {
        const auto capacity = GlobalSize(medium.hGlobal) / sizeof(wchar_t);
        if (capacity <= 32768) if (const auto* text = static_cast<const wchar_t*>(GlobalLock(medium.hGlobal))) {
            size_t n = 0; while (n < capacity && text[n]) ++n;
            if (n < capacity && n) result.push_back(utf8(std::wstring(text, n)));
            GlobalUnlock(medium.hGlobal);
        }
        ReleaseStgMedium(&medium);
    }
    return result;
}
class DropTarget final : public IDropTarget {
    LONG refs_{1};
    bool acceptable_{};
  public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (iid != IID_IUnknown && iid != IID_IDropTarget) return E_NOINTERFACE;
        *out = static_cast<IDropTarget*>(this); AddRef(); return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&refs_); }
    ULONG STDMETHODCALLTYPE Release() override {
        const auto count = InterlockedDecrement(&refs_); if (!count) delete this; return count;
    }
    HRESULT STDMETHODCALLTYPE DragEnter(IDataObject* object, DWORD, POINTL, DWORD* effect) override {
        acceptable_ = !files(object).empty(); *effect &= acceptable_ ? DROPEFFECT_COPY : DROPEFFECT_NONE; return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DragOver(DWORD, POINTL, DWORD* effect) override {
        *effect &= acceptable_ ? DROPEFFECT_COPY : DROPEFFECT_NONE; return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DragLeave() override { acceptable_ = false; return S_OK; }
    HRESULT STDMETHODCALLTYPE Drop(IDataObject* object, DWORD keys, POINTL, DWORD* effect) override {
        auto paths = files(object);
        if (paths.empty()) { *effect = DROPEFFECT_NONE; return S_OK; }
        std::vector<std::string> args{"drop", (keys & MK_SHIFT) ? "append" : "replace"};
        args.insert(args.end(), paths.begin(), paths.end()); dispatch(args);
        *effect &= DROPEFFECT_COPY; acceptable_ = false; return S_OK;
    }
};
LRESULT CALLBACK windowHook(int code, WPARAM w, LPARAM l) {
    if (code >= 0) {
        const auto* message = reinterpret_cast<CWPSTRUCT*>(l);
        if (message->message == installMessage && message->hwnd == target && installed.load() != target) {
            // mpv initializes OLE on this thread. Registration must happen here.
            auto* receiver = new DropTarget;
            const auto old = RevokeDragDrop(target);
            if ((SUCCEEDED(old) || old == DRAGDROP_E_NOTREGISTERED) && SUCCEEDED(RegisterDragDrop(target, receiver))) {
                installed = target.load(); dispatch({"native-ready"});
            } else dispatch({"native-error"});
            receiver->Release();
        }
    }
    return CallNextHookEx(nullptr, code, w, l);
}
}
extern "C" __declspec(dllexport) int mpv_open_cplugin(mpv_handle* handle) {
    if (!mpv_command_async || !mpv_wait_event) return -1;
    // A late OLE/Win32 callback must remain executable even during mpv shutdown.
    HMODULE module{};
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                      reinterpret_cast<LPCWSTR>(&windowHook), &module);
    { std::lock_guard lock(clientMutex); if (client) return 0; client = handle; }
    while (mpv_wait_event(handle, .2)->event_id != MPV_EVENT_SHUTDOWN) {
        HWND window{};
        EnumWindows([](HWND candidate, LPARAM p) -> BOOL {
            DWORD pid{}; GetWindowThreadProcessId(candidate, &pid); wchar_t name[64]{};
            GetClassNameW(candidate, name, 64);
            if (pid == GetCurrentProcessId() && wcscmp(name, L"mpv") == 0 && !GetWindow(candidate, GW_OWNER)) {
                *reinterpret_cast<HWND*>(p) = candidate; return FALSE;
            }
            return TRUE;
        }, reinterpret_cast<LPARAM>(&window));
        if (!window || installed.load() == window) continue;
        if (window != target) {
            if (hook) UnhookWindowsHookEx(hook);
            target = window;
            hook = SetWindowsHookExW(WH_CALLWNDPROC, windowHook, module, GetWindowThreadProcessId(window, nullptr));
        }
        if (hook) { DWORD_PTR result{}; SendMessageTimeoutW(window, installMessage, 0, 0, SMTO_ABORTIFHUNG, 100, &result); }
    }
    if (hook) UnhookWindowsHookEx(hook);
    { std::lock_guard lock(clientMutex); client = nullptr; }
    return 0;
}
