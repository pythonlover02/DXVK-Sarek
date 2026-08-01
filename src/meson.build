subdir('util')
subdir('spirv')
subdir('vulkan')
subdir('dxvk')

if get_option('enable_dxgi')
  if not get_option('enable_d3d11')
    error('D3D11 is required for DXGI.')
  endif
  subdir('dxgi')
endif

if get_option('enable_d3d10') or get_option('enable_d3d11')
  subdir('dxbc')
endif

if get_option('enable_d3d11')
  subdir('d3d11')
endif

if get_option('enable_d3d10')
  if not get_option('enable_d3d11')
    error('D3D11 is required for D3D10.')
  endif
  subdir('d3d10')
endif

if get_option('enable_d3d9')
  subdir('dxso')
  subdir('d3d9')
endif

if get_option('enable_d3d8')
  if not get_option('enable_d3d9')
    error('D3D9 is required for D3D8.')
  endif
  subdir('d3d8')
endif

# DDRAW is 32-bit x86 only, ARM64, x86_64, etc, never had a ddraw runtime
if get_option('enable_ddraw') and cpu_family == 'x86'
  if not get_option('enable_d3d9')
    error('D3D9 is required for DDRAW.')
  endif
  subdir('ddraw')
endif

if not get_option('enable_d3d8') and not get_option('enable_d3d9') and not get_option('enable_dxgi')
  warning('Nothing selected to be built.?')
endif
