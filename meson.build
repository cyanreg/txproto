project('txproto', 'c',
    license: 'LGPL2.1+',
    default_options: [ 'buildtype=debugoptimized', 'c_std=c99', 'warning_level=1' ],
    version: '0.1.0',
    meson_version: '>=0.60.0',
)

lua_ver_maj = 5
lua_ver_min = 4
lua_ver = lua_ver_maj.to_string() + '.' + lua_ver_min.to_string()
conf = configuration_data()

conf.set_quoted('PROJECT_NAME', meson.project_name())
conf.set_quoted('PROJECT_VERSION_STRING', meson.project_version())

version_split = meson.project_version().split('.')
ver_major = version_split[0]
ver_minor = version_split[1]
ver_micro = version_split[2]

conf.set('PROJECT_VERSION_MAJOR', ver_major)
conf.set('PROJECT_VERSION_MINOR', ver_minor)
conf.set('PROJECT_VERSION_MICRO', ver_micro)

cc = meson.get_compiler('c')

build_opts = [
    '-D_ISOC11_SOURCE', '-D_XOPEN_SOURCE=700', '-U__STRICT_ANSI__',

    # Warnings
    '-Wundef', '-Wmissing-prototypes', '-Wshadow', '-Wparentheses',
    '-Wpointer-arith', '-Wno-pointer-sign',

    # Warnings to treat as errors
    '-Werror=implicit-function-declaration',
]

# Required dependencies
threads_dep = dependency('threads')
dependencies = [
    # ffmpeg libs
    dependency('libavcodec', version: '>= 59.4.100'),
    dependency('libavformat', version: '>= 58.42.100'),
    dependency('libswresample', version: '>= 3.6.100'),
    dependency('libavfilter', version: '>= 7.79.100'),
    dependency('libavutil', version: '>= 56.43.100'),

    # lua
    dependency('lua', version: ['>= ' + lua_ver, '< ' + lua_ver_maj.to_string() + '.' + (lua_ver_min + 1).to_string()]),

    # zlib
    dependency('zlib'),

    # pthreads
    threads_dep,
    cc.find_library('m', required : true),
]

# Check for libdl
libdl = cc.find_library('dl', required : false)
if libdl.found()
    dependencies += libdl
endif

# Check for wmain support (Windows/MinGW)
if cc.links('int wmain() { return 0; }', args: '-municode')
     conf.set('HAVE_WMAIN', 1)
     build_opts += '-municode'
endif

# Check for pthread_setname_np
if cc.has_function('pthread_setname_np', prefix: '#include <pthread.h>',
                   args: [ '-D_GNU_SOURCE' ], dependencies: threads_dep)
    conf.set('HAVE_PTHREAD_SETNAME_NP', 1)
    build_opts += '-D_GNU_SOURCE'
endif

# Check for memfd (currently wayland only)
has_memfd = false
if get_option('wayland').auto()
    has_memfd = cc.has_function('memfd_create', prefix: '#include <sys/mman.h>',
                                args: [ '-D_GNU_SOURCE' ])
endif

if has_memfd
    build_opts += '-D_GNU_SOURCE'
endif

add_project_arguments(build_opts, language: 'c')

features = '"base'

# scripts
subdir('scripts')
subdir('src')

conf.set('COMPILER', '"' + cc.get_id() + ' ' + cc.version() + '"')
conf.set('FEATURE_SET', features + '"')

configure_file(
    output: 'config.h',
    configuration: conf,
)
