plugin_install_subdir = 'xfce4' / 'panel' / 'plugins'

plugin_lib = shared_module(
  'resmon',
  [
    'cpu.cc',
    'cpu.h',
    'memswap.cc',
    'memswap.h',
    'network.cc',
    'network.h',
    'plugin.c',
    'plugin.h',
    'settings.cc',
    'settings.h',
    'systemload.cc',
    'uptime.cc',
    'uptime.h',
  ],
  gnu_symbol_visibility: 'hidden',
  c_args: [
    '-DG_LOG_DOMAIN="xfce4-systemload-plugin"',
    '-DHAVE_CONFIG_H',
  ],
  cpp_args: [
    '-DG_LOG_DOMAIN="xfce4-systemload-plugin"',
    '-DHAVE_CONFIG_H',
  ],
  include_directories: [
    include_directories('..'),
  ],
  dependencies: [
    glib,
    gtk,
    libxfce4panel,
    libxfce4ui,
    libxfce4util,
    xfconf,
    libm,
  ],
  install: true,
  install_dir: get_option('prefix') / get_option('libdir') / plugin_install_subdir,
)

i18n.merge_file(
  input: 'resmon.desktop.in',
  output: 'resmon.desktop',
  po_dir: '..' / 'po',
  type: 'desktop',
  install: true,
  install_dir: get_option('prefix') / get_option('datadir') / plugin_install_subdir,
)
