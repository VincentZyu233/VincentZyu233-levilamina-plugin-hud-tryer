add_rules("mode.debug", "mode.release")

add_repositories("levimc-repo https://github.com/LiteLDev/xmake-repo.git")

option("target_type")
    set_default("server")
    set_showmenu(true)
    set_values("server", "client")
option_end()

local levilamina_version = "26.10.14"

-- libssh2 1.11.1's prebuilt Windows package references this exact zlib package path.
add_requires("zlib 1.3.1", {override = true})
add_requires("levilamina " .. levilamina_version, {configs = {target_type = get_config("target_type")}})

add_requires("levibuildscript 0.6.1")

if not has_config("vs_runtime") then
    set_runtimes("MD")
end

target("hud-tryer")
    add_rules("@levibuildscript/linkrule")
    on_load(function (target)
        import("core.base.json")
        local metadata = json.loadfile(path.join(os.projectdir(), "tooth.json"))
        local mod_version = metadata and metadata["version"]
        if type(mod_version) ~= "string" or mod_version == "" then
            raise("tooth.json must contain a non-empty string version")
        end
        target:add("rules", "@levibuildscript/modpacker", {modVersion = mod_version})
    end)
    add_cxflags( "/EHa", "/utf-8", "/W4", "/w44265", "/w44289", "/w44296", "/w45263", "/w44738", "/w45204")
    add_defines("NOMINMAX", "UNICODE")
    add_packages("levilamina")
    set_exceptions("none") -- To avoid conflicts with /EHa.
    set_kind("shared")
    set_languages("c++20")
    set_symbols("debug")
    add_headerfiles("src/**.h")
    add_files("src/**.cpp")
    add_includedirs("src")
    if is_config("target_type", "server") then
    --  add_includedirs("src-server")
    --  add_files("src-server/**.cpp")
    else
    --  add_includedirs("src-client")
    --  add_files("src-client/**.cpp")
    end
