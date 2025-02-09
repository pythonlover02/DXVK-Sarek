#pragma once

#include "d3d9_include.h"

namespace dxvk {

  /**
   * \brief D3D9 Software Cursor
   */
  struct D3D9_SOFTWARE_CURSOR {
    Com<IDirect3DTexture9> Bitmap;
    UINT XHotSpot = 0;
    UINT YHotSpot = 0;
    float X = 0.0f;
    float Y = 0.0f;
  };

  constexpr uint32_t HardwareCursorWidth      = 32u;
  constexpr uint32_t HardwareCursorHeight     = 32u;
  constexpr uint32_t HardwareCursorFormatSize = 4u;
  constexpr uint32_t HardwareCursorPitch      = HardwareCursorWidth * HardwareCursorFormatSize;

  // Format Size of 4 bytes (ARGB)
  using CursorBitmap = uint8_t[HardwareCursorHeight * HardwareCursorPitch];
  // Monochrome mask (1 bit)
  using CursorMask   = uint8_t[HardwareCursorHeight * HardwareCursorWidth / 8];

  class D3D9Cursor {

  public:

    void ResetCursor();

    void ResetHardwareCursor();

    void ResetSoftwareCursor();

    void UpdateCursor(int X, int Y);

    BOOL ShowCursor(BOOL bShow);

    HRESULT SetHardwareCursor(UINT XHotSpot, UINT YHotSpot, const CursorBitmap& bitmap);

    HRESULT SetSoftwareCursor(UINT XHotSpot, UINT YHotSpot, Com<IDirect3DTexture9> pCursorBitmap);

    D3D9_SOFTWARE_CURSOR* GetSoftwareCursor() {
      return &m_sCursor;
    }

    BOOL IsCursorVisible() const {
      return m_visible;
    }

  private:

    BOOL                  m_visible       = FALSE;

    D3D9_SOFTWARE_CURSOR  m_sCursor;

    HCURSOR               m_hCursor       = nullptr;

  };

}