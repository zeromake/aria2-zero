includes("@builtin/check")
includes("@builtin/xpack")
add_rules("mode.debug", "mode.release")

option("uv")
    set_default(false)
    set_showmenu(true)
    set_description("Use external uv library")
option_end()

option("ssl_external")
    set_default(false)
    set_showmenu(true)
    set_description("Use external ssl library")
option_end()

option("use_quictls")
    set_default(true)
    set_showmenu(true)
    set_description("Use external use_quictls library")
option_end()

option("with_breakpad")
    set_default(false)
    set_showmenu(true)
    set_description("Use external with_breakpad library")
option_end()


option("with_static")
    set_default(false)
    set_showmenu(true)
    set_description("Use static glibc build")
option_end()

option("unit")
    set_default(false)
    set_showmenu(true)
    set_description("Build unit test")
option_end()

local ssl_external = get_config("ssl_external") or is_plat("linux", "android")

includes("package.lua")
set_languages("c++14")
set_encodings("utf-8")
set_license("GPL-2.0")
set_rundir(".")
add_defines("CXX11_OVERRIDE=override")
set_configdir("$(buildir)/config")
add_includedirs("$(buildir)/config")
if is_plat("windows") then
    add_cxxflags("/EHsc")
end

local common_headers = {
    "argz.h",
    "arpa/inet.h",
    "fcntl.h",
    "float.h",
    "inttypes.h",
    "langinfo.h",
    "libintl.h",
    "limits.h",
    "libgen.h",
    "locale.h",
    "malloc.h",
    "math.h",
    "memory.h",
    "netdb.h",
    "netinet/in.h",
    "netinet/tcp.h",
    "poll.h",
    "port.h",
    "signal.h",
    "stddef.h",
    "stdint.h",
    "stdio.h",
    "stdio_ext.h",
    "stdlib.h",
    "string.h",
    "strings.h",
    "sys/epoll.h",
    "sys/event.h",
    "sys/ioctl.h",
    "sys/mman.h",
    "sys/param.h",
    "sys/resource.h",
    "sys/stat.h",
    "sys/signal.h",
    "sys/socket.h",
    "sys/time.h",
    "sys/utime.h",
    "sys/types.h",
    "sys/uio.h",
    "sys/utsname.h",
    "termios.h",
    "time.h",
    "unistd.h",
    "utime.h",
    "wchar.h",
    "ifaddrs.h",
    "pwd.h",
    "stdbool.h",
    "pthread.h",
    "getopt.h",

    "windows.h",
    "winsock2.h",
    "ws2tcpip.h",
    {"mmsystem.h", {"windows.h", "mmsystem.h"}},
    "io.h",
    {"iphlpapi.h", {"winsock2.h", "windows.h", "ws2tcpip.h", "iphlpapi.h"}},
    {"winioctl.h", {"windows.h", "winioctl.h"}},
    "share.h",
    "sys/utime.h",
    {"wincrypt.h", {"windows.h", "wincrypt.h"}},
}

for _, common_header in ipairs(common_headers) do
    local k = common_header
    local v = common_header
    if type(common_header) == 'table' then
        k = common_header[1]
        v = common_header[2]
    end
    local name = 'HAVE_'..k:gsub("/", "_"):gsub("%.", "_"):gsub("-", "_"):upper()
    configvar_check_cincludes(name, v)
end

if is_plat("windows", "mingw") then
    configvar_check_csnippets("HAVE_SECURITY_H", [[
#ifndef SECURITY_WIN32
#define SECURITY_WIN32 1
#endif
#include <windows.h>
#include <security.h>
]])
end

set_configvar("ENABLE_METALINK", 1)
set_configvar("ENABLE_XML_RPC", 1)
set_configvar("ENABLE_BITTORRENT", 1)
set_configvar("ENABLE_SSL", 1)
set_configvar("HAVE_LIBCARES", 1)
set_configvar("HAVE_LIBSSH2", 1)
set_configvar("HAVE_OPENSSL", 1)
set_configvar("HAVE_EVP_SHA224", 1)
set_configvar("HAVE_EVP_SHA256", 1)
set_configvar("HAVE_EVP_SHA384", 1)
set_configvar("HAVE_EVP_SHA512", 1)

