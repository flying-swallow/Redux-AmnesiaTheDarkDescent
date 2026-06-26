-- vorbis / vorbisfile / vorbisenc -- built from the extern/vorbis submodule.
-- libvorbis = all extern/vorbis/lib/*.c except the standalone programs and the
-- vorbisenc/vorbisfile translation units (built into their own libs).
local VORBIS_LIB = DEPS_EXTERN .. "/vorbis/lib"
local vorbis_excluded = {
    [VORBIS_LIB .. "/vorbisenc.c"] = true,
    [VORBIS_LIB .. "/vorbisfile.c"] = true,
    [VORBIS_LIB .. "/psytune.c"] = true,
    [VORBIS_LIB .. "/tone.c"] = true,
    [VORBIS_LIB .. "/barkmel.c"] = true,
}

local vorbis_srcs = {}
for _, f in ipairs(os.matchfiles(VORBIS_LIB .. "/*.c")) do
    if not vorbis_excluded[f] then table.insert(vorbis_srcs, f) end
end

-- ogg/vorbis public headers + libvorbis internal headers (lib/*.h).
local function vorbis_includes()
    includedirs {
        CONFIG_DIR .. "/common",            -- generated <ogg/config_types.h>
        DEPS_EXTERN .. "/ogg/include",      -- <ogg/*.h>
        DEPS_EXTERN .. "/vorbis/include",   -- <vorbis/*.h>
        VORBIS_LIB,                         -- internal os.h / misc.h / ...
    }
end

project "vorbis"
    kind "StaticLib"
    language "C"
    set_output("static")
    files (vorbis_srcs)
    vorbis_includes()
    links { "ogg" }
    filter "system:not windows"
        links { "m" }
    filter {}

project "vorbisfile"
    kind "StaticLib"
    language "C"
    set_output("static")
    files { VORBIS_LIB .. "/vorbisfile.c" }
    vorbis_includes()
    links { "vorbis", "ogg" }

project "vorbisenc"
    kind "StaticLib"
    language "C"
    set_output("static")
    files { VORBIS_LIB .. "/vorbisenc.c" }
    vorbis_includes()
    links { "vorbis", "ogg" }
