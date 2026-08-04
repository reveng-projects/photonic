// Copyright (c) 2026 Vitaly Chipounov
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

#pragma once

#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

/// @file
/// PixeLINK API 3.x (Pl-prefix, 32-bit, __cdecl)
/// This is an unofficial header that may be incomplete or inaccurate.

// PL_RETURN_CODE is the single status enumeration shared with the driver;
// it lives in the driver folder (solution/photonic/return_code.h).
#include "../photonic/return_code.h"

/// PlGetNumberDevices
///
/// Count the number of attached PixeLINK cameras.
///
/// The count is reported differently depending on the active transport:
///  - WDM/DirectShow transport: *pMatchCount accumulates the number of
///    devices whose FriendlyName equals pNameFilter.  *pDeviceCount is left
///    at 0.
///  - Direct-IOCTL transport: *pDeviceCount receives the device count.
///    pNameFilter and pMatchCount are not accessed.
///
/// Pass the same pointer as pDeviceCount and pMatchCount to retrieve the
/// count regardless of the active transport.
///
/// pFilter       Must be non-NULL, otherwise PL_ERROR_INVALID_PARAM.  Its
///               value is not otherwise used.
///
/// pDeviceCount  Receives the device count in direct-IOCTL mode.  Set to 0
///               on entry, even when the call fails.
///
/// pNameFilter   WDM mode: device FriendlyName to match.
///
/// pMatchCount   WDM mode: incremented once per device whose FriendlyName
///               equals pNameFilter.
///
/// @param pFilter       Must be non-NULL; its value is otherwise unused.
/// @param pDeviceCount  Receives the device count in direct-IOCTL mode.
/// @param pNameFilter   WDM mode: FriendlyName to match.
/// @param pMatchCount   WDM mode: incremented per matching device.
/// @return PL_SUCCESS on success, PL_ERROR_INVALID_PARAM if pFilter is NULL.
PL_RETURN_CODE __cdecl PlGetNumberDevices(LPCSTR pFilter, ULONG *pDeviceCount, LPCSTR pNameFilter, ULONG *pMatchCount);

/// PlInitialize
///
/// Open a camera and return a handle.
///
/// serialNumber  Interpretation depends on the active transport:
///                 - Direct-IOCTL transport: any non-zero value is accepted,
///                   and deviceIndex alone selects the device.
///                 - WDM/DirectShow transport: treated as an LPCSTR
///                   FriendlyName used to match an enumerated camera, so it
///                   MUST be a valid non-NULL string in this mode.  Pass
///                   (ULONG)(LPCSTR)name, since a plain integer faults.
///
/// deviceIndex   0-based index of the device to open.  In direct-IOCTL mode
///               this selects the Nth reported device.  In WDM mode it
///               indexes among the FriendlyName matches.
///
/// phCamera      Receives the camera handle on success.  Set to NULL on
///               failure.
///
/// @param serialNumber  Device serial number (direct-IOCTL) or FriendlyName cast (WDM).
/// @param deviceIndex   0-based index among matching devices.
/// @param phCamera      Receives the camera handle on success, NULL on failure.
/// @return PL_SUCCESS on success, non-zero on failure.
PL_RETURN_CODE __cdecl PlInitialize(ULONG serialNumber, ULONG deviceIndex, HANDLE *phCamera);

/// PlUninitialize
///
/// Close a camera handle.  *phCamera is set to NULL on return.  If *phCamera
/// is already NULL the function returns PL_SUCCESS immediately.
///
/// phCamera  Pointer to the handle returned by PlInitialize.
///
/// @param phCamera  Pointer to the handle to close; set to NULL on return.
/// @return PL_SUCCESS on success.
PL_RETURN_CODE __cdecl PlUninitialize(HANDLE *phCamera);

/// PlEnumAvailableFrameRates
///
/// Enumerate the frame rates the camera can deliver.  NULL hCamera returns
/// PL_ERROR.
///
/// pFrameRates       Caller-supplied array that receives the frame rates.
///
/// pNumberFrameRates On input the array capacity, on output the number of
///                   frame rates written.
///
/// @param hCamera           Camera handle.
/// @param pFrameRates       Caller-supplied array that receives the frame rates.
/// @param pNumberFrameRates On input the array capacity, on output the count written.
/// @return PL_SUCCESS on success, PL_ERROR on NULL handle.
PL_RETURN_CODE __cdecl PlEnumAvailableFrameRates(HANDLE hCamera, float *pFrameRates, ULONG *pNumberFrameRates);

