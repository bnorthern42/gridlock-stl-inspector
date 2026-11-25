project(
  'gridlock-stl-inspector',
  'cpp',
  version: '1.0.0',
  license: 'MIT',
  meson_version: '>= 0.63.0',
  default_options: [
    'cpp_std=c++23',
    'warning_level=2',
    'werror=true',
  ],
)

fs = import('fs')
cpp = meson.get_compiler('cpp')

requested_inc = get_option('gridlock_include_dir')
resolved_inc = ''

candidate_dirs = [requested_inc]
if requested_inc == '/usr/local/include'
  candidate_dirs += [
    '/home/bradn4/Projects/Software_Dev/C++/Gridlock/include',
    meson.project_source_root() / '..' / 'Gridlock' / 'include',
  ]
endif

foreach dir : candidate_dirs
  if resolved_inc == '' and fs.is_file(dir / 'gridlock' / 'plugin_api.h')
    resolved_inc = dir
  endif
endforeach

if resolved_inc == ''
  error(
    'Could not locate gridlock/plugin_api.h (searched: '
    + ', '.join(candidate_dirs)
    + '). Pass -Dgridlock_include_dir=/path/to/Gridlock/include.'
  )
endif

if resolved_inc != requested_inc
  message('gridlock/plugin_api.h not found in ' + requested_inc + '; falling back to ' + resolved_inc)
endif

extra_cpp_args = cpp.get_supported_arguments([
  '-Wall',
  '-Wextra',
  '-Werror',
  '-fvisibility=hidden',
  '-fvisibility-inlines-hidden',
])

gridlock_abi_dep = declare_dependency(
  compile_args: ['-I' + resolved_inc],
)

darwin_suffix = host_machine.system() == 'darwin' ? 'dylib' : []

extra_link_args = []
link_depends_files = []
if host_machine.system() == 'linux'
  version_script = meson.project_source_root() / 'src' / 'plugin.map'
  version_script_flag = '-Wl,--version-script,' + version_script
  if cpp.has_link_argument(version_script_flag)
    extra_link_args += [version_script_flag]
    link_depends_files += [files('src/plugin.map')]
  endif
elif host_machine.system() == 'darwin'
  if cpp.has_link_argument('-Wl,-exported_symbol,_gridlock_plugin_init_v1')
    extra_link_args += ['-Wl,-exported_symbol,_gridlock_plugin_init_v1']
  endif
endif

shared_module(
  'gridlock_stl_inspector',
  files('src/plugin.cpp'),
  dependencies: [gridlock_abi_dep],
  cpp_args: extra_cpp_args,
  link_args: extra_link_args,
  link_depends: link_depends_files,
  gnu_symbol_visibility: 'hidden',
  name_prefix: 'lib',
  name_suffix: darwin_suffix,
  install: false,
)
