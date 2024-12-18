#include "a2string.h"
#include <memory>

namespace aria2 {
    std::wstring charset_utf8_to_wchar(const char* src) {
        size_t len = ::MultiByteToWideChar(CP_UTF8, 0, src, -1, nullptr, 0);
        if (len <= 0) {
            abort();
        }
        auto buf = std::make_unique<wchar_t[]>((size_t)len);
        len = ::MultiByteToWideChar(CP_UTF8, 0, src, -1, buf.get(), len);
        if (len <= 0) {
            abort();
        }
        else {
            return buf.get();
        }
    }
    std::wstring charset_utf8_to_wchar(const std::string& src) {
        return charset_utf8_to_wchar(src.c_str());
    }
    std::string charset_wchar_to_utf8(const wchar_t* src) {
        int len = ::WideCharToMultiByte(CP_UTF8, 0, src, -1, nullptr, 0, nullptr, nullptr);
        if (len <= 0) {
            abort();
        }
        auto buf = std::make_unique<char[]>((size_t)len);
        len = ::WideCharToMultiByte(CP_UTF8, 0, src, -1, buf.get(), len, nullptr, nullptr);
        if (len <= 0) {
            abort();
        }
        else {
            return buf.get();
        }
    }
    std::string charset_wchar_to_utf8(const std::wstring& src) {
        return charset_wchar_to_utf8(src.c_str());
    }
}