set_configvar("HAVE_SQLITE3", 1)
set_configvar("HAVE_SQLITE3_OPEN_V2", 1)
set_configvar("HAVE_LIBEXPAT", 1)
set_configvar("HAVE_ZLIB", 1)
set_configvar("HAVE_GZBUFFER", 1)
set_configvar("HAVE_GZSETPARAMS", 1)
set_configvar("ENABLE_WEBSOCKET", 1)
set_configvar("ENABLE_ASYNC_DNS", 1)
set_configvar("USE_INTERNAL_MD", 1)
set_configvar("ENABLE_COMMONAD_DELTA_DEBUG", 1)
set_configvar("ENABLE_NLS", 1)

if is_plat("windows", "mingw") then
    add_defines("_POSIX_C_SOURCE=1")
    set_configvar("SCHANNEL_USE_BLACKLISTS", 1)
else
    add_defines("_GNU_SOURCE=1")
    set_configvar("ENABLE_PTHREAD", 1)
end
local PROJECT_NAME = "aria2-zero"
local PROJECT_VERSION = os.getenv("VERSION") or "1.37.0-development"
local PACKAGE_URL = "https://aria2.github.io"
local PACKAGE_BUGREPORT = "https://github.com/zeromake/aria2-zero/issues"

set_configvar("PACKAGE", PROJECT_NAME)
set_configvar("PACKAGE_NAME", PROJECT_NAME)
set_configvar("PACKAGE_STRING", PROJECT_NAME .. " " .. PROJECT_VERSION)
set_configvar("PACKAGE_TARNAME", PROJECT_NAME)
set_configvar("PACKAGE_URL", PACKAGE_URL)
set_configvar("PACKAGE_BUGREPORT", PACKAGE_BUGREPORT)
set_configvar("PACKAGE_VERSION", PROJECT_VERSION)
set_configvar("VERSION", PROJECT_VERSION)

local network_include = {}
if is_plat("windows", "mingw") then
    network_include = {"winsock2.h", "windows.h", "ws2tcpip.h"}
else
    network_include = {"sys/types.h", "sys/socket.h", "netdb.h"}
    configvar_check_ctypes("HAVE_SSIZE_T", "ssize_t", {includes = {"sys/types.h"}})
end

configvar_check_cfuncs("HAVE_GETADDRINFO", "getaddrinfo", {includes = network_include})
configvar_check_ctypes("HAVE_A2_STRUCT_TIMESPEC", "struct timespec", {includes = {"time.h"}})
configvar_check_cfuncs("HAVE_SLEEP", "sleep", {includes = {"unistd.h"}})
configvar_check_cfuncs("HAVE_USLEEP", "usleep", {includes = {"unistd.h"}})
configvar_check_cfuncs("HAVE_LOCALTIME_R", "localtime_r", {includes = {"time.h"}})
configvar_check_cfuncs("HAVE_GETTIMEOFDAY", "gettimeofday", {includes = {"sys/time.h"}})
configvar_check_cfuncs("HAVE_STRPTIME", "strptime", {includes = {"time.h"}})
configvar_check_cfuncs("HAVE_TIMEGM", "timegm", {includes = {"time.h"}})
configvar_check_cfuncs("HAVE_ASCTIME_R", "asctime_r", {includes = {"time.h"}})
configvar_check_cfuncs("HAVE_MMAP", "mmap", {includes = {"sys/mman.h"}})
configvar_check_cfuncs("HAVE_BASENAME", "basename", {includes = {"libgen.h"}})
configvar_check_cfuncs("HAVE_GAI_STRERROR", "gai_strerror", {includes = network_include})
configvar_check_cfuncs("HAVE_STRCASECMP", "strcasecmp", {includes = "strings.h"})
configvar_check_cfuncs("HAVE_STRNCASECMP", "strncasecmp", {includes = "strings.h"})
configvar_check_cfuncs("HAVE_FALLOCATE", "fallocate",{includes = "fcntl.h"})
configvar_check_cfuncs("HAVE_POSIX_FALLOCATE", "posix_fallocate",{includes = "fcntl.h"})

configvar_check_ctypes("HAVE___INT64", "__int64")
configvar_check_ctypes("HAVE_LONG_LONG", "long long")

local sourceDirs = {
    "src/core",
    "src/crypto/common",
    "src/network",
    "src/parser",
    "src/parser/json",
    "src/parser/xml",
    "src/protocol",
    "src/protocol/announce",
    "src/protocol/bt",
    "src/protocol/lpd",
    "src/protocol/metalink",
    "src/protocol/peer",
    "src/protocol/piece",
    "src/protocol/utm",
    "src/protocol/ws",
    "src/rpc",
    "src/storage",
    "src/stream",
    "src/parser",
    "src/util",
}

