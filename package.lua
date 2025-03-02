set_policy("package.install_only", true)
add_repositories("zeromake https://github.com/zeromake/xrepo.git")
local ssl_name = get_config("use_quictls") and "quictls" or "libressl"
add_requires(
    "expat",
    "zlib",
    "sqlite3",
    "c-ares",
    "nonstd.string-view"
)
add_requires("boost.intl")
if get_config("use_quictls") then
    add_requires("quictls")
    add_requires("ssh2", {configs = {quictls = true}})
else
    add_requires("libressl")
    add_requires("ssh2", {configs = {libressl = true}})
end
if get_config("unit") then
    add_requires("cppunit", {optional = true})
end
if os.host() == "windows" then
    add_requires("gettext-tools", {optional = true})
end


if get_config("with_breakpad") then
    add_requires("breakpad")
end
