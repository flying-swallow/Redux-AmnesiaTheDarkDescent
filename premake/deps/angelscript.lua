-- AngelScript (scripting) -- explicit list mirrors extern/CMakeLists.txt.
-- The x64 MSVC assembly callfunc is added (and assembled via MASM) only on VS.
project "AngelScript"
    kind "StaticLib"
    language "C++"
    set_output("static")

    local sd = DEPS_SOURCES .. "/AngelScript/sources/"
    local names = {
        "as_arrayobject", "as_atomic", "as_builder", "as_bytecode",
        "as_callfunc_arm", "as_callfunc_mips", "as_callfunc_ppc_64",
        "as_callfunc_ppc", "as_callfunc_sh4", "as_callfunc_x64_gcc",
        "as_callfunc_x64_msvc", "as_callfunc_x86", "as_callfunc_xenon",
        "as_callfunc", "as_compiler", "as_configgroup", "as_context",
        "as_datatype", "as_gc", "as_generic", "as_globalproperty", "as_memory",
        "as_module", "as_objecttype", "as_outputbuffer", "as_parser",
        "as_restore", "as_scriptcode", "as_scriptengine", "as_scriptfunction",
        "as_scriptnode", "as_scriptobject", "as_string_util", "as_string",
        "as_thread", "as_tokenizer", "as_typeinfo", "as_variablescope",
    }
    for _, n in ipairs(names) do files { sd .. n .. ".cpp" } end
    includedirs { DEPS_SOURCES .. "/AngelScript/include" }

    filter "system:windows"
        files { sd .. "as_callfunc_x64_msvc_asm.asm" }
    filter { "system:windows", "files:**as_callfunc_x64_msvc_asm.asm" }
        defines { "_M_X64" }
    filter {}
