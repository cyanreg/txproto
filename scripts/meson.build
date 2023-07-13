luac = find_program('luac' + lua_ver, required: false)

if not luac.found()
    luac = find_program('luac', required: true)

    res = run_command(luac, '-v').stdout()
    assert(res.contains(' ' + lua_ver), 'luac executable version ' + lua_ver + ' is required to compile ' + meson.project_name() + ', executable version: \'' + res)
endif

builtin_scripts = [
    'utils.lua',
]

builtin_lua_scripts = []
foreach s: builtin_scripts
    compiled_script = custom_target('Compiled ' + s,
                                    input: s,
                                    output: s + '.bin',
                                    command: [luac, '-o', '@OUTPUT@', '@INPUT@'])

    builtin_lua_scripts += custom_target('Converted ' + s + '.bin',
                                         input: compiled_script,
                                         output: s + '.bin' + '.h',
                                         command: [files('../tools/xxdi.py'), '@INPUT@', '@OUTPUT@'])
endforeach

cli_scripts = [
    'default.lua',
]

cli_lua_scripts = []
if get_option('cli').enabled()
    foreach s: cli_scripts
        compiled_script = custom_target('Compiled ' + s,
                                        input: s,
                                        output: s + '.bin',
                                        command: [luac, '-o', '@OUTPUT@', '@INPUT@'])

        cli_lua_scripts += custom_target('Converted ' + s + '.bin',
                                         input: compiled_script,
                                         output: s + '.bin' + '.h',
                                         command: [files('../tools/xxdi.py'), '@INPUT@', '@OUTPUT@'])
    endforeach
endif
