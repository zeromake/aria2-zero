#include "a2io.h"

#ifdef _WIN32

#include <wchar.h>
#include <string>
#include <vector>
#ifdef HAVE_SYS_UTIME_H
#include <sys/utime.h>
#endif // HAVE_SYS_UTIME_H

#define isWindowsDeviceRoot(b) ((b >= L'A' && b <= L'Z') || (b >= L'a' && b <= L'z'))

static inline bool isAbsolute(const std::wstring path) {
  return path.size() > 2 && isWindowsDeviceRoot(path[0]) && path[1] == L':';
}

static std::wstring getCurrentPath() {
  auto size = GetCurrentDirectoryW(0, NULL);
  std::vector<wchar_t> buf(size);
  GetCurrentDirectoryW(size, buf.data());
  auto currentPath = std::wstring(buf.data());
  while (currentPath.back() == L'\\') {
    currentPath.pop_back();
  }
  return std::move(currentPath);
}

const std::wstring toNamespacedPath(const wchar_t *path) {
  std::wstring originPath(path);
  for (std::wstring::iterator i = originPath.begin(), eoi = originPath.end(); i != eoi; ++i) {
    if (*i == L'/') {
      *i = L'\\';
    }
  }
  if (!isAbsolute(originPath)) {
    static const std::wstring currentPath = getCurrentPath();
    originPath = originPath[0] == L'\\' ? currentPath + originPath : currentPath + L"\\" + originPath;
  }
  if (originPath.size() >= MAX_PATH) {
    originPath = L"\\\\?\\" + originPath;
  }
  return std::move(originPath);
}

int a2stat(const wchar_t* path, a2_struct_stat* buf) {
  return _wstat64(toNamespacedPath(path).c_str(), buf);
}

int a2mkdir(const wchar_t *path, int openMode) {
  return _wmkdir(toNamespacedPath(path).c_str());
}

int a2unlink(const wchar_t *path) {
  return _wunlink(toNamespacedPath(path).c_str());
}

int a2rmdir(const wchar_t *path) {
  return _wrmdir(toNamespacedPath(path).c_str());
}

int a2open(const wchar_t *path, int flags, int mode) {
  return _wsopen(toNamespacedPath(path).c_str(), flags, _SH_DENYNO, mode);
}

FILE* a2fopen(const wchar_t *path, const wchar_t *mode) {
  return _wfsopen(toNamespacedPath(path).c_str(), mode, _SH_DENYNO);
}

int a2rename(const wchar_t *oldpath, const wchar_t *newpath) {
  BOOL ok = MoveFileExW(
    toNamespacedPath(oldpath).c_str(),
    toNamespacedPath(newpath).c_str(),
    MOVEFILE_COPY_ALLOWED | MOVEFILE_REPLACE_EXISTING);
  return ok ? 0 : -1;
}

wchar_t* a2getcwd(wchar_t* buf, int size) {
  if (GetCurrentDirectoryW(size, buf)) {
    return buf;
  }
  return NULL;
}

HANDLE a2CreateFileW(
    const wchar_t* lpFileName,
    DWORD dwDesiredAccess,
    DWORD dwShareMode,
    LPSECURITY_ATTRIBUTES lpSecurityAttributes,
    DWORD dwCreationDisposition,
    DWORD dwFlagsAndAttributes,
    HANDLE hTemplateFile) {
  return CreateFileW(
    toNamespacedPath(lpFileName).c_str(),
    dwDesiredAccess,
    dwShareMode,
    lpSecurityAttributes,
    dwCreationDisposition,
    dwFlagsAndAttributes,
    hTemplateFile);
}

#ifdef HAVE_SYS_UTIME_H
int a2utime(const wchar_t* path, a2utimbuf* t) {
  return _wutime64(toNamespacedPath(path).c_str(), t);
}
#endif

#endif // _WIN32