/// Source pixel format selector for PlFormatImage (inputFormat).
/// Selects the raw layout of the source buffer.  Only the three values below
/// are supported.
typedef enum _PL_IMAGE_FORMAT_SRC {
    PL_IMAGE_FORMAT_SRC_MONO = 0x00, ///< mono / 8-bit raw
    PL_IMAGE_FORMAT_SRC_TYPE2 = 0x02,
    PL_IMAGE_FORMAT_SRC_TYPE5 = 0x05,
} PL_IMAGE_FORMAT_SRC;

/// Destination conversion selector for PlFormatImage (outputFormat).
/// Valid combinations with the source format:
///   PL_IMAGE_FORMAT_SRC_MONO:  DST_1, DST_2
///   PL_IMAGE_FORMAT_SRC_TYPE2: DST_1, DST_2
///   PL_IMAGE_FORMAT_SRC_TYPE5: DST_1, DST_2, DST_3
/// Any other (source, destination) pair returns PL_ERROR_INVALID_COUNT.
typedef enum _PL_IMAGE_FORMAT_DST {
    PL_IMAGE_FORMAT_DST_1 = 0x01,
    PL_IMAGE_FORMAT_DST_2 = 0x02,
    PL_IMAGE_FORMAT_DST_3 = 0x03,
} PL_IMAGE_FORMAT_DST;

/// PlFormatImage
///
/// Convert a raw camera image buffer from one pixel layout to another and
/// write the converted pixels into a caller-supplied destination buffer.
/// The source is treated as height rows of rowBytes each.  The mono ->
/// format-1 conversion produces a BMP with bottom-up rows, so the image
/// appears vertically flipped relative to the raw source.
///
/// pSrc            Raw source image buffer.
///
/// inputFormat     Source pixel-format selector (PL_IMAGE_FORMAT_SRC).
///
/// rowBytes        Bytes per image row (row stride).
///
/// height          Number of image rows.
///
/// outputFormat    Destination conversion selector (PL_IMAGE_FORMAT_DST).
///                 Valid values depend on inputFormat (see enum above).
///
/// pDest           Destination buffer that receives the converted pixels.
///
/// pDestBufferSize On input the destination buffer size.  Must be non-NULL.
///                 When the buffer is too small, receives the required size.
///
/// Returns PL_SUCCESS on success, PL_ERROR_INVALID_PARAM when
/// pDestBufferSize is NULL, PL_ERROR_INVALID_COUNT for an unsupported
/// (inputFormat, outputFormat) pair, or PL_ERROR_UNSUPPORTED_FORMAT when the
/// destination buffer is too small (with *pDestBufferSize set to the
/// required size).
///
/// @param pSrc            Raw source image buffer.
/// @param inputFormat     Source pixel-format selector (PL_IMAGE_FORMAT_SRC).
/// @param rowBytes        Bytes per image row.
/// @param height          Number of image rows.
/// @param outputFormat    Destination conversion selector (PL_IMAGE_FORMAT_DST).
/// @param pDest           Destination buffer.
/// @param pDestBufferSize On input the destination buffer size, on output the required size.
/// @return PL_SUCCESS on success, or an error code on failure.
PL_RETURN_CODE __cdecl PlFormatImage(LPVOID pSrc, ULONG inputFormat, ULONG rowBytes, ULONG height, ULONG outputFormat,
                                     LPVOID pDest, ULONG *pDestBufferSize);

/// PlGetCurrentFrameRate
///
/// Retrieve the frame rate the camera is currently delivering.  NULL hCamera
/// returns PL_ERROR.
///
/// pFrameRate  Receives the current frame rate (frames per second).  Matches
///             the float values produced by PlEnumAvailableFrameRates.
///
/// @param hCamera    Camera handle.
/// @param pFrameRate Receives the current frame rate (frames per second).
/// @return PL_SUCCESS on success, PL_ERROR on NULL handle.
PL_RETURN_CODE __cdecl PlGetCurrentFrameRate(HANDLE hCamera, float *pFrameRate);

