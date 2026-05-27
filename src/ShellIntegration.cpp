#include "ShellIntegration.h"

#include <Windows.h>

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

}  // namespace

void RegisterJxrAssociation(const std::wstring& exePath) {
    const std::wstring quoted = L"\"" + exePath + L"\"";
    const std::wstring command = quoted + L" \"%1\"";
    const std::wstring icon = quoted + L",0";

    const std::wstring verbKey =
        L"Software\\Classes\\SystemFileAssociations\\.jxr\\shell"
        L"\\OpenWithSundial";
    WriteString(verbKey, nullptr, L"Open with Sundial");
    WriteString(verbKey, L"Icon", icon);
    WriteString(verbKey + L"\\command", nullptr, command);
}

}  // namespace sundial
