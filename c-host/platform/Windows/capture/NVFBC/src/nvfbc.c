/*
Looking Glass - KVM FrameRelay (KVMFR) Client
Copyright (C) 2017-2019 Geoffrey McRae <geoff@hostfission.com>
https://looking-glass.hostfission.com

This program is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free Software
Foundation; either version 2 of the License, or (at your option) any later
version.

This program is distributed in the hope that it will be useful, but WITHOUT ANY
WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A
PARTICULAR PURPOSE. See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with
this program; if not, write to the Free Software Foundation, Inc., 59 Temple
Place, Suite 330, Boston, MA 02111-1307 USA
*/

#include "interface/capture.h"
#include "interface/platform.h"
#include "windows/platform.h"
#include "windows/windebug.h"
#include "windows/mousehook.h"
#include <assert.h>
#include <stdlib.h>
#include <windows.h>

#include <NvFBC/nvFBC.h>
#include "wrapper.h"

struct iface
{
  bool        stop;
  NvFBCHandle nvfbc;

  void       * pointerShape;
  unsigned int pointerSize;
  unsigned int maxWidth, maxHeight;
  unsigned int width   , height;

  uint8_t * frameBuffer;

  NvFBCFrameGrabInfo grabInfo;

  osEventHandle * frameEvent;
  osEventHandle * cursorEvents[2];

  int mouseX, mouseY, mouseHotX, mouseHotY;
  bool mouseVisible;
};

static struct iface * this = NULL;

static void nvfbc_free();

static void getDesktopSize(unsigned int * width, unsigned int * height)
{
  HMONITOR    monitor     = MonitorFromWindow(GetDesktopWindow(), MONITOR_DEFAULTTOPRIMARY);
  MONITORINFO monitorInfo = {
    .cbSize = sizeof(MONITORINFO)
  };

  GetMonitorInfo(monitor, &monitorInfo);
  CloseHandle(monitor);

  *width  = monitorInfo.rcMonitor.right  - monitorInfo.rcMonitor.left;
  *height = monitorInfo.rcMonitor.bottom - monitorInfo.rcMonitor.top;
}

static void on_mouseMove(int x, int y)
{
  this->mouseX = x;
  this->mouseY = y;
  os_signalEvent(this->cursorEvents[1]);
}

static const char * nvfbc_getName()
{
  return "NVFBC (NVidia Frame Buffer Capture)";
};

static bool nvfbc_create()
{
  if (!NvFBCInit())
    return false;

  this = (struct iface *)calloc(sizeof(struct iface), 1);

  if (!NvFBCToSysCreate(NULL, 0, &this->nvfbc, &this->maxWidth, &this->maxHeight))
  {
    nvfbc_free();
    return false;
  }

  this->frameEvent = os_createEvent(true);
  if (!this->frameEvent)
  {
    DEBUG_ERROR("failed to create the frame event");
    nvfbc_free();
    return false;
  }

  return true;
}

static bool nvfbc_init(void * pointerShape, const unsigned int pointerSize)
{
  this->stop     = false;
  this->pointerShape = pointerShape;
  this->pointerSize  = pointerSize;

  getDesktopSize(&this->width, &this->height);
  os_resetEvent(this->frameEvent);

  HANDLE event;
  if (!NvFBCToSysSetup(
    this->nvfbc,
    BUFFER_FMT_ARGB10,
    false,
    true,
    false,
    0,
    (void **)&this->frameBuffer,
    NULL,
    &event
  ))
  {
    return false;
  }

  this->cursorEvents[0] = os_wrapEvent(event);
  this->cursorEvents[1] = os_createEvent(true);
  mouseHook_install(on_mouseMove);

  Sleep(100);
  return true;
}

static void nvfbc_stop()
{
  this->stop = true;
  os_signalEvent(this->cursorEvents[1]);
  os_signalEvent(this->frameEvent);
}

static bool nvfbc_deinit()
{
  mouseHook_remove();
  return true;
}