/// PlGetDeviceInfo
///
/// Retrieve descriptive information about an opened camera.  NULL hCamera
/// returns PL_ERROR.
///
/// pDeviceInfo   Caller-supplied buffer that receives the device-information
///               record.
///
/// pBufferSize   On input the buffer capacity, on output the number of bytes
///               written.
///
/// pType         Selector for the information type / record to retrieve.
///
/// @param hCamera     Camera handle.
/// @param pDeviceInfo Caller-supplied buffer that receives the information record.
/// @param pBufferSize On input the buffer capacity, on output the bytes written.
/// @param pType       Information type/record selector.
/// @return PL_SUCCESS on success, PL_ERROR on NULL handle.
PL_RETURN_CODE __cdecl PlGetDeviceInfo(HANDLE hCamera, LPVOID pDeviceInfo, ULONG *pBufferSize, ULONG pType);

/// PlGetFeature
///
/// Retrieve the current value(s) of a camera feature.
///
/// hCamera   Must be a valid handle: a NULL handle is not rejected and
///           crashes the caller.
///
/// featureId Feature identifier selecting which camera feature to read.
///
/// flags     Feature flags / mode word (e.g. one-push / auto / manual bits).
///
/// pParams   Caller-supplied parameter buffer that receives the feature
///           value(s).
///
/// @param hCamera   Camera handle (must be valid; NULL is not checked).
/// @param featureId Feature identifier.
/// @param flags     Feature flags/mode word.
/// @param pParams   Receives the feature value(s).
/// @return PL_SUCCESS on success, non-zero on failure.
PL_RETURN_CODE __cdecl PlGetFeature(HANDLE hCamera, ULONG featureId, ULONG flags, float *pParams);

/// PlSetFeature
///
/// Set the current value(s) of a camera feature.
///
/// hCamera   Must be a valid handle: a NULL handle is not rejected and
///           crashes the caller.
///
/// featureId Feature identifier selecting which camera feature to set.
///
/// flags     Feature flags / mode word (e.g. one-push / auto / manual bits).
///
/// pParams   Caller-supplied parameter buffer holding the feature value(s)
///           to apply.
///
/// @param hCamera   Camera handle (must be valid; NULL is not checked).
/// @param featureId Feature identifier.
/// @param flags     Feature flags/mode word.
/// @param pParams   Feature value(s) to apply.
/// @return PL_SUCCESS on success, non-zero on failure.
PL_RETURN_CODE __cdecl PlSetFeature(HANDLE hCamera, ULONG featureId, ULONG flags, float *pParams);

/// PlGetPixelFormat
///
/// Retrieve the current pixel-format code of an opened camera.  NULL hCamera
/// returns PL_ERROR.
///
/// pPixelFormat   Receives the current pixel-format code.  NULL returns
///                PL_ERROR_INVALID_PARAM.
///
/// @param hCamera      Camera handle.
/// @param pPixelFormat Receives the current pixel-format code.
/// @return PL_SUCCESS on success, PL_ERROR on NULL handle, PL_ERROR_INVALID_PARAM if pPixelFormat is NULL.
PL_RETURN_CODE __cdecl PlGetPixelFormat(HANDLE hCamera, ULONG *pPixelFormat);

/// PlSetPixelFormat
///
/// Set the pixel-format code of an opened camera.  NULL hCamera returns
/// PL_ERROR.
///
/// pixelFormat    New pixel-format code, passed by value.
///
/// @param hCamera     Camera handle.
/// @param pixelFormat New pixel-format code.
/// @return PL_SUCCESS on success, PL_ERROR on NULL handle.
PL_RETURN_CODE __cdecl PlSetPixelFormat(HANDLE hCamera, ULONG pixelFormat);

/// PlGetPreviewWindow
///
/// Retrieve the on-screen rectangle of the camera's preview window.  NULL
/// hCamera returns PL_ERROR.
///
/// pPreviewRect  Receives the preview window's screen coordinates (left,
///               top, right, bottom).  NULL returns PL_ERROR_INVALID_PARAM.
///               Returns PL_ERROR_NO_STREAM when no preview window exists
///               and PL_ERROR_NO_PREVIEW when the window placement cannot be
///               queried.
///
/// @param hCamera      Camera handle.
/// @param pPreviewRect Receives the preview window's screen coordinates.
/// @return PL_SUCCESS on success, PL_ERROR on NULL handle, PL_ERROR_INVALID_PARAM if pPreviewRect is NULL.
PL_RETURN_CODE __cdecl PlGetPreviewWindow(HANDLE hCamera, RECT *pPreviewRect);

