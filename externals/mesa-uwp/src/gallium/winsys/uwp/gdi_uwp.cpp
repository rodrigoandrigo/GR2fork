#include "gdi_uwp.h"

#include "..\..\frontends\wgl\stw_pixelformat.h"
#include "..\..\frontends/wgl/stw_winsys.h"

#include <wrl.h>
#include <wrl/client.h>
#include <dxgi1_4.h>
#include <d3d12.h>
#include <memory>
#include <vector>
#include <mutex>
#include <unordered_map>

#include "util/uwp_util.h"

static std::mutex pixel_format_mutex;
struct uwp_pixel_format {
   int format;
   bool explicitly_set;
};
static std::unordered_map<HDC, uwp_pixel_format> pixel_formats;
BOOL WINAPI GetClientRect( _In_ HWND hWnd, _Out_ LPRECT lpRect)
{
   if (nullptr != lpRect)
   {
      lpRect->top = 0;
      lpRect->bottom = uwp_get_height();
      lpRect->left = 0;
      lpRect->right = uwp_get_width();
      return lpRect->bottom >= 0 && lpRect->right >= 0;
   }
   return FALSE;
}

/*There is only a single corewindow in our model, and we don't even have control over it*/

HWND
WINAPI
WindowFromDC(
   _In_ HDC hDC)
{
   return hDC ? (HWND)hDC :
      (HWND)reinterpret_cast<IUnknown*>(uwp_get_window_reference());
}

HDC
WINAPI
GetDC(
   _In_opt_ HWND hWnd)
{
   return (HDC)hWnd;
}

int
WINAPI
ReleaseDC(
   _In_opt_ HWND hWnd,
   _In_ HDC hDC)
{
   std::lock_guard<std::mutex> lock(pixel_format_mutex);
   pixel_formats.erase(hDC);
   return 1;
}

int WINAPI GetPixelFormat(HDC hdc)
{
   {
      std::lock_guard<std::mutex> lock(pixel_format_mutex);
      auto it = pixel_formats.find(hdc);
      if (it != pixel_formats.end())
         return it->second.format;
   }
   int count = stw_pixelformat_get_count(hdc);
#ifdef UWP_HDR
   const enum pipe_format default_format = PIPE_FORMAT_R10G10B10A2_UNORM;
#else
   const enum pipe_format default_format = PIPE_FORMAT_B8G8R8A8_UNORM;
#endif
   for (int format = 1; format <= count; ++format) {
      const struct stw_pixelformat_info *info = stw_pixelformat_get_info(format);
      if (info &&
          info->stvis.color_format == default_format &&
          info->pfd.cStencilBits > 0 &&
          (info->pfd.dwFlags & PFD_DOUBLEBUFFER) &&
          !(info->pfd.dwFlags & PFD_SUPPORT_GDI)) {
         std::lock_guard<std::mutex> lock(pixel_format_mutex);
         return pixel_formats.emplace(hdc, uwp_pixel_format{format, false})
            .first->second.format;
      }
   }
   return 0;
}

int DescribePixelFormat(
   HDC                     hdc,
   int                     iPixelFormat,
   UINT                    nBytes,
   LPPIXELFORMATDESCRIPTOR ppfd
)
{

   int count = stw_pixelformat_get_count(hdc);
   if (!ppfd)
      return count;
   if (iPixelFormat > 0 && nBytes >= sizeof(*ppfd) && count > 0)
   {
      const struct stw_pixelformat_info * info = stw_pixelformat_get_info(iPixelFormat);
      if (info && ppfd)
      {
         *ppfd = info->pfd;
         return count;
      }
   }
   return FALSE;
}

BOOL SetPixelFormat(
   HDC                         hdc,
   int                         format,
   const PIXELFORMATDESCRIPTOR* ppfd
)
{
   if (!stw_pixelformat_get_count(hdc) || !stw_pixelformat_get_info(format))
      return FALSE;
   std::lock_guard<std::mutex> lock(pixel_format_mutex);
   auto existing = pixel_formats.find(hdc);
   if (existing != pixel_formats.end() && existing->second.explicitly_set)
      return existing->second.format == format;
   /* A guessed default may be replaced by the host's first explicit choice. */
   pixel_formats[hdc] = {format, true};
   return TRUE;
}

HWND
WINAPI
CreateWindowEx(
   _In_ DWORD dwExStyle,
   _In_opt_ LPCSTR lpClassName,
   _In_opt_ LPCSTR lpWindowName,
   _In_ DWORD dwStyle,
   _In_ int X,
   _In_ int Y,
   _In_ int nWidth,
   _In_ int nHeight,
   _In_opt_ HWND hWndParent,
   _In_opt_ HMENU hMenu,
   _In_opt_ HINSTANCE hInstance,
   _In_opt_ LPVOID lpParam)
{
   return NULL;
}

BOOL
WINAPI
AdjustWindowRectEx(
   _Inout_ LPRECT lpRect,
   _In_ DWORD dwStyle,
   _In_ BOOL bMenu,
   _In_ DWORD dwExStyle)
{
   return TRUE;
}

BOOL
WINAPI
DestroyWindow(
   _In_ HWND hWnd)
{
   return TRUE;
}
