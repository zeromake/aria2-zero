#ifndef D_A2STRING_H
#define D_A2STRING_H

#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif

#include <string>

namespace aria2 {
std::wstring charset_utf8_to_wchar(const char* src);
std::wstring charset_utf8_to_wchar(const std::string& src);
std::string charset_wchar_to_utf8(const wchar_t* src);
std::string charset_wchar_to_utf8(const std::wstring& src);
} // namespace aria2

#endif // D_A2STRING_H