/// PlSetPreviewWindow
///
/// Set the on-screen position of the camera's preview window.  NULL hCamera
/// returns PL_ERROR.
///
/// pPreviewPos   Desired top-left screen position (x, y) of the preview
///               window.  NULL returns PL_ERROR_INVALID_PARAM.  Returns
///               PL_SUCCESS when the window is moved, and
///               PL_ERROR_NO_STREAM when no preview window currently exists.
///
/// @param hCamera     Camera handle.
/// @param pPreviewPos Desired top-left screen position of the preview window.
/// @return PL_SUCCESS on success, PL_ERROR on NULL handle.
PL_RETURN_CODE __cdecl PlSetPreviewWindow(HANDLE hCamera, POINT *pPreviewPos);

/// PlResetPreviewWindow
///
/// Reset the camera's preview window to its default on-screen placement.
/// Has no effect when no preview window exists.  NULL hCamera returns
/// PL_ERROR.
///
/// @param hCamera  Camera handle.
/// @return PL_SUCCESS on success, PL_ERROR on NULL handle.
PL_RETURN_CODE __cdecl PlResetPreviewWindow(HANDLE hCamera);

/// Preview state values accepted by PlSetPreviewState.
///
///   - PL_PREVIEW_START: creates the preview window (if needed) and starts
///     or resumes previewing.
///   - PL_PREVIEW_PAUSE: freezes a running preview.
///   - PL_PREVIEW_STOP: stops the preview and destroys the window.
///
/// WARNING: value 1 is INVALID and must never be passed: the call never
/// returns and the calling thread hangs.  The only valid values are 0, 2 and
/// 3.  Resume from the paused state is requested with PL_PREVIEW_START, not
/// a distinct value.
typedef enum _PL_PREVIEW_STATE {
    PL_PREVIEW_STOP = 0,  ///< 0x00 - stop preview, destroy window
    PL_PREVIEW_START = 2, ///< 0x02 - start/run preview (also resumes)
    PL_PREVIEW_PAUSE = 3, ///< 0x03 - pause (freeze) a running preview
                          ///< NOTE: value 1 is invalid; passing it hangs the caller (see above).
} PL_PREVIEW_STATE;

/// PlSetPreviewState
///
/// Start, stop, or pause the camera's on-screen preview, creating or
/// destroying the preview window as required.  NULL hCamera returns
/// PL_ERROR.
///
/// previewState  Desired preview state, one of PL_PREVIEW_STATE.  Returns
///               PL_SUCCESS on a successful transition (or when already in
///               the requested state), PL_ERROR_INVALID_COUNT when the
///               preview window could not be created, or another non-zero
///               status on camera failure.  See the PL_PREVIEW_STATE
///               warning: the value 1 must never be passed.
///
/// @param hCamera      Camera handle.
/// @param previewState Desired preview state (PL_PREVIEW_STATE).
/// @return PL_SUCCESS on success, non-zero on failure.
PL_RETURN_CODE __cdecl PlSetPreviewState(HANDLE hCamera, PL_PREVIEW_STATE previewState);

/// PlStartPreview
///
/// Create the camera's on-screen preview window and start previewing.
///
/// The window is created with the given title, style, and position.  The
/// requested width/height size the preview rectangle after division by the
/// camera's decimation factors, and 0xFFFFFFFF selects the camera's native
/// sensor dimension instead.
///
/// Returns PL_SUCCESS on success, PL_ERROR_INVALID_PARAM when windowTitle is
/// NULL, PL_ERROR_NOT_PREPARED when a preview window already exists, or
/// PL_ERROR_INVALID_COUNT when window creation fails.  NULL hCamera returns
/// PL_ERROR.
///
/// windowTitle   Window title.  Must be non-NULL.
///
/// windowStyle   Win32 window style flags.  Use WS_CHILD | WS_VISIBLE with a
///               valid hWndParent: creating the preview with a top-level /
///               overlapped style fails.
///
/// posX          Window left position, in screen/parent coordinates.
///
/// posY          Window top position, in screen/parent coordinates.
///
/// width         Requested preview width, or 0xFFFFFFFF for the camera's
///               native sensor width.  Note the argument order: width comes
///               before height.
///
/// height        Requested preview height, or 0xFFFFFFFF for the camera's
///               native sensor height.
///
/// hWndParent    Parent window handle.  Required (and must be a valid HWND)
///               for a WS_CHILD preview window.
///
/// controlId     Child-window identifier.  Any value (e.g. 0) is accepted
///               for a WS_CHILD window.
///
/// @param hCamera      Camera handle.
/// @param windowTitle  Window title (must be non-NULL).
/// @param windowStyle  Win32 window style flags.
/// @param posX         Window left position.
/// @param posY         Window top position.
/// @param width        Preview width, or 0xFFFFFFFF for the native sensor width.
/// @param height       Preview height, or 0xFFFFFFFF for the native sensor height.
/// @param hWndParent   Parent window handle.
/// @param controlId    Child-window identifier.
/// @return PL_SUCCESS on success, non-zero on failure.
PL_RETURN_CODE __cdecl PlStartPreview(HANDLE hCamera, LPCSTR windowTitle, ULONG windowStyle, ULONG posX, ULONG posY,
                                      ULONG width, ULONG height, HWND hWndParent, ULONG controlId);