target("aria2")
    set_kind("$(kind)")
    add_files("deps/wslay/lib/*.c")
    if is_mode("release") and get_config("with_breakpad") then
        if is_plat("windows") then
            add_cxflags("/Zi", "/FS", "/Fd$(buildir)\\$(plat)\\$(arch)\\release\\aria2.pdb")
            add_ldflags("/DEBUG", "/PDB:$(buildir)\\$(plat)\\$(arch)\\release\\aria2.pdb")
        else
            add_cxflags("-g")
        end
    end
    for _, dir in ipairs(sourceDirs) do
        add_files(dir .. "/*.cc")
        add_includedirs(dir)
    end
    add_files(
        "src/poll/select/*.cc",
        "src/parser/xml/expat/*.cc",
        "src/protocol/sftp/*.cc",
        "src/util/uri_split.c",
        "compat/gai_strerror.c",
        "compat/a2io.cc"
    )
    add_includedirs(
        "compat",
        "src/tls",
        "src/crypto",
        "src/poll",
        "src/protocol/sftp"
    )
    add_defines("WSLAY_VERSION=\""..PROJECT_VERSION.."\"")
    on_config(function (target)
        local variables = target:get("configvar") or {}
        for _, opt in ipairs(target:orderopts()) do
            for name, value in pairs(opt:get("configvar")) do
                if variables[name] == nil then
                    variables[name] = table.unwrap(value)
                    variables["__extraconf_" .. name] = opt:extraconf("configvar." .. name, value)
                end
            end
        end
        local set_configvar = function(k, v)
            if v == nil then
                return
            end
            target:set("configvar", k, v)
            variables[k] = v
        end
        set_configvar("HOST", vformat("$(host)"))
        set_configvar("BUILD", vformat("$(arch)-$(os)"))
        set_configvar("TARGET", vformat("$(arch)-$(os)"))
        local is_msvc = is_plat("windows")
        local compat_sources = {
            ["HAVE_ASCTIME_R"] = {"compat/asctime_r.c"},
            ["HAVE_GETADDRINFO"] = {"compat/getaddrinfo.c"},
            ["HAVE_GETTIMEOFDAY"] = {"compat/gettimeofday.c"},
            ["HAVE_LOCALTIME_R"] = {"compat/localtime_r.c"},
            ["HAVE_STRPTIME"] = {"compat/strptime.c"},
            ["HAVE_TIMEGM"] = {"compat/timegm.c"},
            ["HAVE_STRNCASECMP"] = {"compat/strncasecmp.c"},
            ["HAVE_GETOPT_H"] = {"compat/_getopt.c"}
        }
        for k, v in pairs(compat_sources) do
            if not variables[k] then
                target:add("files", table.unpack(v))
            end
        end
        if target:has_cfuncs("X509_get_default_cert_file", {includes = {"openssl/x509.h"}}) then
            set_configvar("HAVE_X509_GET_DEFAULT_CERT_FILE", 1)
        end
        if variables["HAVE_FALLOCATE"] or variables["HAVE_POSIX_FALLOCATE"] or target:is_plat("windows", "mingw", "macosx", "iphoneos") then
            set_configvar("HAVE_SOME_FALLOCATE", 1)
        end
        if get_config("unit") then
            local dir = path.absolute(os.projectdir(), '')
            dir = dir:gsub("\\", "/")
            set_configvar('A2_TEST_DIR', dir..'/test/data')
            set_configvar('A2_TEST_OUT_DIR', dir..'/build/test_out')
        end
    end)
    local skip = {}
    if is_plat("windows", "mingw") then
        add_files("src/win32/*.cc")
        add_includedirs("src/win32")
        add_syslinks("ws2_32", "shell32", "iphlpapi")
    end
    if ssl_external ~= true then
        if is_plat("windows", "mingw") then
            add_files("src/tls/wintls/*.cc")
            add_syslinks("crypt32", "secur32")
            set_configvar("SECURITY_WIN32", 1)
        elseif is_plat("macosx", "iphoneos") then
            add_files("src/tls/apple/*.cc")
            add_frameworks("CoreFoundation")
        end
        add_files("src/crypto/libssl/*.cc")
    else
        add_files("src/tls/libssl/*.cc", "src/crypto/libssl/*.cc")
    end
    if get_config("uv") then
        set_configvar("HAVE_LIBUV", 1)
        add_files("src/poll/libuv/*.cc")
        add_packages("uv")
    else
        if is_plat("linux", "android") then
            add_files("src/poll/epoll/*.cc")
            set_configvar("HAVE_EPOLL", 1)
        elseif is_plat("macosx", "iphoneos", "bsd") then
            add_files("src/poll/kqueue/*.cc")
            set_configvar("HAVE_KQUEUE", 1)
        end
    end

    add_includedirs("include", "deps/wslay/lib/includes")
    add_headerfiles("include/(aria2/*.h)")
    add_packages(
        "expat",
        "zlib",
        "sqlite3",
        "c-ares",
        get_config("use_quictls") and "quictls" or "libressl",
        "ssh2",
        "boost.intl",
        {public = true}
    )
    add_configfiles("config.h.in")
    add_defines("HAVE_CONFIG_H=1")
    if is_plat("macosx", "iphoneos") then
        add_frameworks("Security")
    end
