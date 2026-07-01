# Range 拼接错误分析报告

## 相关 Issues

- [#1971 - Strange Range Request for MP4 Files](https://github.com/aria2/aria2/issues/1971)
- [#1344 - Invalid range header results (errorCode=8)](https://github.com/aria2/aria2/issues/1344)
- [#1587 - Chunked Transfer-Encoding 分段下载支持 PR](https://github.com/aria2/aria2/pull/1587)

---

## 问题概述

aria2 在多分段下载场景下，存在 HTTP Range 头构建错误以及请求-响应 Range 验证不一致的问题，导致 `errorCode=8 "Invalid range header"` 错误。核心原因是 Range 头的 **发送值** 与 **内部校验值** 使用了不同的计算路径。

---

## 代码分析

### 1. Range 头构建流程

Range 头在 `HttpRequest::createRequest()` (`src/network/HttpRequest.cc`) 中构建：

```cpp
if (segment_ && segment_->getLength() > 0 &&
    (request_->isPipeliningEnabled() || getStartByte() > 0 ||
     getEndByte() > 0)) {
  std::string rangeHeader = "bytes=";
  rangeHeader += util::uitos(getStartByte());   // 无符号整数转字符串
  rangeHeader += '-';
  if (request_->isPipeliningEnabled() || getEndByte() > 0) {
    rangeHeader += util::itos(getEndByte());     // 有符号整数转字符串
  }
  builtinHds.emplace_back("Range:", rangeHeader);
}
```

### 2. 两个关键方法：getStartByte() vs getEndByte()

**`getStartByte()`** (`HttpRequest.cc:90-97`)：
```cpp
int64_t HttpRequest::getStartByte() const {
  if (!segment_) return 0;
  return fileEntry_->gtoloff(segment_->getPositionToWrite());
  //                         ^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
  //                         已写入位置（断点续传）
}
```

**`getEndByte()`** (`HttpRequest.cc:99-113`)：
```cpp
int64_t HttpRequest::getEndByte() const {
  if (!segment_ || !request_) return 0;
  if (request_->isPipeliningEnabled()) {
    auto endByte = fileEntry_->gtoloff(segment_->getPosition() +
                                       segment_->getLength() - 1);
    //                                 ^^^^^^^^^^^^^^
    //                                 segment 起始位置（非已写入位置）
    return std::min(endByte, fileEntry_->getLength() - 1);
  }
  if (endOffsetOverride_ > 0) {
    return endOffsetOverride_ - 1;
  }
  return 0;
}
```

---

## Bug #1: getStartByte 使用 getPositionToWrite，Range 头与验证不一致

### 问题描述

`getStartByte()` 返回的是 `segment_->getPositionToWrite()`（已写入位置），而非 `segment_->getPosition()`（segment 起始位置）。

在断点续传场景下：
- **首次请求**：`getPositionToWrite() == getPosition()`，Range 请求正确
- **续传请求**：`getPositionToWrite() > getPosition()`，Range 请求从已写位置开始

`isRangeSatisfied` 中用同一个 `getStartByte()` 去对比响应的 `startByte`，此时如果 segment 内部状态在请求发出后和响应返回前发生变化（例如 writtenLength 被更新），验证就会失败。

### Issue #1971 的体现

错误日志：
```
Range: bytes=1090977792-      (实际发送的请求)
Invalid range header.
  Request:  2261507-1093239298/1093239299   (验证时计算的值)
  Response: 1090977792-1093239298/1093239299 (服务器返回的值)
```

请求发出时 `getStartByte() = 1090977792`，但验证时 `getStartByte()` 返回了 `2261507`。说明在请求发出到响应返回之间，`segment_->getPositionToWrite()` 发生了变化（writtenLength 被重置或更新）。

### 根因

`getStartByte()` 是一个**动态计算值**，依赖 segment 的 `writtenLength_` 状态。而 Range 头在 `createRequest()` 时刻一次性生成为字符串，之后 segment 状态可能改变。但验证时又重新调用 `getStartByte()`，导致两次调用的结果不同。

### 修复（已实现）

在 `HttpRequest` 中增加 `sentStartByte_`、`sentEndByte_`、`rangeSent_` 三个字段，在 `createRequest()` 构建 Range 头时缓存实际发送的值：

```cpp
// HttpRequest.h — 新增字段
int64_t sentStartByte_;
int64_t sentEndByte_;
bool rangeSent_;

// HttpRequest.cc — createRequest() 中缓存发送值
sentStartByte_ = getStartByte();
sentEndByte_ = getEndByte();
rangeSent_ = true;
// 后续使用 sentStartByte_/sentEndByte_ 构建 Range 头字符串

// HttpRequest.cc — isRangeSatisfied() 优先使用缓存值
auto startByte = rangeSent_ ? sentStartByte_ : getStartByte();
auto endByte = rangeSent_ ? sentEndByte_ : getEndByte();

// HttpRequest.cc — getRange() 同样优先使用缓存值
if (rangeSent_) {
  return Range(sentStartByte_, sentEndByte_, fileEntry_->getLength());
}
```

同时修改 `HttpResponse::validateResponse()` 中的错误报告，使用 `httpRequest_->getRange()` 获取缓存的请求 Range（而非重新调用 `getStartByte()`/`getEndByte()`），确保错误消息中的 "Request" 部分与实际发送的 Range 头一致。

---

## Bug #2: entityLength 不一致导致误判

### 问题描述

原始 `isRangeSatisfied` 的最后一个条件：
```cpp
fileEntry_->getLength() == 0 || fileEntry_->getLength() == range.entityLength
```

这要求本地已知的文件大小与服务器返回的 entityLength 完全一致。但在以下场景中会失败：

1. **服务器动态内容**：某些服务器（如 CDN、API 网关）可能在不同时刻返回略有不同的 Content-Length
2. **首次请求和续传请求之间文件变化**：Issue #1344 中 `Request: 0-96669988/96669989` vs `Response: 0-96669988/96670038`，服务器报告的文件大小从 `96669989` 变为 `96670038`

### 根因

aria2 将首次请求获得的 `entityLength` 作为 `fileEntry_->getLength()` 缓存，后续所有响应都必须与之一致。但对于文件大小不稳定的源（动态文件、不一致的镜像源），这个严格校验会导致下载失败。

### 修复（已实现）

从 `isRangeSatisfied()` 中移除 entityLength 严格比较。entityLength 变化属于文件级问题（文件被修改），不应在 Range 验证层处理，由上层 `HttpResponseCommand` 根据 entityLength 变化决定是否需要重新下载：

```cpp
bool HttpRequest::isRangeSatisfied(const Range& range) const {
  if (!segment_) return true;
  auto startByte = rangeSent_ ? sentStartByte_ : getStartByte();
  auto endByte = rangeSent_ ? sentEndByte_ : getEndByte();
  return startByte == range.startByte &&
         (endByte == 0 || endByte == range.endByte);
}
```

---

## Bug #3: Transfer-Encoding 与 Content-Range 互斥处理

### 问题描述

原始代码中存在两层错误处理：

1. `HttpHeaderProcessor.cc`：Transfer-Encoding 存在时删除 Content-Range
```cpp
if (result_->defined(HttpHeader::TRANSFER_ENCODING)) {
    result_->remove(HttpHeader::CONTENT_LENGTH);
    result_->remove(HttpHeader::CONTENT_RANGE);  // ← 丢弃了 Content-Range
}
```

2. `HttpResponse::validateResponse()`：Transfer-Encoding 存在时跳过 Range 验证
```cpp
if (!httpHeader_->defined(HttpHeader::TRANSFER_ENCODING)) {
    // 只有非 chunked 时才校验 Range
}
```

这导致使用 chunked transfer-encoding 的服务器（如 Google Drive）：
1. Content-Range 被删除 → `httpHeader_->getRange()` 回退到解析 Content-Length
2. 即使 transfer-encoding 存在也跳过了 Range 校验
3. 无法进行分段下载

### 修复（已实现）

参考 PR #1587 的核心理解：**Transfer-Encoding 描述的是传输编码（分块传输），Content-Range 描述的是资源位置（分段范围），两者工作在不同层级，并不互斥（RFC 7233）。**

1. `HttpHeaderProcessor.cc`：仅删除 Content-Length，**保留 Content-Range**：
```cpp
// RFC 7230: if both transfer-encoding and content-length are
// present, transfer-encoding overrides content-length.
// Content-Range is kept because it operates at a different layer
// (resource position) than transfer-encoding (wire encoding).
if (result_->defined(HttpHeader::TRANSFER_ENCODING)) {
  result_->remove(HttpHeader::CONTENT_LENGTH);
}
```

2. `HttpResponse::validateResponse()`：移除 Transfer-Encoding 保护，无论是否有 Transfer-Encoding 都进行 Range 验证：
```cpp
case 200:
case 206:
{
  auto responseRange = httpHeader_->getRange();
  if (!httpRequest_->isRangeSatisfied(responseRange)) {
    auto requestRange = httpRequest_->getRange();
    throw DL_ABORT_EX2(
        fmt(EX_INVALID_RANGE_HEADER, requestRange.startByte,
            requestRange.endByte, requestRange.entityLength,
            responseRange.startByte, responseRange.endByte,
            responseRange.entityLength),
        error_code::CANNOT_RESUME);
  }
  return;
}
```

---

## Bug #4: endOffsetOverride 计算中的潜在溢出

### 问题描述

`HttpRequestCommand.cc` 中计算 endOffset：

```cpp
size_t nextIndex = getPieceStorage()->getNextUsedIndex(segment->getIndex());
endOffset = std::min(getFileEntry()->getLength(),
                     getFileEntry()->gtoloff(
                         static_cast<int64_t>(segment->getSegmentLength()) *
                         nextIndex));
```

`segment->getSegmentLength()` 返回 `int32_t`（pieceLength_），`nextIndex` 是 `size_t`。乘法结果可能超过文件实际范围，导致 `gtoloff` 中的 `assert(offset_ <= goff)` 触发断言失败。

### 修复（已实现）

将 `nextIndex` 也显式转为 `int64_t`，并在调用 `gtoloff` 前将全局偏移 clamp 到 `getLastOffset()` 范围内：

```cpp
size_t nextIndex =
    getPieceStorage()->getNextUsedIndex(segment->getIndex());
auto globalOffset =
    static_cast<int64_t>(segment->getSegmentLength()) * static_cast<int64_t>(nextIndex);
globalOffset = std::min(globalOffset, getFileEntry()->getLastOffset());
endOffset =
    std::min(getFileEntry()->getLength(),
             getFileEntry()->gtoloff(globalOffset));
```

---

## 影响范围

| 场景 | 影响 | 严重度 | 状态 |
|------|------|--------|------|
| 大文件多分段断点续传 | segment 状态变化导致验证失败 | 高 | ✅ 已修复 |
| 服务器 entityLength 不一致 | 下载失败无法恢复 | 中 | ✅ 已修复 |
| chunked + Range 服务器 | 无法分段下载 | 中 | ✅ 已修复 |
| 超大文件 endOffset 计算 | 可能断言失败 | 低 | ✅ 已修复 |

## 修改文件

| 文件 | 改动 |
|------|------|
| `src/network/HttpRequest.h` | 新增 `sentStartByte_`/`sentEndByte_`/`rangeSent_` 字段 |
| `src/network/HttpRequest.cc` | 缓存发送的 Range 值；`isRangeSatisfied` 使用缓存值、移除 entityLength 校验；`getRange` 优先返回缓存值 |
| `src/network/HttpResponse.cc` | 移除 Transfer-Encoding 保护；错误报告使用 `getRange()` 缓存值 |
| `src/network/HttpHeaderProcessor.cc` | 不再删除 Content-Range（仅删除 Content-Length） |
| `src/network/HttpRequestCommand.cc` | endOffset 计算增加 `int64_t` 转换和 `getLastOffset()` clamp |
| `test/HttpRequestTest.cc` | 适配 entityLength 校验放宽后的断言变化 |
| `test/HttpResponseTest.cc` | chunked 测试改为：正确 Content-Range 通过、错误 Content-Range 抛异常 |
| `test/HttpHeaderProcessorTest.cc` | 适配 Content-Range 保留行为 |