/// PlStartVideoStream
///
/// Start the camera's video (frame) stream.  NULL hCamera returns PL_ERROR.
///
/// @param hCamera  Camera handle.
/// @return PL_SUCCESS on success, PL_ERROR on NULL handle.
PL_RETURN_CODE __cdecl PlStartVideoStream(HANDLE hCamera);

/// PlStopVideoStream
///
/// Stop the camera's video (frame) stream.  NULL hCamera returns PL_ERROR.
///
/// @param hCamera  Camera handle.
/// @return PL_SUCCESS on success, PL_ERROR on NULL handle.
PL_RETURN_CODE __cdecl PlStopVideoStream(HANDLE hCamera);

/// PlSetCurrentFrameRate
///
/// Set the frame rate the camera should deliver.  NULL hCamera returns
/// PL_ERROR.
///
/// frameRate   New frame rate (frames per second), passed by value.  Matches
///             the float values produced by PlEnumAvailableFrameRates.
///
/// @param hCamera   Camera handle.
/// @param frameRate New frame rate (frames per second).
/// @return PL_SUCCESS on success, PL_ERROR on NULL handle.
PL_RETURN_CODE __cdecl PlSetCurrentFrameRate(HANDLE hCamera, float frameRate);

/// PlReturnVideoData
///
/// Copy the most recently captured video frame into a caller-supplied
/// buffer.  NULL hCamera returns PL_ERROR.
///
/// Note the argument order: the buffer size comes before the destination
/// pointer.
///
/// bufferSize   Number of bytes to copy.  Must equal the frame size exactly:
///              the copy is not clamped to the frame, so a larger value
///              faults.
///
/// pBuffer      Destination buffer that receives the frame data.
///
/// @param hCamera    Camera handle.
/// @param bufferSize Number of bytes to copy; must equal the frame size exactly.
/// @param pBuffer    Destination buffer that receives the frame data.
/// @return PL_SUCCESS on success, PL_ERROR on NULL handle.
PL_RETURN_CODE __cdecl PlReturnVideoData(HANDLE hCamera, ULONG bufferSize, void *pBuffer);

/// PlGetSubWindowSettings
///
/// Read the camera's current sub-window (region-of-interest, ROI) settings.
/// NULL hCamera returns PL_ERROR.
///
/// pDecimationX Receives the horizontal decimation/binning factor.
///
/// pDecimationY Receives the vertical decimation/binning factor.
///
/// pOffsetX     Receives the ROI horizontal origin (left/X start), in pixels
///              (already multiplied by the horizontal decimation factor).
///
/// pOffsetY     Receives the ROI vertical origin (top/Y start), in pixels
///              (already multiplied by the vertical decimation factor).
///
/// pWidth       Receives the ROI width, in pixels (already multiplied by the
///              horizontal decimation factor).
///
/// pHeight      Receives the ROI height, in pixels (already multiplied by the
///              vertical decimation factor).
///
/// @param hCamera      Camera handle.
/// @param pDecimationX Receives the horizontal decimation/binning factor.
/// @param pDecimationY Receives the vertical decimation/binning factor.
/// @param pOffsetX    Receives the ROI horizontal origin in pixels.
/// @param pOffsetY    Receives the ROI vertical origin in pixels.
/// @param pWidth      Receives the ROI width in pixels.
/// @param pHeight     Receives the ROI height in pixels.
/// @return PL_SUCCESS on success, PL_ERROR on NULL handle.
PL_RETURN_CODE __cdecl PlGetSubWindowSettings(HANDLE hCamera, ULONG *pDecimationX, ULONG *pDecimationY, ULONG *pOffsetX,
                                              ULONG *pOffsetY, ULONG *pWidth, ULONG *pHeight);

