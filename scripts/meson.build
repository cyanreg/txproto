luac = find_program('luac')

builtin_scripts = [
    'default.lua',
]

builtin_scripts_compiled = []
foreach s: builtin_scripts
    builtin_scripts_compiled += s + '.bin'
endforeach

luac_target = custom_target('luac',
                            input: builtin_scripts,
                            output: builtin_scripts_compiled,
                            command: [luac, '-o', '@OUTPUT@', '@INPUT@'])

xxd = find_program('xxd')

builtin_scripts_inc = []
foreach s: builtin_scripts_compiled
    builtin_scripts_inc += s + '.h'
endforeach

xxd_target = custom_target('xxd',
                           input: luac_target,
                           output: builtin_scripts_inc,
                           command: [xxd, '-i', '@INPUT@', '@OUTPUT@'])
