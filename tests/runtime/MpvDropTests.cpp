// Exercise the actual OLE receiver with a real CF_HDROP data object, without
// sending mouse/keyboard input or touching a user's player.
#include "../../integrations/mpv-xml/DropTarget.cpp"
#include <cstdio>
#include <cstring>
#include <shlobj.h>

namespace {
std::vector<std::string> captured;
int capture(mpv_handle*, uint64_t, const char** args) {
    captured.clear(); for (int i = 0; args[i]; ++i) captured.emplace_back(args[i]); return 0;
}
class FileData final : public IDataObject {
  public:
    std::vector<std::wstring> paths;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void**) override { return E_NOINTERFACE; }
    ULONG STDMETHODCALLTYPE AddRef() override { return 1; }
    ULONG STDMETHODCALLTYPE Release() override { return 1; }
    HRESULT STDMETHODCALLTYPE GetData(FORMATETC* f, STGMEDIUM* out) override {
        if (f->cfFormat != CF_HDROP) return DV_E_FORMATETC;
        size_t chars = 1; for (const auto& p : paths) chars += p.size() + 1;
        auto memory = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, sizeof(DROPFILES) + chars * sizeof(wchar_t));
        auto* value = static_cast<DROPFILES*>(GlobalLock(memory)); value->pFiles = sizeof(DROPFILES); value->fWide = TRUE;
        auto* text = reinterpret_cast<wchar_t*>(value + 1);
        for (const auto& p : paths) { std::memcpy(text, p.c_str(), (p.size() + 1) * sizeof(wchar_t)); text += p.size() + 1; }
        GlobalUnlock(memory); out->tymed = TYMED_HGLOBAL; out->hGlobal = memory; out->pUnkForRelease = nullptr; return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetDataHere(FORMATETC*, STGMEDIUM*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE QueryGetData(FORMATETC*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE GetCanonicalFormatEtc(FORMATETC*, FORMATETC*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetData(FORMATETC*, STGMEDIUM*, BOOL) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE EnumFormatEtc(DWORD, IEnumFORMATETC**) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE DAdvise(FORMATETC*, DWORD, IAdviseSink*, DWORD*) override { return OLE_E_ADVISENOTSUPPORTED; }
    HRESULT STDMETHODCALLTYPE DUnadvise(DWORD) override { return OLE_E_ADVISENOTSUPPORTED; }
    HRESULT STDMETHODCALLTYPE EnumDAdvise(IEnumSTATDATA**) override { return OLE_E_ADVISENOTSUPPORTED; }
};
}
int main() {
    pfn_mpv_command_async = capture; client = reinterpret_cast<mpv_handle*>(1);
    FileData data; data.paths = {L"C:\\中文 & 空格\\弹幕.XML"};
    DropTarget receiver; DWORD effect = DROPEFFECT_COPY | DROPEFFECT_MOVE;
    if (receiver.DragEnter(&data, 0, {}, &effect) != S_OK || effect != DROPEFFECT_COPY) return 1;
    receiver.Drop(&data, 0, {}, &effect);
    if (captured != std::vector<std::string>{"script-message-to", "danmax_drop", "drop", "replace", utf8(data.paths[0])}) return 2;
    data.paths.push_back(L"C:\\movie.mkv"); effect = DROPEFFECT_COPY;
    receiver.Drop(&data, MK_SHIFT, {}, &effect);
    if (captured.size() != 6 || captured[3] != "append" || captured[5] != "C:\\movie.mkv") return 3;
    data.paths.assign(129, L"C:\\too-many.xml"); effect = DROPEFFECT_COPY;
    receiver.Drop(&data, 0, {}, &effect); if (effect != DROPEFFECT_NONE) return 4;
    std::puts("OLE Unicode paths, replace/append, multiple files and bounds passed"); return 0;
}