/// PlSetSubWindowSettings
///
/// Write the camera's sub-window (region-of-interest, ROI) settings.  Takes
/// the same six ROI values as PlGetSubWindowSettings, in the same order,
/// passed by value.  NULL hCamera returns PL_ERROR.
///
/// decimationX  Horizontal decimation/binning factor.
///
/// decimationY  Vertical decimation/binning factor.
///
/// offsetX      ROI horizontal origin (left/X start), in pixels.
///
/// offsetY      ROI vertical origin (top/Y start), in pixels.
///
/// width        ROI width, in pixels.
///
/// height       ROI height, in pixels.
///
/// @param hCamera     Camera handle.
/// @param decimationX Horizontal decimation/binning factor.
/// @param decimationY Vertical decimation/binning factor.
/// @param offsetX     ROI horizontal origin in pixels.
/// @param offsetY     ROI vertical origin in pixels.
/// @param width       ROI width in pixels.
/// @param height      ROI height in pixels.
/// @return PL_SUCCESS on success, PL_ERROR on NULL handle.
PL_RETURN_CODE __cdecl PlSetSubWindowSettings(HANDLE hCamera, ULONG decimationX, ULONG decimationY, ULONG offsetX,
                                              ULONG offsetY, ULONG width, ULONG height);

/// PlGetTimeout
///
/// Read the camera's command/operation timeout value.  NULL hCamera returns
/// PL_ERROR.
///
/// pTimeout  Receives the current timeout value (milliseconds).
///
/// @param hCamera  Camera handle.
/// @param pTimeout Receives the current timeout (milliseconds).
/// @return PL_SUCCESS on success, PL_ERROR on NULL handle.
PL_RETURN_CODE __cdecl PlGetTimeout(HANDLE hCamera, ULONG *pTimeout);

/// PlSetTimeout
///
/// Set the camera's command/operation timeout value.  NULL hCamera returns
/// PL_ERROR.
///
/// timeout   New timeout value (milliseconds), passed by value.
///
/// @param hCamera  Camera handle.
/// @param timeout  New timeout value (milliseconds).
/// @return PL_SUCCESS on success, PL_ERROR on NULL handle.
PL_RETURN_CODE __cdecl PlSetTimeout(HANDLE hCamera, ULONG timeout);

/// PlSetTriggerMode
///
/// Set the camera's trigger mode.  NULL hCamera returns PL_ERROR.
///
/// triggerMode New trigger mode, passed by value.  No trigger-mode constants
///             are defined in this header.
///
/// @param hCamera     Camera handle.
/// @param triggerMode New trigger mode.
/// @return PL_SUCCESS on success, PL_ERROR on NULL handle.
PL_RETURN_CODE __cdecl PlSetTriggerMode(HANDLE hCamera, ULONG triggerMode);

/// PlReadExtI2cRegister
///
/// Read a register from an external I2C device attached to the camera.  NULL
/// hCamera returns PL_ERROR.
///
/// i2cAddress      I2C slave/device address selecting the external device.
///
/// registerAddress Register address within the I2C device to read.
///
/// pValue          Receives the register value read from the device.
///
/// @param hCamera          Camera handle.
/// @param i2cAddress       I2C slave address of the external device.
/// @param registerAddress  Register address within the I2C device.
/// @param pValue           Receives the register value.
/// @return PL_SUCCESS on success, PL_ERROR on NULL handle.
PL_RETURN_CODE __cdecl PlReadExtI2cRegister(HANDLE hCamera, ULONG i2cAddress, ULONG registerAddress, ULONG *pValue);

/// PlWriteExtI2cRegister
///
/// Write a register on an external I2C device attached to the camera.  NULL
/// hCamera returns PL_ERROR.
///
/// i2cAddress      I2C slave/device address selecting the external device.
///
/// registerAddress Register address within the I2C device to write.
///
/// value           Register value to write to the device, passed by value.
///
/// @param hCamera          Camera handle.
/// @param i2cAddress       I2C slave address of the external device.
/// @param registerAddress  Register address within the I2C device.
/// @param value            Register value to write.
/// @return PL_SUCCESS on success, PL_ERROR on NULL handle.
PL_RETURN_CODE __cdecl PlWriteExtI2cRegister(HANDLE hCamera, ULONG i2cAddress, ULONG registerAddress, ULONG value);

#ifdef __cplusplus
}
#endif
