project(
  'cliplist',
  'cpp',
  meson_version : '>= 1.3.0',
  version : '0.1',
  default_options : ['warning_level=3'],
)


qt6=import('qt6')
qt6_dep=dependency('qt6',modules:['Core','Gui','Widgets'])
moc_files=qt6.compile_moc(
  headers:['cliplist.h','htmlclipboardlist.h'],
  dependencies:qt6_dep
  )

sources = [
  'main.cpp',
]

exe = executable(
  'cliplist',
  sources,
  moc_files,
  dependencies : qt6_dep,
  install : true,
)

test('basic', exe)
