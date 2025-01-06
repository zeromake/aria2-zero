set_policy("package.install_only", true)
add_repositories("zeromake https://github.com/zeromake/xrepo.git")
add_requires(
    "expat",
    "zlib",
    "sqlite3",
    "c-ares",
    "libressl",
    "ssh2",
    "nonstd.string-view"
)
add_requires("boost.intl", {configs = {debug = true}})
if get_config("unit") then
    add_requires("cppunit", {optional = true})
end
