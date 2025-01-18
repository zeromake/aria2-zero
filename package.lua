set_policy("package.install_only", true)
add_repositories("zeromake https://github.com/zeromake/xrepo.git")
add_requires(
    "expat",
    "zlib",
    "sqlite3",
    "c-ares",
    "quictls",
    "nonstd.string-view"
)
add_requires("boost.intl")
add_requires("ssh2", {configs = {quictls = true}})
if get_config("unit") then
    add_requires("cppunit", {optional = true})
end
if is_plat("windows", "mingw") then
    add_requires("gettext-tools", {optional = true})
end
