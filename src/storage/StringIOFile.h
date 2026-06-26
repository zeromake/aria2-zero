#ifndef D_STRING_IO_FILE_H
#define D_STRING_IO_FILE_H

#include "IOFile.h"

#include <cstdarg>
#include <cstdio>
#include <string>

namespace aria2 {

// 将所有 write 输出捕获到内存 std::string，用于序列化快照。
// 不支持 read/gets 操作。
class StringIOFile : public IOFile {
public:
  StringIOFile() = default;

  std::string& str() { return buf_; }
  const std::string& str() const { return buf_; }

protected:
  virtual size_t onRead(void* ptr, size_t count) CXX11_OVERRIDE { return 0; }

  virtual size_t onWrite(const void* ptr, size_t count) CXX11_OVERRIDE
  {
    buf_.append(static_cast<const char*>(ptr), count);
    return count;
  }

  virtual char* onGets(char* s, int size) CXX11_OVERRIDE { return nullptr; }

  virtual int onVprintf(const char* format, va_list va) CXX11_OVERRIDE
  {
    va_list va2;
    va_copy(va2, va);
    int n = vsnprintf(nullptr, 0, format, va);
    if (n < 0) {
      va_end(va2);
      return -1;
    }
    size_t pos = buf_.size();
    buf_.resize(pos + n);
    vsnprintf(&buf_[pos], n + 1, format, va2);
    va_end(va2);
    return n;
  }

  virtual int onFlush() CXX11_OVERRIDE { return 0; }
  virtual int onClose() CXX11_OVERRIDE { return 0; }
  virtual bool onSupportsColor() CXX11_OVERRIDE { return false; }
  virtual bool isError() const CXX11_OVERRIDE { return false; }
  virtual bool isEOF() const CXX11_OVERRIDE { return false; }
  virtual bool isOpen() const CXX11_OVERRIDE { return true; }

private:
  std::string buf_;
};

} // namespace aria2

#endif // D_STRING_IO_FILE_H
