project('linenoise', 'c')

sources = [
  'linenoise.c',
]

inc = include_directories('include')

lib = static_library('linenoise',
    sources,
    include_directories: inc,
    install: false,
)

linenoise_dep = declare_dependency(
    include_directories: inc,
    link_with: lib
)
