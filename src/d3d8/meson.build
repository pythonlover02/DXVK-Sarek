d3d8_res = wrc_generator.process('version.rc')

d3d8_src = [
  'd3d8_buffer.cpp',
  'd3d8_device.cpp',
  'd3d8_interface.cpp',
  'd3d8_main.cpp',
  'd3d8_multithread.cpp',
  'd3d8_options.cpp',
  'd3d8_shader.cpp',
  'd3d8_state_block.cpp',
  'd3d8_surface.cpp',
  'd3d8_swapchain.cpp',
  'd3d8_texture.cpp',
  'd3d8_volume.cpp'
]

d3d8_dll = shared_library('d3d8'+dll_ext, d3d8_src, d3d8_res,
  name_prefix         : '',
  dependencies        : [ lib_d3d9, util_dep, dxso_dep, dxvk_dep ],
  include_directories : dxvk_include_path,
  install             : true,
  vs_module_defs      : 'd3d8'+def_spec_ext,
)

d3d8_dep = declare_dependency(
  link_with           : [ d3d8_dll ],
  include_directories : [ dxvk_include_path ],
)
