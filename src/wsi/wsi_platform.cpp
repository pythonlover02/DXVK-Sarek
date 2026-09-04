#include "wsi_platform.h"
#include "wsi_monitor.h"
#include "wsi_window.h"
#include "../util/util_env.h"
#include "../util/util_error.h"

namespace dxvk::wsi {
  static const WsiBootstrap *wsiBootstrap[] = {
    &Win32WSI,
  };

  static WsiDriver* createDriver() {
    std::string hint = dxvk::env::getEnvVar("DXVK_WSI_DRIVER");
    if (hint == "")
      hint = "Win32";

    for (const WsiBootstrap *b : wsiBootstrap) {
      WsiDriver *driver = nullptr;

      if (hint == b->name && b->createDriver(&driver))
        return driver;
    }

    throw DxvkError("Failed to initialize WSI.");
  }

  // The driver is created on first use rather than being tied to the
  // lifetime of a DxvkInstance. Each DLL linking this library holds its
  // own copy of the pointer, and not every one of them creates an
  // instance: d3d11 takes the one owned by the dxgi factory, so a driver
  // created alongside the instance would leave d3d11's copy null. Creating
  // it here also keeps it alive past the last instance, which swap chains
  // rely on when they restore the display mode from their destructors.
  static WsiDriver* getDriver() {
    static WsiDriver* driver = createDriver();
    return driver;
  }

  // Resolves the driver on every use, so that the call sites below can
  // keep reading as plain member calls.
  struct WsiDriverRef {
    WsiDriver* operator -> () const { return getDriver(); }
  };

  static WsiDriverRef s_driver;

  void init() {
    // Not required, but reports a driver that cannot be created at the
    // point the instance is set up rather than on the first window call.
    getDriver();
  }

  void quit() {
  }

  std::vector<const char *> getInstanceExtensions() {
    return s_driver->getInstanceExtensions();
  }

  void getWindowSize(
          HWND      hWindow,
          uint32_t* pWidth,
          uint32_t* pHeight) {
    s_driver->getWindowSize(hWindow, pWidth, pHeight);
  }

  void resizeWindow(
          HWND             hWindow,
          DxvkWindowState* pState,
          uint32_t         width,
          uint32_t         height) {
    s_driver->resizeWindow(hWindow, pState, width, height);
  }

  void saveWindowState(
          HWND             hWindow,
          DxvkWindowState* pState,
          bool             saveStyle) {
    s_driver->saveWindowState(hWindow, pState, saveStyle);
  }

  void restoreWindowState(
          HWND             hWindow,
          DxvkWindowState* pState,
          bool             restoreCoordinates) {
    s_driver->restoreWindowState(hWindow, pState, restoreCoordinates);
  }

  bool setWindowMode(
          HMONITOR         hMonitor,
          HWND             hWindow,
          DxvkWindowState* pState,
    const WsiMode&         mode) {
    return s_driver->setWindowMode(hMonitor, hWindow, pState, mode);
  }

  bool enterFullscreenMode(
          HMONITOR         hMonitor,
          HWND             hWindow,
          DxvkWindowState* pState,
          [[maybe_unused]]
          bool             modeSwitch) {
    return s_driver->enterFullscreenMode(hMonitor, hWindow, pState, modeSwitch);
  }

  bool leaveFullscreenMode(
          HWND             hWindow,
          DxvkWindowState* pState) {
    return s_driver->leaveFullscreenMode(hWindow, pState);
  }

  bool restoreDisplayMode() {
    return s_driver->restoreDisplayMode();
  }

  HMONITOR getWindowMonitor(HWND hWindow) {
    return s_driver->getWindowMonitor(hWindow);
  }

  bool isWindow(HWND hWindow) {
    return s_driver->isWindow(hWindow);
  }

  bool isMinimized(HWND hWindow) {
    return s_driver->isMinimized(hWindow);
  }

  bool isOccluded(HWND hWindow) {
    return s_driver->isOccluded(hWindow);
  }

  void updateFullscreenWindow(
          HMONITOR hMonitor,
          HWND     hWindow,
          bool     forceTopmost) {
    s_driver->updateFullscreenWindow(hMonitor, hWindow, forceTopmost);
  }

  VkResult createSurface(
          HWND                hWindow,
          PFN_vkGetInstanceProcAddr pfnVkGetInstanceProcAddr,
          VkInstance          instance,
          VkSurfaceKHR*       pSurface) {
    return s_driver->createSurface(hWindow, pfnVkGetInstanceProcAddr, instance, pSurface);
  }

  HMONITOR getDefaultMonitor() {
    return s_driver->getDefaultMonitor();
  }

  HMONITOR enumMonitors(uint32_t index) {
    return s_driver->enumMonitors(index);
  }

  bool getDisplayName(
          HMONITOR         hMonitor,
          WCHAR            (&Name)[32]) {
    return s_driver->getDisplayName(hMonitor, Name);
  }

  bool getDesktopCoordinates(
          HMONITOR         hMonitor,
          RECT*            pRect) {
    return s_driver->getDesktopCoordinates(hMonitor, pRect);
  }

  bool getDisplayMode(
          HMONITOR         hMonitor,
          uint32_t         modeNumber,
          WsiMode*         pMode) {
    return s_driver->getDisplayMode(hMonitor, modeNumber, pMode);
  }

  bool getCurrentDisplayMode(
          HMONITOR         hMonitor,
          WsiMode*         pMode) {
    return s_driver->getCurrentDisplayMode(hMonitor, pMode);
  }

}