target_end()

rule("mo")
    set_extensions(".po")
    on_buildcmd_file(function (target, batchcmds, sourcefile, opt)
        import("lib.detect.find_tool")
        local msgfmt = assert(find_tool("msgfmt"), "msgfmt not found!")
        local targetdir = path.join(string.vformat("$(buildir)/locale"), path.basename(sourcefile), "LC_MESSAGES")
        batchcmds:mkdir(targetdir)
        local mo = path.join(targetdir, "aria2-zero.mo")
        batchcmds:show_progress(opt.progress, "${color.build.object}compiling %s", sourcefile)
        batchcmds:vrunv(msgfmt.program, {"-o", mo, sourcefile})
        batchcmds:add_depfiles(sourcefile, mo)
    end)
rule_end()

target("aria2c")
    if is_mode("release") and get_config("with_breakpad") then
        if is_plat("windows") then
            add_cxflags("/Zi", "/FS", "/Fd$(buildir)\\$(plat)\\$(arch)\\release\\aria2c.pdb")
            add_ldflags("/DEBUG", "/PDB:$(buildir)\\$(plat)\\$(arch)\\release\\aria2c.pdb")
        else
            add_cxflags("-g")
        end
    end
    if get_config("with_breakpad") then
        add_packages("breakpad")
        add_defines("ENABLE_BREAKPAD=1")
    end
    if os.host() == "windows" then
        add_packages("gettext-tools")
    end
    if is_plat("windows", "mingw") then
        add_files("src/resource.rc")
    end
    add_rules("mo")
    add_files("po/*.po")
    add_files("src/*.cc")
    add_deps("aria2")
    add_includedirs("include", "compat", "src/core", "src/tls", "src/network", "src/util", "src/storage")
    add_defines("HAVE_CONFIG_H=1")
    if is_plat("mingw") then
        add_ldflags("-static")
    elseif is_plat("android", "linux") and get_config("with_static") then
        add_ldflags("-static")
    end
    -- after_build(function (target)
    --     os.mkdir("dist")
    --     local ext = is_plat("windows") and ".exe" or ""
    --     os.cp(target:targetfile(), format("dist/aria2c-%s-%s%s", target:plat(), target:arch(), ext))
    -- end)

xpack("aria2")
    set_title("aria2")
    set_description("aria2 is a lightweight multi-protocol & multi-source command-line download utility.")
    set_formats("zip")
    set_version(PROJECT_VERSION)
    set_homepage("https://github.com/zeromake/aria2-zero")
    set_author("zeromake<a390720046@gmail.com>")
    add_targets("aria2c")
    add_installfiles("build/(locale/*/LC_MESSAGES/aria2-zero.mo)", {prefixdir = "share"})
    set_basename("aria2-$(plat)-$(arch)")
xpack_end()

if get_config("unit") then
target("test")
    set_default(false)
    add_files("test/AllTest.cc", "test/TestUtil.cc")
    -- add_files("test/UtilTest2.cc")
    add_files("test/*.cc|AllTest.cc|TestUtil.cc|CookieBoxTest.cc")
    add_deps("aria2")
    add_includedirs(
        "include",
        "compat",
        "src/core",
        "src/tls",
        "src/parser",
        "src/parser/json",
        "src/crypto/common",
        "src/crypto",
        "src/poll",
        "src/poll/select",
        "src/stream",
        "src/network",
        "src/util",
        "src/rpc",
        "src/storage",
        "src/protocol",
        "src/protocol/lpd",
        "src/protocol/bt",
        "src/protocol/utm",
        "src/protocol/peer",
        "src/protocol/piece",
        "src/protocol/metalink",
        "src/protocol/announce"
    )
    add_packages("cppunit")
    add_tests("default")
    add_defines("HAVE_CONFIG_H=1")
target_end()
end
