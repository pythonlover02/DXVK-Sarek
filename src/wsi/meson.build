wsi_src = [
  'wsi_platform.cpp',
  'win32/wsi_monitor_win32.cpp',
  'win32/wsi_platform_win32.cpp',
  'win32/wsi_window_win32.cpp',
]

wsi_lib = static_library('wsi', wsi_src,
  include_directories : [ dxvk_include_path ])

wsi_dep = declare_dependency(
  link_with           : [ wsi_lib ])
