project(
  'resmon',
  'c',
  'cpp',
  version : '1.3.2',
  license : 'BSD-2-Clause',
  meson_version : '>= 0.54.0',
  default_options : ['c_std=gnu11', 'cpp_std=gnu++17', 'buildtype=debugoptimized', 'warning_level=2']
)

cc = meson.get_compiler('c')
cxx = meson.get_compiler('cpp')
pkgconfig = import('pkgconfig')
i18n = import('i18n')

glib = dependency('glib-2.0')
gtk = dependency('gtk+-3.0')
libxfce4panel = dependency('libxfce4panel-2.0')
libxfce4ui = dependency('libxfce4ui-2')
libxfce4util = dependency('libxfce4util-1.0')
xfconf = dependency('libxfconf-0')
libm = cc.find_library('m')

prefix = get_option('prefix')
localedir = prefix / get_option('localedir')

add_project_arguments('-DGETTEXT_PACKAGE="resmon"', language: ['c', 'cpp'])

subdir('panel-plugin')
