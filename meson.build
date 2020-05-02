project('txproto', 'c',
    license: 'LGPL2.1+',
    default_options: [ 'buildtype=debugoptimized', 'c_std=gnu11'],
    version: '0.1',
    meson_version: '>=0.41.0',
)

conf = configuration_data()

conf.set_quoted('PROJECT_NAME', meson.project_name())
conf.set_quoted('PROJECT_VERSION_STRING', meson.project_version())

subdir('protocols')
subdir('src')

configure_file(
    output: 'config.h',
    configuration: conf,
)
