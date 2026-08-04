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

#ifndef PHOTONIC_RETURN_CODE_H
#define PHOTONIC_RETURN_CODE_H

/// @file
/// PL_RETURN_CODE -- unified PixeLINK API 3.x return / status code space.
///
/// These codes are shared by the driver and pixelinkapi.dll. Codes generated
/// only inside the DLL (the driver never writes them) are marked "DLL-only";
/// codes only the driver writes are marked "driver-only".
typedef enum _PL_RETURN_CODE {
    PL_SUCCESS = 0x00, ///< operation succeeded

    PL_ERROR = 0x01,                     ///< DLL-only: general failure (NULL handle / I/O)
                                         ///<  ~ApiInvalidHandleError
    PL_ERROR_INVALID_PARAM = 0x02,       ///< DLL-only: NULL pointer argument (pre-IOCTL
                                         ///<  check) ~ApiNullPointerError
    PL_ERROR_DEVICE_NOT_FOUND = 0x03,    ///< DLL: no device at requested index; driver:
                                         ///<  surprise removal ~ApiNoDeviceError
    PL_ERROR_WDM_INIT = 0x04,            ///< DLL-only: CoCreateInstance / WDM init failed
                                         ///<  ~ApiGevInitializationError
    PL_ERROR_INVALID_COUNT = 0x07,       ///< DLL: zero count/size arg; driver: index out
                                         ///<  of range (serial idx>=2, feature id)
    PL_ERROR_OUT_OF_MEMORY = 0x09,       ///< DLL: malloc NULL; driver: pool / MDL
                                         ///<  allocation failed ~ApiOutOfMemoryError
    PL_ERROR_NO_PREVIEW = 0x0A,          ///< DLL: preview-window op failed; driver:
                                         ///<  frame-rate query failed (reused)
    PL_ERROR_NO_STREAM = 0x0B,           ///< DLL-only: no active stream / preview window
                                         ///<  ~ApiNoStreamError
    PL_ERROR_FEATURE_NOT_PRESENT = 0x0C, ///< driver-only: feature PRESENT bit clear
                                         ///<  ~ApiNotSupportedError
    PL_ERROR_NOT_PREPARED = 0x0D,        ///< DLL: stream state < prepared; driver: SW
                                         ///<  trigger w/o prepared ctx ~ApiCameraNotReady
    PL_ERROR_HARDWARE = 0x0E,            ///< driver-only: underlying DCS / device command
                                         ///<  failed ~ApiHardwareError
    PL_ERROR_UNSUPPORTED_XPORT = 0x0F,   ///< DLL-only: unsupported transport mode
    PL_ERROR_TIMEOUT = 0x12,             ///< DLL-only: WaitForSingleObject timed out
                                         ///<  ~ApiTimeoutError
    PL_ERROR_OVERFLOW = 0x13,            ///< DLL-only: too many pending frames
    PL_ERROR_OUT_OF_RANGE = 0x14,        ///< driver-only: frame-rate enumeration index
                                         ///<  exhausted ~ApiOutOfRangeError / ApiEnumDoneError
    PL_ERROR_UNSUPPORTED_FORMAT = 0x15,  ///< DLL-only: pixel format / conversion not
                                         ///<  supported ~ApiUnsupportedPixelFormatError
    PL_ERROR_NO_RESOURCES = 0x16,        ///< driver-only: capture resource acquisition
                                         ///<  failed ~ApiNotEnoughResourcesError
    PL_ERROR_INVALID_STATE = 0x17,       ///< driver-only: stream busy / wrong state for
                                         ///<  the request ~ApiNotPermittedWhileStreaming
    PL_ERROR_BAD_FRAME_SIZE = 0x19,      ///< driver-only: mapped frame size != prepared
                                         ///<  size ~ApiBadFrameSizeError
    PL_ERROR_LOCK_FAILED = 0x1A,         ///< driver-only: MmProbeAndLockPages on user
                                         ///<  buffer failed ~ApiFrameInUseError
    PL_ERROR_ISOCH_ATTACH = 0x1B,        ///< driver-only: isochronous attach failed
                                         ///<  ~ApiOutOfBandwidthError
    PL_ERROR_NOT_STREAMING = 0x1F,       ///< driver-only: no video stream context / no
                                         ///<  frames allocated
    PL_ERROR_FORMAT_UNAVAILABLE = 0x20,  ///< DLL: serial-number IOCTL failed at init;
                                         ///<  driver: invalid event Type (reused)
    PL_ERROR_SIZE_MISMATCH = 0x24,       ///< DLL-only: returned data size does not match
                                         ///<  expected
} PL_RETURN_CODE;

#endif // PHOTONIC_RETURN_CODE_H
