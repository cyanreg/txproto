luac = find_program('luac')
xxd = find_program('xxd')

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
                                         command: [xxd, '-i', '@INPUT@', '@OUTPUT@'])
endforeach
