#pragma once

#include <d3d9.h>

namespace dxvk {
  using NTSTATUS = LONG;
  using D3DKMT_HANDLE = UINT;

  // Slightly modified definitions...
  struct D3DKMT_CREATEDCFROMMEMORY {
    void*         pMemory;
    D3DFORMAT     Format;
    UINT          Width;
    UINT          Height;
    UINT          Pitch;
    HDC           hDeviceDc;
    PALETTEENTRY* pColorTable;
    HDC           hDc;
    HANDLE        hBitmap;
  };

  struct D3DKMT_DESTROYDCFROMMEMORY {
    HDC    hDC     = nullptr;
    HANDLE hBitmap = nullptr;
  };

  typedef enum _D3DKMT_ESCAPETYPE
  {
      D3DKMT_ESCAPE_UPDATE_RESOURCE_WINE = 0x80000000,
      D3DKMT_ESCAPE_SET_PRESENT_RECT_WINE = 0x80000001,
  } D3DKMT_ESCAPETYPE;

  typedef struct _D3DDDI_ESCAPEFLAGS
  {
      union
      {
          struct
          {
              UINT HardwareAccess :1;
              UINT Reserved       :31;
          };
          UINT Value;
      };
  } D3DDDI_ESCAPEFLAGS;

  typedef struct _D3DKMT_ESCAPE
  {
      D3DKMT_HANDLE      hAdapter;
      D3DKMT_HANDLE      hDevice;
      D3DKMT_ESCAPETYPE  Type;
      D3DDDI_ESCAPEFLAGS Flags;
      void              *pPrivateDriverData;
      UINT               PrivateDriverDataSize;
      D3DKMT_HANDLE      hContext;
  } D3DKMT_ESCAPE;

  using D3DKMTCreateDCFromMemoryType  = NTSTATUS(STDMETHODCALLTYPE*) (D3DKMT_CREATEDCFROMMEMORY*);
  NTSTATUS D3DKMTCreateDCFromMemory (D3DKMT_CREATEDCFROMMEMORY*  Arg1);

  using D3DKMTDestroyDCFromMemoryType = NTSTATUS(STDMETHODCALLTYPE*) (D3DKMT_DESTROYDCFROMMEMORY*);
  NTSTATUS D3DKMTDestroyDCFromMemory(D3DKMT_DESTROYDCFROMMEMORY* Arg1);

  // The present rect escape is only understood by Wine's gdi32. On a host
  // that does not export D3DKMTEscape at all, or an older Wine that does
  // not know the escape type, this degrades to a warning and the caller
  // carries on with plain window positioning.
  using D3DKMTEscapeType              = NTSTATUS(STDMETHODCALLTYPE*) (const D3DKMT_ESCAPE*);
  NTSTATUS D3DKMTEscape             (const D3DKMT_ESCAPE*        Arg1);

}
