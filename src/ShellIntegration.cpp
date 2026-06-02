#include "ShellIntegration.h"

#include <Windows.h>
#include <objbase.h>
#include <shlobj.h>
#include <shobjidl.h>

namespace sundial {
namespace {

bool WriteString(const std::wstring& subkey, const wchar_t* valueName,
                 const std::wstring& value) {
    HKEY hKey = nullptr;
    LSTATUS s = RegCreateKeyExW(HKEY_CURRENT_USER, subkey.c_str(), 0,
                                nullptr, 0, KEY_WRITE, nullptr, &hKey,
                                nullptr);
    if (s != ERROR_SUCCESS) return false;
    const DWORD bytes =
        static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t));
    s = RegSetValueExW(hKey, valueName, 0, REG_SZ,
                       reinterpret_cast<const BYTE*>(value.c_str()), bytes);
    RegCloseKey(hKey);
    return s == ERROR_SUCCESS;
}

// Add an "Open with Sundial" right-click verb for a single extension (".jxr",
// ".avif", ...) under the per-user SystemFileAssociations hive.
void RegisterOpenVerb(const std::wstring& exePath, const std::wstring& ext) {
    const std::wstring quoted = L"\"" + exePath + L"\"";
    const std::wstring command = quoted + L" \"%1\"";
    const std::wstring icon = quoted + L",0";

    const std::wstring verbKey =
        L"Software\\Classes\\SystemFileAssociations\\" + ext +
        L"\\shell\\OpenWithSundial";
    WriteString(verbKey, nullptr, L"Open with Sundial");
    WriteString(verbKey, L"Icon", icon);
    WriteString(verbKey + L"\\command", nullptr, command);
}

}  // namespace

void RegisterImageAssociations(const std::wstring& exePath) {
    // Image formats Sundial can re-open as HDR from Explorer. AVIF goes through
    // libavif (PQ / HLG / gain-map / SDR); JXR through WIC.
    RegisterOpenVerb(exePath, L".jxr");
    RegisterOpenVerb(exePath, L".avif");
}

std::wstring PickFolderDialog(void* owner, const std::wstring& seedFolder) {
    IFileOpenDialog* dlg = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_ALL,
                                IID_PPV_ARGS(&dlg)))) {
        return {};
    }
    DWORD opts = 0;
    dlg->GetOptions(&opts);
    dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_PATHMUSTEXIST |
                    FOS_FORCEFILESYSTEM | FOS_NOCHANGEDIR);
    dlg->SetTitle(L"Choose output folder");
    if (!seedFolder.empty()) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(SHCreateItemFromParsingName(seedFolder.c_str(), nullptr,
                                                  IID_PPV_ARGS(&item)))) {
            dlg->SetFolder(item);
            item->Release();
        }
    }
    std::wstring chosen;
    if (SUCCEEDED(dlg->Show(reinterpret_cast<HWND>(owner)))) {
        IShellItem* result = nullptr;
        if (SUCCEEDED(dlg->GetResult(&result))) {
            PWSTR p = nullptr;
            if (SUCCEEDED(result->GetDisplayName(SIGDN_FILESYSPATH, &p))) {
                chosen = p;
                CoTaskMemFree(p);
            }
            result->Release();
        }
    }
    dlg->Release();
    return chosen;
}

void EnsureStartupShortcutArgs() {
    PWSTR startupDir = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_Startup, 0, nullptr,
                                    &startupDir))) {
        return;
    }
    // Velopack names the shortcut after packTitle ("Sundial").
    const std::wstring lnk = std::wstring(startupDir) + L"\\Sundial.lnk";
    CoTaskMemFree(startupDir);

    if (GetFileAttributesW(lnk.c_str()) == INVALID_FILE_ATTRIBUTES) return;

    IShellLinkW* link = nullptr;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
                                IID_IShellLinkW,
                                reinterpret_cast<void**>(&link)))) {
        return;
    }
    IPersistFile* file = nullptr;
    if (SUCCEEDED(link->QueryInterface(IID_IPersistFile,
                                       reinterpret_cast<void**>(&file)))) {
        if (SUCCEEDED(file->Load(lnk.c_str(), STGM_READWRITE))) {
            wchar_t args[1024] = {};
            link->GetArguments(args, ARRAYSIZE(args));
            // Only rewrite when the flag is absent, so we don't re-touch the
            // file (and churn Explorer) on every launch.
            if (wcsstr(args, L"--startup") == nullptr) {
                if (SUCCEEDED(link->SetArguments(L"--startup"))) {
                    file->Save(lnk.c_str(), TRUE);
                }
            }
        }
        file->Release();
    }
    link->Release();
}

}  // namespace sundial
