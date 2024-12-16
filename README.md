# aria2-zero - The ultra fast download utility


## Disclaimer

This program comes with no warranty.
You must use this program at your own risk.


## Features

- 使用 xmake 一键编译
- 支持 msvc 编译
- 支持 windows 超长路径（MAX_PATH 一般 260 个字符，拼接了 `\\?\` 应该是支持 32767 个字符？）
- 对 http 头做了简单的排序
- 统一使用 libressl 做 sftp 和 hashcheck 支持（internal 貌似在 windows 下有 bug）
- 支持新版的 Metalink v3 命名空间
- 支持默认跳过 bt 种子里的 `_____padding_file_` 文件，不会创建，也不会写入磁盘（但是依旧会下载，这个 bt 里是用来填充另一个文件的，不能不下载）
- 应用了 [#2209](https://github.com/aria2/aria2/pull/2209) 补丁，未认证时没有正确回收 socket

## ChangeLog

- [x] 改为 xmake 工具编译
- [x] 支持 msvc 编译
- [x] 清理掉 makefile 支持
- [x] 把 launchpad 上的 po 直接放到项目里
- [x] 支持 ci 编译
    + [x] windows 的 x64,x86,arm64
    + [x] osx 的 x86_64,arm64,universal
    + [x] linux 的 x86_64
- [ ] 支持 windows 下的 po 翻译文件加载
- [ ] 从上游拉一些值得修复的问题
    + [x] [findFirstDiskWriterEntry 可能越界](https://github.com/aria2/aria2/issues/2216)
    + [x] [Metalink v3 支持新命名空间](https://github.com/aria2/aria2/issues/2267)
    + [x] [对常规 http 头做强制排序](https://github.com/aria2/aria2/issues/2272)
    + [ ] [CDN 地址经常是重定向后有过期时间支持重试和分片下载时从源头链接开始](https://github.com/aria2/aria2/issues/2197)
    + [ ] [follow-torrent 不起作用](https://github.com/aria2/aria2/issues/2196)
    + [ ] [支持 m3u8](https://github.com/aria2/aria2/issues/2164)
    + [ ] [支持种子文件过滤](https://github.com/aria2/aria2/issues/843)
    + [ ] [应该有选项跳过 Range 头添加](https://github.com/aria2/aria2/issues/2051)
    + [ ] [磁力链添加任务会奔溃，看起来是 std::array 创建导致的，gcc 12](https://github.com/aria2/aria2/issues/2064)
    + [ ] [空间不足，创建 bt 任务失败无法通过恢复按钮重建](https://github.com/aria2/aria2/issues/2043)
    + [ ] [支持远端checksum](https://github.com/aria2/aria2/issues/2076)
    + [ ] [特殊网盘拒绝下载](https://github.com/aria2/aria2/issues/2042)
    + [ ] [支持 ftp 递归下载](https://github.com/aria2/aria2/issues/2036)
    + [ ] [支持下载前重命名](https://github.com/aria2/aria2/issues/2034)
    + [ ] [下载 bt 选择了文件也会预创建没有选择的文件](https://github.com/aria2/aria2/issues/2032)
    + [ ] [看上去像是 url 太多或者是下载列表太大一次性分配的内存太多](https://github.com/aria2/aria2/issues/2025)
    + [ ] [sftp 支持端口设置](https://github.com/aria2/aria2/issues/2022)
    + [ ] [兼容文件列表读取支持 UTF-8 BOM](https://github.com/aria2/aria2/issues/2021)
    + [ ] [支持自定义 http range 分片大小](https://github.com/aria2/aria2/issues/2017)
    + [ ] [排布任务好像有问题](https://github.com/aria2/aria2/issues/2012)
    + [ ] [强制重新开始而不是继续](https://github.com/aria2/aria2/issues/2010)
    + [ ] [高精度超时](https://github.com/aria2/aria2/issues/2002)
    + [x] windows 下支持超过 255 字节的路径，额外支持超大路径 [1](https://github.com/aria2/aria2/issues/1997),[2](https://github.com/aria2/aria2/issues/1981),[3](https://github.com/aria2/aria2/issues/1070), [4](https://github.com/imfile-io/imfile-desktop/issues/56)
    + [ ] Range 拼接错误 [1](https://github.com/aria2/aria2/issues/1971), [2](https://github.com/aria2/aria2/issues/1344#issuecomment-1570701152), [3](https://github.com/aria2/aria2/pull/1587), [4]()
    + [ ] [进度](https://github.com/aria2/aria2/issues?page=8&q=is%3Aissue+is%3Aopen),(#1971)[https://github.com/aria2/aria2/issues/1971]
- [ ] 标记一些有报告，但是没有问题的 issues
    - [无法使用 ip 直连下载内网文件](https://github.com/aria2/aria2/issues/2049)
- [ ] 检查其它 fork 是否有好的修改
    - [ ] [Aria2-Pro-Core](https://github.com/P3TERX/Aria2-Pro-Core)

## 参考

- [m3u8 直播原理](https://simonzhangcn.github.io/blog-src/dist/live/#%E4%B8%BB%E6%B5%81%E7%9B%B4%E6%92%AD%E5%8D%8F%E8%AE%AE)
