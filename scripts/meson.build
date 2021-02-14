luac = find_program('luac')

builtin_scripts = [
    'default.lua',
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