static void nvfbc_free()
{
  NvFBCToSysRelease(&this->nvfbc);

  if (this->frameEvent)
    os_freeEvent(this->frameEvent);

  free(this);
  this = NULL;
  NvFBCFree();
}

static unsigned int nvfbc_getMaxFrameSize()
{
  return this->maxWidth * this->maxHeight * 4;
}

static CaptureResult nvfbc_capture()
{
  getDesktopSize(&this->width, &this->height);
  NvFBCFrameGrabInfo grabInfo;
  CaptureResult result = NvFBCToSysCapture(
    this->nvfbc,
    1000,
    0, 0,
    this->width,
    this->height,
    &grabInfo
  );

  if (result != CAPTURE_RESULT_OK)
    return result;

  memcpy(&this->grabInfo, &grabInfo, sizeof(grabInfo));
  os_signalEvent(this->frameEvent);
  return CAPTURE_RESULT_OK;
}

static CaptureResult nvfbc_getFrame(CaptureFrame * frame)
{
  if (!os_waitEvent(this->frameEvent, TIMEOUT_INFINITE))
  {
    DEBUG_ERROR("Failed to wait on the frame event");
    return CAPTURE_RESULT_ERROR;
  }

  if (this->stop)
    return CAPTURE_RESULT_REINIT;

  frame->width  = this->grabInfo.dwWidth;
  frame->height = this->grabInfo.dwHeight;
  frame->pitch  = this->grabInfo.dwBufferWidth * 4;
  frame->stride = this->grabInfo.dwBufferWidth;

  // the bIsHDR check isn't reliable, if it's not set check a few pixels to see if
  // the alpha channel has data in it. If not HDR the alpha channel should read zeros
  this->grabInfo.bIsHDR =
    this->grabInfo.bIsHDR ||
    (this->frameBuffer[3] != 0)                                                                  || // top left
    (this->frameBuffer[(((frame->height * frame->stride) / 2) + frame->width / 2) * 4 + 3] != 0) || // center
    (this->frameBuffer[(((frame->height - 1) * frame->stride) + frame->width - 1) * 4 + 3] != 0);   // bottom right

  frame->format = this->grabInfo.bIsHDR ? CAPTURE_FMT_RGBA10 : CAPTURE_FMT_BGRA;
  memcpy(frame->data, this->frameBuffer, frame->pitch * frame->height);
  return CAPTURE_RESULT_OK;
}

static CaptureResult nvfbc_getPointer(CapturePointer * pointer)
{
  osEventHandle * events[2];
  memcpy(&events, &this->cursorEvents, sizeof(osEventHandle *) * 2);
  if (!os_waitEvents(events, 2, false, TIMEOUT_INFINITE))
  {
    DEBUG_ERROR("Failed to wait on the cursor events");
    return CAPTURE_RESULT_ERROR;
  }

  if (this->stop)
    return CAPTURE_RESULT_REINIT;

  CaptureResult result;
  pointer->shapeUpdate = false;
  if (events[0])
  {
    result = NvFBCToSysGetCursor(this->nvfbc, pointer, this->pointerShape, this->pointerSize);
    this->mouseVisible = pointer->visible;
    this->mouseHotX    = pointer->x;
    this->mouseHotY    = pointer->y;
    if (result != CAPTURE_RESULT_OK)
      return result;
  }

  pointer->visible = this->mouseVisible;
  pointer->x       = this->mouseX - this->mouseHotX;
  pointer->y       = this->mouseY - this->mouseHotY;
  return CAPTURE_RESULT_OK;
}

struct CaptureInterface Capture_NVFBC =
{
  .getName         = nvfbc_getName,
  .create          = nvfbc_create,
  .init            = nvfbc_init,
  .stop            = nvfbc_stop,
  .deinit          = nvfbc_deinit,
  .free            = nvfbc_free,
  .getMaxFrameSize = nvfbc_getMaxFrameSize,
  .capture         = nvfbc_capture,
  .getFrame        = nvfbc_getFrame,
  .getPointer      = nvfbc_getPointer
};