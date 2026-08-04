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

/// @file
/// Photonic driver IOCTL interface
///
/// This header defines the IOCTL codes exchanged between user-space components
/// and the photonic driver. It is intended to be shared by both sides.
///
/// It does NOT include any platform headers so that it can be consumed from
/// either context. The includer must provide CTL_CODE and the associated
/// constants beforehand:
///   - Kernel mode: #include <wdm.h> (or <ntddk.h>)
///   - User  mode:  #include <windows.h> (or <winioctl.h>)

/// IOCTL codes

/// All codes use device type FILE_DEVICE_UNKNOWN (0x22), METHOD_BUFFERED and
/// FILE_ANY_ACCESS; only the function code varies.
#define PHOTONIC_IOCTL_SDK_VERSION_1 \
    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x806, METHOD_BUFFERED, FILE_ANY_ACCESS) ///< 0x222018
#define PHOTONIC_IOCTL_SDK_VERSION_2 \
    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x807, METHOD_BUFFERED, FILE_ANY_ACCESS) ///< 0x22201C
#define PHOTONIC_IOCTL_SERIAL_NUMBER \
    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x809, METHOD_BUFFERED, FILE_ANY_ACCESS) ///< 0x222024
#define PHOTONIC_IOCTL_SET_CANCEL_TIMEOUT \
    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x80B, METHOD_BUFFERED, FILE_ANY_ACCESS) ///< 0x22202C
#define PHOTONIC_IOCTL_GET_VENDOR_NAME \
    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x80C, METHOD_BUFFERED, FILE_ANY_ACCESS) ///< 0x222030
#define PHOTONIC_IOCTL_GET_MODEL_NAME \
    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x80D, METHOD_BUFFERED, FILE_ANY_ACCESS) ///< 0x222034
#define PHOTONIC_IOCTL_SUBWINDOW_GET \
    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x835, METHOD_BUFFERED, FILE_ANY_ACCESS) ///< 0x2220D4
#define PHOTONIC_IOCTL_SUBWINDOW_SET \
    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x836, METHOD_BUFFERED, FILE_ANY_ACCESS)                                ///< 0x2220D8
#define PHOTONIC_IOCTL_START_VIDEO CTL_CODE(FILE_DEVICE_UNKNOWN, 0x837, METHOD_BUFFERED, FILE_ANY_ACCESS) ///< 0x2220DC
#define PHOTONIC_IOCTL_STOP_VIDEO  CTL_CODE(FILE_DEVICE_UNKNOWN, 0x838, METHOD_BUFFERED, FILE_ANY_ACCESS) ///< 0x2220E0
#define PHOTONIC_IOCTL_REGISTER_EVENT \
    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x839, METHOD_BUFFERED, FILE_ANY_ACCESS) ///< 0x2220E4
#define PHOTONIC_IOCTL_UNREGISTER_EVENT \
    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x83A, METHOD_BUFFERED, FILE_ANY_ACCESS) ///< 0x2220E8
#define PHOTONIC_IOCTL_GET_TRANSFER_INFO \
    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x83B, METHOD_BUFFERED, FILE_ANY_ACCESS) ///< 0x2220EC
#define PHOTONIC_IOCTL_PREPARE_VIDEO \
    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x83C, METHOD_BUFFERED, FILE_ANY_ACCESS) ///< 0x2220F0
#define PHOTONIC_IOCTL_UNPREPARE_VIDEO \
    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x83D, METHOD_BUFFERED, FILE_ANY_ACCESS) ///< 0x2220F4
#define PHOTONIC_IOCTL_MAP_VIDEO_FRAME \
    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x83E, METHOD_BUFFERED, FILE_ANY_ACCESS) ///< 0x2220F8
#define PHOTONIC_IOCTL_UNMAP_VIDEO_FRAME \
    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x83F, METHOD_BUFFERED, FILE_ANY_ACCESS)                                 ///< 0x2220FC
#define PHOTONIC_IOCTL_INITIALIZE   CTL_CODE(FILE_DEVICE_UNKNOWN, 0x845, METHOD_BUFFERED, FILE_ANY_ACCESS) ///< 0x222114
#define PHOTONIC_IOCTL_PROPERTY_GET CTL_CODE(FILE_DEVICE_UNKNOWN, 0x846, METHOD_BUFFERED, FILE_ANY_ACCESS) ///< 0x222118
#define PHOTONIC_IOCTL_PROPERTY_SET CTL_CODE(FILE_DEVICE_UNKNOWN, 0x847, METHOD_BUFFERED, FILE_ANY_ACCESS) ///< 0x22211C
#define PHOTONIC_IOCTL_TRIGGER_SET  CTL_CODE(FILE_DEVICE_UNKNOWN, 0x848, METHOD_BUFFERED, FILE_ANY_ACCESS) ///< 0x222120
#define PHOTONIC_IOCTL_STROBE_SET   CTL_CODE(FILE_DEVICE_UNKNOWN, 0x849, METHOD_BUFFERED, FILE_ANY_ACCESS) ///< 0x222124
#define PHOTONIC_IOCTL_SW_TRIGGER   CTL_CODE(FILE_DEVICE_UNKNOWN, 0x84A, METHOD_BUFFERED, FILE_ANY_ACCESS) ///< 0x222128
#define PHOTONIC_IOCTL_ENUM_FRAME_RATE \
    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x84B, METHOD_BUFFERED, FILE_ANY_ACCESS) ///< 0x22212C
#define PHOTONIC_IOCTL_SET_FRAME_RATE \
    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x84C, METHOD_BUFFERED, FILE_ANY_ACCESS)                               ///< 0x222130
#define PHOTONIC_IOCTL_IMAGE_FLIP CTL_CODE(FILE_DEVICE_UNKNOWN, 0x84D, METHOD_BUFFERED, FILE_ANY_ACCESS) ///< 0x222134
#define PHOTONIC_IOCTL_GET_FRAME_RATE \
    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x84F, METHOD_BUFFERED, FILE_ANY_ACCESS)                                ///< 0x22213C
#define PHOTONIC_IOCTL_GET_CHANNEL CTL_CODE(FILE_DEVICE_UNKNOWN, 0x850, METHOD_BUFFERED, FILE_ANY_ACCESS) ///< 0x222140
#define PHOTONIC_IOCTL_GET_PACKET_SIZE \
    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x854, METHOD_BUFFERED, FILE_ANY_ACCESS) ///< 0x222150
#define PHOTONIC_IOCTL_GET_PIXEL_FORMAT \
    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x856, METHOD_BUFFERED, FILE_ANY_ACCESS) ///< 0x222158
#define PHOTONIC_IOCTL_SET_PIXEL_FORMAT \
    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x857, METHOD_BUFFERED, FILE_ANY_ACCESS) ///< 0x22215C
#define PHOTONIC_IOCTL_PREPARE_IMAGER \
    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x858, METHOD_BUFFERED, FILE_ANY_ACCESS) ///< 0x222160
#define PHOTONIC_IOCTL_UNPREPARE_IMAGER \
    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x859, METHOD_BUFFERED, FILE_ANY_ACCESS)                                 ///< 0x222164
#define PHOTONIC_IOCTL_START_IMAGER CTL_CODE(FILE_DEVICE_UNKNOWN, 0x85A, METHOD_BUFFERED, FILE_ANY_ACCESS) ///< 0x222168
#define PHOTONIC_IOCTL_STOP_IMAGER  CTL_CODE(FILE_DEVICE_UNKNOWN, 0x85B, METHOD_BUFFERED, FILE_ANY_ACCESS) ///< 0x22216C
#define PHOTONIC_IOCTL_GET_DCAM_VERSION \
    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x85C, METHOD_BUFFERED, FILE_ANY_ACCESS) ///< 0x222170
#define PHOTONIC_IOCTL_GET_NAMES_LENGTH \
    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x85D, METHOD_BUFFERED, FILE_ANY_ACCESS) ///< 0x222174
#define PHOTONIC_IOCTL_INVALIDATE_FORMAT \
    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x85E, METHOD_BUFFERED, FILE_ANY_ACCESS) ///< 0x222178
/// Returns the newline-delimited device-names buffer whose length is reported by
/// PHOTONIC_IOCTL_GET_NAMES_LENGTH. Not handled by the photonic driver's dispatch;
/// issued by pixelinkapi.dll (PlGetNumberDevices counts the entries,
/// PlInitialize selects the Nth entry as the device path for CreateFileA).
#define PHOTONIC_IOCTL_GET_NAMES CTL_CODE(FILE_DEVICE_UNKNOWN, 0x8F3, METHOD_BUFFERED, FILE_ANY_ACCESS) ///< 0x2223CC
#define PHOTONIC_IOCTL_GET_LAST_ERROR \
    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x8FE, METHOD_BUFFERED, FILE_ANY_ACCESS)                            ///< 0x2223F8
#define PHOTONIC_IOCTL_MAILBOX CTL_CODE(FILE_DEVICE_UNKNOWN, 0xDC6, METHOD_BUFFERED, FILE_ANY_ACCESS) ///< 0x223718

/// Status / error codes

/// The driver writes these into both the per-IOCTL Status field (buf[0] of the
/// shared METHOD_BUFFERED buffer) and DevExt->LastError (returned by
/// PHOTONIC_IOCTL_GET_LAST_ERROR).  pixelinkapi.dll forwards them to the
/// application unchanged as PL_RETURN_CODE, so the driver and the user-space SDK
/// share ONE enumeration, defined in the common header below.  The enum is 32
/// bits wide (matching the original UINT32 fields) and is used in place of
/// UINT32 wherever a structure field carries one of these codes.
// clang-format off
#include "return_code.h"
// clang-format on

/// IOCTL data structures

/// For each IOCTL below: the input buffer (driver IN), the output buffer
/// (driver OUT) and the user-space API(s) that trigger it.  All buffers use
/// METHOD_BUFFERED.  Structures are #pragma pack(1) because they cross the
/// user/kernel boundary verbatim.  Field offsets are noted in comments.
///
/// 32-bit (WOW64) ABI: the only client is the 32-bit pixelinkapi.dll, so every
/// pointer/handle a client passes is 32 bits wide on the wire.  Structures whose
/// layout therefore depends on caller bitness carry a "32" suffix (e.g.
/// PHOTONIC_*_IN32) and declare those fields as UINT32; the driver zero-extends
/// them to native PVOID/HANDLE.  The driver rejects native 64-bit callers
/// (IoIs32bitProcess) in PhotonicDeviceControl, so these structs are never used
/// with a 64-bit layout.  Structures with no pointer/handle field have identical
/// layout in both bitnesses and keep their plain names.
#pragma pack(push, 1)

/// SECTION 1: Identity / version / device naming
///   IOCTLs: SDK_VERSION_1, SDK_VERSION_2, SERIAL_NUMBER, SET_CANCEL_TIMEOUT,
///           GET_VENDOR_NAME, GET_MODEL_NAME, GET_DCAM_VERSION,
///           GET_NAMES_LENGTH, GET_NAMES, GET_LAST_ERROR

/// PHOTONIC_IOCTL_SDK_VERSION_1  (0x222018) -- SDK version query (variant 1)
/// PHOTONIC_IOCTL_SDK_VERSION_2  (0x22201C) -- SDK version query (variant 2)
///   Handler:  PhotonicIoctlSdkVersion
///   Input:    none (InputBufferLength ignored)
///   Output:   none -- handler always returns STATUS_NOT_IMPLEMENTED
///   Triggers: not exercised by the DLL (stubs)
/// No in/out structures: the handler is a stub that returns STATUS_NOT_IMPLEMENTED
/// without touching the system buffer.

/// PHOTONIC_IOCTL_SERIAL_NUMBER  (0x222024)
///   Handler:  PhotonicIoctlSerialNumber (stub, returns STATUS_NOT_IMPLEMENTED)
///   Input:    PHOTONIC_SERIAL_NUMBER_IN  (12 bytes / 3 DWORDs)
///   Output:   PHOTONIC_SERIAL_NUMBER_OUT (12 bytes / 3 DWORDs, same buffer)
///   Triggers: PlInitialize; in=0xC, out=0xC
///
/// The DLL sets Index=1 (requesting the high serial-number word).
/// If Index >= 2 the driver sets Status=7 (bad index).
/// Otherwise buf[2] receives the byte-swapped low (Index==0) or high (Index==1)
/// 32-bit serial-number word. The two words are the node EUI-64 (bus-info-block
/// quadlets 3-4), returned byte-swapped here and raw in MAILBOX GET_ADDRESSES.
typedef struct {
    PL_RETURN_CODE
    Status;       ///< offset 0x00 -- driver writes result: SUCCESS or BAD_INDEX
    UINT32 Index; ///< offset 0x04 -- 0=low word, 1=high word (>=2 → error)
    UINT32 Value; ///< offset 0x08 -- driver writes byte-swapped serial-number word
} PHOTONIC_SERIAL_NUMBER_IN, PHOTONIC_SERIAL_NUMBER_OUT;

/// PHOTONIC_IOCTL_SET_CANCEL_TIMEOUT  (0x22202C)
///   Handler:  PhotonicIoctlSetCancelTimeout (stub, returns STATUS_NOT_IMPLEMENTED)
///   Input:    PHOTONIC_SET_CANCEL_TIMEOUT_IN  (12 bytes / 3 DWORDs)
///   Output:   PHOTONIC_SET_CANCEL_TIMEOUT_OUT (same buffer, driver writes Status)
///   Triggers: PlSetTimeout
///
/// buf[2] carries the timeout in milliseconds.  The driver converts it to
/// 100-ns units (×10 000) and stores it as the cancel timeout.
/// buf[0] is set to 0 on success.
typedef struct {
    PL_RETURN_CODE Status; ///< offset 0x00 -- in: ignored; out: SUCCESS
    UINT32 Reserved;       ///< offset 0x04 -- not used by driver
    UINT32 TimeoutMs;      ///< offset 0x08 -- cancel timeout in milliseconds
} PHOTONIC_SET_CANCEL_TIMEOUT_IN, PHOTONIC_SET_CANCEL_TIMEOUT_OUT;

/// PHOTONIC_IOCTL_GET_VENDOR_NAME  (0x222030)
///   Handler:  PhotonicIoctlGetVendorName (stub, returns STATUS_NOT_IMPLEMENTED)
///   Input:    none (InputBufferLength ignored)
///   Output:   PHOTONIC_GET_VENDOR_NAME_OUT -- variable-length ANSI string
///             (the 12-byte TEXTUAL_LEAF header of the Config ROM leaf is
///              stripped before copying)
///   Triggers: PlGetDeviceInfo
///
/// The output buffer receives the raw vendor-name string with no NUL terminator
/// added by the driver; the caller must size the buffer appropriately.
typedef struct {
    UINT8 Name[1]; ///< offset 0x00 -- vendor name string (variable length, not
                   ///< NUL-terminated)
} PHOTONIC_GET_VENDOR_NAME_OUT;

/// PHOTONIC_IOCTL_GET_MODEL_NAME  (0x222034)
///   Handler:  PhotonicIoctlGetModelName (stub, returns STATUS_NOT_IMPLEMENTED)
///   Input:    none (InputBufferLength ignored)
///   Output:   PHOTONIC_GET_MODEL_NAME_OUT -- variable-length ANSI string
///             (same TEXTUAL_LEAF strip)
///   Triggers: PlGetDeviceInfo
typedef struct {
    UINT8 Name[1]; ///< offset 0x00 -- model name string (variable length, not
                   ///< NUL-terminated)
} PHOTONIC_GET_MODEL_NAME_OUT;

/// PHOTONIC_IOCTL_GET_DCAM_VERSION  (0x222170)
///   Handler:  PhotonicIoctlGetDcamVersion (stub, returns STATUS_NOT_IMPLEMENTED)
///   Input:    none (InputBufferLength ignored)
///   Output:   PHOTONIC_GET_DCAM_VERSION_OUT (4 bytes)
///   Triggers: DLL-internal, during device initialization; in=0, out=4
///
/// Returns the unit directory's unit_sw_version, the implemented DCAM
/// specification revision (the 0x000100 / 0x000101 / 0x000102 family). It is
/// not a CSR address.
typedef struct {
    UINT32 UnitSwVersion; ///< offset 0x00 -- DCAM unit_sw_version from the
                          ///< configuration ROM unit directory
} PHOTONIC_GET_DCAM_VERSION_OUT;

/// PHOTONIC_IOCTL_GET_NAMES_LENGTH  (0x222174)
///   Handler:  PhotonicIoctlGetNamesLength (stub, returns STATUS_NOT_IMPLEMENTED)
///   Input:    none (InputBufferLength ignored)
///   Output:   PHOTONIC_GET_NAMES_LENGTH_OUT (up to 8 bytes)
///             buf[0] written if OutputBufferLength >= 4
///             buf[1] written if OutputBufferLength >= 8
///   Triggers: not exercised by the DLL directly; companion to GET_NAMES
///
/// Returns the byte counts of the vendor-name and model-name strings (after
/// stripping the 12-byte TEXTUAL_LEAF header from each).  A caller uses these
/// lengths to size the buffers for GET_VENDOR_NAME / GET_MODEL_NAME.
typedef struct {
    UINT32 VendorNameLen; ///< offset 0x00 -- length of vendor name string in bytes
                          ///< (0 if absent)
    UINT32 ModelNameLen;  ///< offset 0x04 -- length of model name string in bytes (0
                          ///< if absent)
} PHOTONIC_GET_NAMES_LENGTH_OUT;

/// PHOTONIC_IOCTL_GET_NAMES  (0x2223CC)
///   Handler:  NOT handled by the photonic driver dispatch table
///   Input:    PHOTONIC_GET_NAMES_IN  (0x400 bytes)
///   Output:   PHOTONIC_GET_NAMES_OUT (0x400 bytes, same buffer)
///   Triggers: PlGetNumberDevices (and the DLL's device enumeration)
///
/// Issued directly to the "\\\\.\\vitdcam" device (opened with CreateFileA
/// before a camera handle exists).  Returns a newline-delimited list of
/// device path strings; PlGetNumberDevices counts the '\n' separators to
/// determine the number of available cameras, and PlInitialize picks the
/// Nth entry as the path to pass to CreateFileA.
#define PHOTONIC_GET_NAMES_BUF_SIZE 0x400
typedef struct {
    UINT8 Data[PHOTONIC_GET_NAMES_BUF_SIZE]; ///< offset 0x00 -- input: unused (sent as
                                             ///< zeros); output: newline-delimited
                                             ///< device paths
} PHOTONIC_GET_NAMES_IN, PHOTONIC_GET_NAMES_OUT;

/// PHOTONIC_IOCTL_GET_LAST_ERROR  (0x2223F8)
///   Handler:  PhotonicIoctlGetLastError
///   Input:    none (InputBufferLength ignored)
///   Output:   PHOTONIC_GET_LAST_ERROR_OUT (4 bytes)
///   Triggers: DLL-internal; in=0, out=4
///
/// Returns DevExt->LastError -- the driver's internal error code from the last
/// failed operation.  If a surprise-removal is pending the driver first forces
/// LastError=3 (device removed) before returning it.
typedef struct {
    PL_RETURN_CODE LastError; ///< offset 0x00 -- driver's last error code (SUCCESS =
                              ///< no error, DEVICE_REMOVED, etc.)
} PHOTONIC_GET_LAST_ERROR_OUT;

// === END SECTION 1 ===

/// SECTION 2: Sub-window (ROI) / pixel format / channel
///   IOCTLs: SUBWINDOW_GET, SUBWINDOW_SET, GET_PIXEL_FORMAT, SET_PIXEL_FORMAT,
///           IMAGE_FLIP, INVALIDATE_FORMAT, GET_CHANNEL, GET_PACKET_SIZE

/// Shared ROI layout (0x18 bytes, 6 x UINT32)
/// Used by both PHOTONIC_IOCTL_SUBWINDOW_GET and PHOTONIC_IOCTL_SUBWINDOW_SET
/// as an in/out buffer (same structure serves input and output).
///
/// On GET: driver fills all fields; Status is set to 0.
/// On SET: caller fills Scale..Height; Status is reserved/unused on input
///         and set to 0 by the driver on success.
typedef struct _PHOTONIC_SUBWINDOW {
    PL_RETURN_CODE Status; ///< offset 0x00  SUCCESS (output); reserved on input
    UINT32 Scale;          ///< offset 0x04  decimation factor relative to the
                           ///<              full-resolution mode
    UINT32 OffsetX;        ///< offset 0x08  horizontal offset * Scale
    UINT32 OffsetY;        ///< offset 0x0C  vertical offset   * Scale
    UINT32 Width;          ///< offset 0x10  image width  * Scale
    UINT32 Height;         ///< offset 0x14  image height * Scale
} PHOTONIC_SUBWINDOW;

/// PHOTONIC_IOCTL_SUBWINDOW_GET (0x2220D4)
///   Input : PHOTONIC_SUBWINDOW  (0x18 bytes; driver ignores the input contents)
///   Output: PHOTONIC_SUBWINDOW  (0x18 bytes)
///   Handler: PhotonicIoctlSubwindowGet
///   APIs   : PlGetSubWindowSettings
typedef PHOTONIC_SUBWINDOW PHOTONIC_SUBWINDOW_GET_IN;
typedef PHOTONIC_SUBWINDOW PHOTONIC_SUBWINDOW_GET_OUT;

/// PHOTONIC_IOCTL_SUBWINDOW_SET (0x2220D8)
///   Input : PHOTONIC_SUBWINDOW  (0x18 bytes; Scale and Width/Offset/Height used)
///   Output: PHOTONIC_SUBWINDOW  (0x18 bytes; only Status = 0 is meaningful)
///   Handler: PhotonicIoctlSubwindowSet
///   APIs   : PlSetSubWindowSettings
///   Note   : Validate-only: the handler accepts only the current mode's
///            default sub-window and programs no camera register.
typedef PHOTONIC_SUBWINDOW PHOTONIC_SUBWINDOW_SET_IN;
typedef PHOTONIC_SUBWINDOW PHOTONIC_SUBWINDOW_SET_OUT;

/// PHOTONIC_IOCTL_GET_PIXEL_FORMAT (0x222158)
///   Input : none  (0 bytes)
///   Output: 4 bytes
///   Handler: PhotonicIoctlGetPixelFormat
///   APIs   : PlGetPixelFormat

/// (no input structure)
typedef struct _PHOTONIC_GET_PIXEL_FORMAT_OUT {
    UINT32 PixelFormat; ///< offset 0x00  DCAM pixel format code (e.g. 0=Mono8,
                        ///< 2=Mono16, ...)
} PHOTONIC_GET_PIXEL_FORMAT_OUT;

/// PHOTONIC_IOCTL_SET_PIXEL_FORMAT (0x22215C)
///   Input : 4 bytes  (target DCAM pixel format code)
///   Output: 4 bytes  (Status = 0 on success; overwritten into same buffer).
///           Observed at runtime: the DLL passes a zero output length, so the
///           Status echo is never returned to the caller.
///   Handler: PhotonicIoctlSetPixelFormat
///   APIs   : PlSetPixelFormat
///   Note   : The driver scans the format table to find the first entry
///            matching the requested pixel format code, then selects it,
///            preserving the current ROI. The call is rejected (LastError =
///            0x17) while either front-end holds the camera: the DirectShow
///            capture stream is open or a direct-buffer capture slot is
///            prepared.
typedef struct _PHOTONIC_SET_PIXEL_FORMAT_IN {
    UINT32 PixelFormat; ///< offset 0x00  target DCAM pixel format code
} PHOTONIC_SET_PIXEL_FORMAT_IN;
typedef struct _PHOTONIC_SET_PIXEL_FORMAT_OUT {
    PL_RETURN_CODE Status; ///< offset 0x00  SUCCESS
} PHOTONIC_SET_PIXEL_FORMAT_OUT;

/// PHOTONIC_IOCTL_IMAGE_FLIP (0x222134)
///   Input : 8 bytes  (two ULONGs forwarded verbatim to extended command 0xD)
///   Output: same buffer (outLen bytes; no output fields defined by driver)
///   Handler: PhotonicIoctlImageFlip (stub, returns STATUS_NOT_IMPLEMENTED)
///   APIs   : not exercised by the DLL
typedef struct _PHOTONIC_IMAGE_FLIP_IN {
    UINT32 FlipHorizontal; ///< offset 0x00  horizontal flip control word
    UINT32 FlipVertical;   ///< offset 0x04  vertical flip control word
} PHOTONIC_IMAGE_FLIP_IN;
/// (no distinct output structure; on success outLen bytes are echoed back
///  with no modifications to the buffer)

/// PHOTONIC_IOCTL_INVALIDATE_FORMAT (0x222178)
///   Input : none  (0 bytes; DLL passes lpInBuffer=NULL, nInBufferSize=0)
///   Output: none  (0 bytes; DLL passes lpOutBuffer=NULL, nOutBufferSize=0)
///   Handler: PhotonicIoctlInvalidateCurrentFormat
///   APIs   : invoked internally when the DLL resets the current format
///            (PlSetSubWindowSettings path)
///   Note   : The handler is a pure no-op: the driver caches no format (every
///            format query reads the live camera registers), so nothing is
///            dropped and nothing is written.

/// (no input or output structures)

/// PHOTONIC_IOCTL_GET_CHANNEL (0x222140)
///   Input : none / ignored
///   Output: 4 bytes
///   Handler: PhotonicIoctlGetChannel (stub, returns STATUS_NOT_IMPLEMENTED)
///   APIs   : not exercised by the DLL

/// (no input structure)
typedef struct _PHOTONIC_GET_CHANNEL_OUT {
    UINT32 IsochChannel; ///< offset 0x00  isochronous channel number
} PHOTONIC_GET_CHANNEL_OUT;

/// PHOTONIC_IOCTL_GET_PACKET_SIZE (0x222150)
///   Input : none / ignored
///   Output: 4 bytes
///   Handler: PhotonicIoctlGetPacketSize (stub, returns STATUS_NOT_IMPLEMENTED)
///   APIs   : not exercised by the DLL

/// (no input structure)
typedef struct _PHOTONIC_GET_PACKET_SIZE_OUT {
    UINT32 IsochBytesPerPacket; ///< offset 0x00  isochronous packet size in bytes
} PHOTONIC_GET_PACKET_SIZE_OUT;

// === END SECTION 2 ===

/// SECTION 3: Video streaming (continuous video)
///   IOCTLs: START_VIDEO, STOP_VIDEO, PREPARE_VIDEO, UNPREPARE_VIDEO,
///           MAP_VIDEO_FRAME, UNMAP_VIDEO_FRAME, GET_TRANSFER_INFO,
///           REGISTER_EVENT, UNREGISTER_EVENT

/// PHOTONIC_IOCTL_START_VIDEO (0x2220DC)
///   Input  (4 bytes):  PHOTONIC_START_VIDEO_IN
///   Output (0 bytes):  none (the DLL passes OutputBufferLength=0; the Status
///                      echo is written only when the caller supplies room)
///   Handler: PhotonicIoctlStartVideo
///   API: PlStartVideoStream
///   Note: The single buffer is shared (METHOD_BUFFERED); buf[0] is echoed back
///         as 0 on success.  The dword selects the transmission mode: non-zero
///         (the DLL sends 1 when streaming) enables continuous transmission
///         (ISO_EN); zero selects single-frame mode -- ISO_EN stays off and
///         each externally triggered acquisition is armed per shot through
///         PHOTONIC_IOCTL_SW_TRIGGER (the DLL's triggered snap runs on this
///         family with value 0).
typedef struct _PHOTONIC_START_VIDEO_IN {
    UINT32 Flags; ///< offset 0x00 -- non-zero = continuous (ISO_EN), 0 = single-frame
                  ///< (per-shot arm via SW_TRIGGER)
} PHOTONIC_START_VIDEO_IN;

/// PHOTONIC_IOCTL_STOP_VIDEO (0x2220E0)
///   Input  (0 bytes):  none
///   Output (0 bytes):  none
///   Handler: PhotonicIoctlStopVideo
///   API: PlStopVideoStream (also called from several other DLL helper sites)
///   Note: DLL passes in=0 / out=0.  The driver writes buf[0]=0 on success but
///         since the DLL supplies a zero-length output buffer the value is never
///         seen by user space.

/// PHOTONIC_IOCTL_PREPARE_VIDEO (0x2220F0)
///   Input  (8 bytes):  PHOTONIC_PREPARE_VIDEO_IN
///   Output (0 bytes):  none (the DLL passes OutputBufferLength=0; the Status
///                      echo is written only when the caller supplies room)
///   Handler: PhotonicIoctlPrepareVideo
///   API: not exercised by the DLL directly (issued from its internal
///        capture path)
///   Note: Allocates the capture slot for the requested frame count; an
///         existing stopped slot (whichever family prepared it) is re-prepared
///         (released and rebuilt) when the calling handle owns it; a running
///         slot, or another handle's slot, rejects the call.
typedef struct _PHOTONIC_PREPARE_VIDEO_IN {
    PL_RETURN_CODE Status; ///< offset 0x00 -- reserved/padding; driver writes
                           ///< SUCCESS on success
    UINT32
    FrameCount; ///< offset 0x04 -- number of DMA ring-buffer frames to allocate
} PHOTONIC_PREPARE_VIDEO_IN;

/// PHOTONIC_IOCTL_UNPREPARE_VIDEO (0x2220F4)
///   Input  (0 bytes):  none
///   Output (0 bytes):  none
///   Handler: PhotonicIoctlUnprepareVideo
///   API: not exercised by the DLL directly (issued from its cleanup paths)
///   Note: Releases the capture slot.  The driver writes buf[0]=0 on success
///         but the DLL supplies a zero-length output buffer.

/// PHOTONIC_IOCTL_MAP_VIDEO_FRAME (0x2220F8)
///   Input  (0x0C bytes):  PHOTONIC_MAP_VIDEO_FRAME_IN32
///   Output (0 bytes):     none (the DLL passes OutputBufferLength=0; the
///   Status echo is written only when the caller supplies room)
///   Handler: PhotonicIoctlMapVideoFrame
///   API: PlReturnVideoData (indirectly, via the DLL's capture path)
///   Note: Locks the FrameCount user-space frame buffers starting at BaseVA
///         (stride = FrameSize) as the DMA ring.  FrameSize must match the
///         frame size computed during PREPARE_VIDEO or the IOCTL fails with
///         LastError=0x19.
typedef struct _PHOTONIC_MAP_VIDEO_FRAME_IN32 {
    UINT32 FrameCount; ///< offset 0x00 -- number of frames to map; written back as 0
                       ///< on success
    UINT32 FrameSize;  ///< offset 0x04 -- size of each frame in bytes; must match
                       ///< prepared size
    UINT32 BaseVA;     ///< offset 0x08 -- 32-bit user-mode base VA of the frame array
                       ///< (WOW64); driver casts via (PVOID)(ULONG_PTR)
} PHOTONIC_MAP_VIDEO_FRAME_IN32;

/// PHOTONIC_IOCTL_UNMAP_VIDEO_FRAME (0x2220FC)
///   Input  (0 bytes):  none
///   Output (0 bytes):  none
///   Handler: PhotonicIoctlUnmapVideoFrame
///   API: not exercised by the DLL directly (issued from its capture and
///        cleanup paths)
///   Note: Unlocks every mapped frame and resets the ring to empty.

/// PHOTONIC_IOCTL_GET_TRANSFER_INFO (0x2220EC)
///   Input  (0 bytes):  none
///   Output (0x0C bytes):  PHOTONIC_GET_TRANSFER_INFO_OUT
///   Handler: PhotonicIoctlGetTransferInfo
///   API: not exercised by the DLL directly (issued from its worker thread)
///   Note: Returns the ring index of the last delivered frame and the running
///         delivered total.  Valid while the shared capture slot exists,
///         whichever family prepared it (LastError=0x1F otherwise).  The DLL
///         calls it after its registered event fires and uses
///         CurrentFrameIndex to address the mapped ring.
typedef struct _PHOTONIC_GET_TRANSFER_INFO_OUT {
    PL_RETURN_CODE Status;    ///< offset 0x00 -- SUCCESS on success
    UINT32 CurrentFrameIndex; ///< offset 0x04 -- ring index of the last
                              ///< completed frame
    UINT32 TotalFrameCount;   ///< offset 0x08 -- total frames received
} PHOTONIC_GET_TRANSFER_INFO_OUT;

/// PHOTONIC_IOCTL_REGISTER_EVENT (0x2220E4)
///   Input  (0x0C bytes):  PHOTONIC_REGISTER_EVENT_IN32
///   Output (0 bytes):     none (driver writes Status into shared buffer but DLL
///                         uses out-length 0, so it is not returned)
///   Handler: PhotonicIoctlRegisterEvent
///   API: not exercised by the DLL directly (issued from its worker thread)
///   Note: The driver calls ObReferenceObjectByHandle on Handle to obtain a
///         referenced KEVENT* and arms it with a delivered-frame countdown:
///         Type=0 arms a countdown of 1 -- the event fires on the next
///         delivered frame and the entry is retired (the DLL's per-shot /
///         per-fetch completion signal, re-registered before every wait);
///         Type=1 arms a countdown of the mapped ring depth -- it fires once
///         that many frames have been delivered.  The capture slot must exist
///         (either family's prepare) and a ring must be mapped
///         (MappedFrameCount > 0), else LastError=0x1F; a Type other than 0/1
///         fails with LastError=0x20.
typedef struct _PHOTONIC_REGISTER_EVENT_IN32 {
    PL_RETURN_CODE Status; ///< offset 0x00 -- reserved/padding; driver writes
                           ///< SUCCESS on success
    UINT32 Handle;         ///< offset 0x04 -- 32-bit user-mode event HANDLE (WOW64); driver
                           ///< casts via (HANDLE)(ULONG_PTR) for ObReferenceObjectByHandle
    UINT32 Type;           ///< offset 0x08 -- 0 = fires on the next delivered frame,
                           ///< 1 = fires after a full ring's worth of frames
} PHOTONIC_REGISTER_EVENT_IN32;

/// PHOTONIC_IOCTL_UNREGISTER_EVENT (0x2220E8)
///   Input  (0x0C bytes):  PHOTONIC_UNREGISTER_EVENT_IN32
///   Output (0 bytes):     none (driver writes Status into shared buffer but DLL
///                         uses out-length 0)
///   Handler: PhotonicIoctlUnregisterEvent
///   API: not exercised by the DLL directly (issued from its worker thread)
///   Note: Removes the registered entry matching Handle and Type before it
///         fires (the DLL only calls this on a shot-wait timeout), dropping the
///         event object reference.  Fails with STATUS_NOT_FOUND (no LastError)
///         if no matching entry exists -- benign when a concurrent delivery
///         just fired and retired the entry.
typedef struct _PHOTONIC_UNREGISTER_EVENT_IN32 {
    PL_RETURN_CODE Status; ///< offset 0x00 -- reserved/padding; driver writes
                           ///< SUCCESS on success
    UINT32
    Handle;      ///< offset 0x04 -- 32-bit user-mode event HANDLE that was previously
                 ///< registered (WOW64); driver casts via (HANDLE)(ULONG_PTR)
    UINT32 Type; ///< offset 0x08 -- event type (mirrors REGISTER_EVENT; used for
                 ///< lookup)
} PHOTONIC_UNREGISTER_EVENT_IN32;
// === END SECTION 3 ===

/// SECTION 4: Imager (camera/transmit-side control of the shared session)
///   IOCTLs: PREPARE_IMAGER, UNPREPARE_IMAGER, START_IMAGER, STOP_IMAGER

/// Despite the name, the imager family is NOT a still-capture mode and the
/// video family (SECTION 3) is not only for streaming: they are two control
/// surfaces over the same capture session, split along the FireWire bus.  The
/// imager verbs drive the camera/transmit side (isochronous bandwidth, the
/// channel, continuous transmission via ISO_EN); the video verbs drive the
/// host/receive side as well as the transmit side, so a client using only the
/// video family gets a complete session.  Both families operate on the one
/// capture slot (Extension->IoctlCapture) -- there is no per-family ownership:
/// a video prepare over an existing slot (imager-prepared included) is
/// accepted only when the slot is stopped and owned by the calling handle,
/// and then releases and rebuilds it rather than topping it up (a running
/// slot fails with LastError=0x17), the state-bearing
/// IOCTLs (MAP/UNMAP_VIDEO_FRAME, GET_TRANSFER_INFO, REGISTER/UNREGISTER_
/// EVENT, SW_TRIGGER) work whichever family created the slot, and either
/// family's stop / unprepare acts on the whole session (the client's teardown
/// interleaves both families and ignores the trailing failure).  Single-frame
/// capture is selected by the START value (SECTION 3 / SW_TRIGGER notes), not
/// by this family: the DLL's triggered snap never uses these verbs beyond the
/// shared teardown.  See docs/ioctl-interface-design.md.

/// PHOTONIC_IOCTL_PREPARE_IMAGER  (0x222160)
///   Input : 8 bytes  (PHOTONIC_PREPARE_IMAGER_IN)
///   Output: none     (driver writes buf[0]=0 into the shared METHOD_BUFFERED
///                     buffer but the DLL passes OutputBufferLength=0)
///   Handler : PhotonicIoctlPrepareImager
///   Pl* API : driven by the DLL's internal imager state machine; no named
///             Pl* export calls it directly
///
/// Allocates the shared capture slot, resolving the camera's current format
/// selection exactly as PREPARE_VIDEO does (the frame size a later
/// MAP_VIDEO_FRAME must match is the same whichever family prepared the
/// slot).  The driver does NOT read any input field for processing; only
/// buf[0] is written back (== 0 on success).  The DLL passes 8 bytes so that
/// the shared METHOD_BUFFERED buffer is large enough to receive the 4-byte
/// status word in buf[0].  Unlike PREPARE_VIDEO there is no re-prepare path:
/// any existing slot fails the call with LastError=0x17.
typedef struct _PHOTONIC_PREPARE_IMAGER_IN {
    UINT32 Reserved0; ///< offset 0x00 -- not read by driver; shared buf slot for
                      ///< status reply
    UINT32 Reserved1; ///< offset 0x04 -- not read by driver; value 4 set by DLL
                      ///< (padding)
} PHOTONIC_PREPARE_IMAGER_IN;

/// PHOTONIC_IOCTL_UNPREPARE_IMAGER  (0x222164)
///   Input : none  (DLL passes inLen=0)
///   Output: none  (DLL passes outLen=0)
///   Handler : PhotonicIoctlUnprepareImager
///   Pl* API : driven by the DLL's internal imager state machine; no named
///             Pl* export calls it directly
///
/// Releases the whole session (stop, unmap, engine destroy) and frees the
/// slot, whichever family prepared it.  Fails with STATUS_UNSUCCESSFUL and no
/// LastError when the slot is already gone: the DLL's teardown calls this
/// after UNPREPARE_VIDEO has freed the slot and ignores the status, so the
/// failure must be graceful (error return, no side effects).
///
/// (No input/output structure -- both sizes are 0.)

/// PHOTONIC_IOCTL_START_IMAGER  (0x222168)
///   Input : 4 bytes  (PHOTONIC_START_IMAGER_IN)
///   Output: none     (DLL passes outLen=0)
///   Handler : PhotonicIoctlStartImager
///   Pl* API : driven by the DLL's internal imager state machine; no named
///             Pl* export calls it directly
///
/// Starts the transmit half only: programs the camera and acquires the
/// isochronous resources, but attaches no ring and issues no channel listen.
/// The dword selects the transmission mode exactly as in START_VIDEO:
/// non-zero (the DLL always sends 1) enables
/// continuous transmission (ISO_EN) -- with nothing mapped this free-runs the
/// camera with nobody listening (the transmit-only use case); zero selects
/// single-frame mode.  On success sets the stream running and writes
/// buf[0]=0; without a slot the call fails with LastError=0x1F.
typedef struct _PHOTONIC_START_IMAGER_IN {
    UINT32 Mode; ///< offset 0x00 -- non-zero = continuous (ISO_EN; the DLL always
                 ///< sends 1), 0 = single-frame
} PHOTONIC_START_IMAGER_IN;

/// PHOTONIC_IOCTL_STOP_IMAGER  (0x22216C)
///   Input : none  (DLL passes inLen=0)
///   Output: none  (DLL passes outLen=0)
///   Handler : PhotonicIoctlStopImager
///   Pl* API : driven by the DLL's internal imager state machine; no named
///             Pl* export calls it directly
///
/// Full stop of BOTH halves of the session (inherited asymmetry: the imager
/// stop stops everything): camera transmission disabled (ISO_EN cleared, an
/// armed one-shot cancelled), attached buffers cancelled and drained,
/// isochronous resources released.  The slot stays prepared, so a start can
/// run again without a re-prepare.  Idempotent while the slot exists; fails
/// with STATUS_UNSUCCESSFUL and no LastError when it does not (the client's
/// teardown relies on that failure being harmless).  On success writes
/// buf[0]=0; since both buffer lengths are 0 the write is effectively a no-op
/// (same note as UNPREPARE_IMAGER).
///
/// (No input/output structure -- both sizes are 0.)
// === END SECTION 4 ===

/// SECTION 5: Frame rate
///   IOCTLs: ENUM_FRAME_RATE, SET_FRAME_RATE, GET_FRAME_RATE

/// PHOTONIC_IOCTL_ENUM_FRAME_RATE (0x22212C)
///   Input  : 0x0C bytes - PHOTONIC_ENUM_FRAME_RATE_IN
///   Output : 0x0C bytes - PHOTONIC_ENUM_FRAME_RATE_OUT  (same buffer,
///   METHOD_BUFFERED) Handler: PhotonicIoctlEnumFrameRate API    :
///   PlEnumAvailableFrameRates
///
/// Queries the Nth available frame rate supported by the camera.  The driver
/// reads the frame-rate enumeration table built during initialization, converts
/// the 100-ns period to frames-per-second as an IEEE float, and writes it back
/// into buf[2].  If the requested index is out of range, buf[0] is set to
/// 0x14 (PL_ERROR_OUT_OF_RANGE) and the function still
/// returns STATUS_SUCCESS so the caller knows enumeration is exhausted.
/// buf[0] == 0 on success.  DCAM_REG_FRAME_RATE (CSR 0x600) is NOT accessed
/// directly here; the rate period table is populated during initialization.
/// The DLL passes the same 0xC block as both in- and out-buffer.

typedef struct _PHOTONIC_ENUM_FRAME_RATE_IN {
    PL_RETURN_CODE Status; ///< offset 0x00 - ignored on input; overwritten in OUT
    ULONG Index;           ///< offset 0x04 - zero-based frame-rate enumeration index
    ULONG Reserved;        ///< offset 0x08 - unused / pad to 0x0C
} PHOTONIC_ENUM_FRAME_RATE_IN;

typedef struct _PHOTONIC_ENUM_FRAME_RATE_OUT {
    PL_RETURN_CODE
    Status;             ///< offset 0x00 - SUCCESS; OUT_OF_RANGE if index out of range
    ULONG Reserved;     ///< offset 0x04 - (index word; not modified on return)
    float FrameRateFps; ///< offset 0x08 - frames/s = 1e7 / rate_100ns (IEEE float)
} PHOTONIC_ENUM_FRAME_RATE_OUT;

/// PHOTONIC_IOCTL_SET_FRAME_RATE (0x222130)
///   Input  : 0x0C bytes - PHOTONIC_SET_FRAME_RATE_IN
///   Output : none (output buffer length = 0)
///   Handler: PhotonicIoctlSetFrameRate
///   API    : PlSetCurrentFrameRate
///
/// Selects the active frame rate by its enumeration index.  The driver looks
/// up the corresponding DCAM frame-rate code and writes it to
/// DCAM_REG_FRAME_RATE (CSR 0x600) via Photonic1394WriteRegister.  The call is
/// rejected (LastError = 0x17, STATUS_INVALID_DEVICE_STATE) while either
/// front-end holds the camera: the DirectShow capture stream is open or a
/// direct-buffer capture slot is prepared.
/// buf[0] is set to 0 on success but is not returned to the caller because the
/// output buffer size is 0.
/// The DLL allocates a 0xC buffer, places the index at offset 4, and passes a
/// 0 output length.

typedef struct _PHOTONIC_SET_FRAME_RATE_IN {
    PL_RETURN_CODE
    Status;         ///< offset 0x00 - unused on input (left uninitialised by DLL)
    ULONG Index;    ///< offset 0x04 - zero-based frame-rate enumeration index
    ULONG Reserved; ///< offset 0x08 - unused / pad to 0x0C
} PHOTONIC_SET_FRAME_RATE_IN;

/// PHOTONIC_IOCTL_GET_FRAME_RATE (0x22213C)
///   Input  : none (input buffer not read by the handler)
///   Output : 0x0C bytes - PHOTONIC_GET_FRAME_RATE_OUT
///   Handler: PhotonicIoctlGetFrameRate
///   API    : PlGetCurrentFrameRate
///
/// Returns the camera's current frame rate.  The driver reads the current
/// rate selection live from DCAM_REG_FRAME_RATE (CSR 0x600; a Format 7 mode
/// reports its packet-derived rate instead) and computes:
///   FrameRateFps = 1e7 / (float)rate_100ns   (IEEE single-precision)
/// buf[0] == 0 on success.
///
/// NOTE: PlGetCurrentFrameRate in the DLL does NOT issue this IOCTL.  It
/// returns the value cached in the DLL object (set by an earlier
/// PlSetCurrentFrameRate call).  The IOCTL is
/// exercised only if the DLL cache is bypassed or the driver is accessed
/// directly; the buffer layout is inferred from the driver handler.

typedef struct _PHOTONIC_GET_FRAME_RATE_OUT {
    PL_RETURN_CODE Status; ///< offset 0x00 - SUCCESS on success
    ULONG Reserved;        ///< offset 0x04 - written as zero
    float FrameRateFps;    ///< offset 0x08 - current frames/s = 1e7 / rate_100ns (IEEE
                           ///< float)
} PHOTONIC_GET_FRAME_RATE_OUT;
// === END SECTION 5 ===

/// SECTION 6: Trigger / strobe
///   IOCTLs: TRIGGER_SET, STROBE_SET, SW_TRIGGER

/// Trigger-type enumeration (two observable values in DCAM_REG_TRIGGER_MODE).
/// The handler writes 0x80000000 for FREE_RUNNING and 0x81000000 for EXTERNAL.
/// buf[1] == 0 selects FREE_RUNNING; non-zero selects EXTERNAL.
typedef enum _PHOTONIC_TRIGGER_TYPE {
    PHOTONIC_TRIGGER_FREE_RUNNING = 0, ///< internal free-running mode
    PHOTONIC_TRIGGER_EXTERNAL = 1,     ///< external hardware trigger
} PHOTONIC_TRIGGER_TYPE;

/// PHOTONIC_IOCTL_TRIGGER_SET (0x222120)
///
///   Input  : 20 bytes -- PHOTONIC_TRIGGER_SET_IN
///   Output :  4 bytes -- PHOTONIC_TRIGGER_SET_OUT (status DWORD, 0 = success)
///   Handler: PhotonicIoctlTriggerSet (photonic/ioctl/trigger.c; stub, returns
///            STATUS_NOT_IMPLEMENTED)
///   DLL API: PlSetTriggerMode -- forwards triggerMode (ULONG) to the camera
///            object's vtable method, which issues the IOCTL.
///
/// The handler writes DCAM TRIGGER_MODE (0x830) with 0x80000000, plus
/// 0x01000000 when Type is non-zero (external) and 0x02000000 when Enable is
/// non-zero, then forwards Delay, Polarity and Parameter to extended commands
/// 9, 10 and 11 in that fixed pairing (the payload order does not follow the
/// input dword order). See remaining-ioctls-design.md section 7.3.
typedef struct _PHOTONIC_TRIGGER_SET_IN {
    UINT32 Enable;    ///< offset 0x00 -- non-zero enables trigger (sets bit 0x02000000
                      ///< in DCAM reg)
    UINT32 Type;      ///< offset 0x04 -- 0 = FREE_RUNNING (0x80000000), non-zero =
                      ///< EXTERNAL (0x81000000)
    UINT32 Polarity;  ///< offset 0x08 -- polarity, forwarded to ext command 10
    UINT32 Delay;     ///< offset 0x0C -- trigger delay, forwarded to ext command 9
    UINT32 Parameter; ///< offset 0x10 -- additional trigger parameter (exposure),
                      ///< forwarded to ext command 11
} PHOTONIC_TRIGGER_SET_IN;

typedef struct _PHOTONIC_TRIGGER_SET_OUT {
    PL_RETURN_CODE Status; ///< offset 0x00 -- SUCCESS (written by driver on success)
} PHOTONIC_TRIGGER_SET_OUT;

/// PHOTONIC_IOCTL_STROBE_SET (0x222124)
///
///   Input  : 20 bytes -- PHOTONIC_STROBE_SET_IN
///   Output :  4 bytes -- PHOTONIC_STROBE_SET_OUT (status DWORD, 0 = success)
///   Handler: PhotonicIoctlStrobeSet (photonic/ioctl/trigger.c; stub, returns
///            STATUS_NOT_IMPLEMENTED)
///   DLL API: no named Pl* export issues it directly
///
/// The handler assembles the 12-byte payload of extended command 12 (strobe
/// control) in CPU memory order:
///   byte  0    = 0x00 (fixed)
///   byte  1    = Mode
///   byte  2    = Polarity
///   byte  3    = Enable
///   bytes 4-7  = Delay dword
///   bytes 8-11 = Duration dword
/// The transport's per-dword swap applies on the wire, so quadlet 0 transmits
/// as Enable, Polarity, Mode, 0x00. See remaining-ioctls-design.md section 7.4.
typedef struct _PHOTONIC_STROBE_SET_IN {
    UINT8 Enable;   ///< offset 0x00 -- strobe enable flag (payload byte 3)
    UINT8 Pad0[3];  ///< offset 0x01
    UINT8 Polarity; ///< offset 0x04 -- strobe polarity (payload byte 2)
    UINT8 Pad1[3];  ///< offset 0x05
    UINT8 Mode;     ///< offset 0x08 -- strobe mode / type byte (payload byte 1)
    UINT8 Pad2[3];  ///< offset 0x09
    UINT32 Delay;   ///< offset 0x0C -- strobe delay (payload bytes 4-7)
    UINT32
    Duration; ///< offset 0x10 -- strobe duration (payload bytes 8-11)
} PHOTONIC_STROBE_SET_IN;

typedef struct _PHOTONIC_STROBE_SET_OUT {
    PL_RETURN_CODE Status; ///< offset 0x00 -- SUCCESS (written by driver on success)
} PHOTONIC_STROBE_SET_OUT;

/// PHOTONIC_IOCTL_SW_TRIGGER (0x222128)
///
///   Input  : 0 bytes -- none (lpInBuffer = NULL, nInBufferSize = 0)
///   Output : 0 bytes -- none (lpOutBuffer = NULL, nOutBufferSize = 0)
///   Handler: PhotonicIoctlSwTrigger (photonic/ioctl/trigger.c)
///   DLL API: issued from the DLL's worker thread -- writes 0x80000000
///            to the DCAM ONE_SHOT register (0x61C), arming the camera for one
///            externally triggered acquisition: the camera waits for its
///            trigger input (a serial port's DTR line, pulsed by user space
///            out of band), captures one frame when the pulse fires, transmits
///            it and stops by itself.  The arm captures nothing on its own,
///            and the interval between arm and frame is unbounded (the driver
///            imposes no timeout; the DLL owns the completion timeout).
///
/// No input or output structures; the IOCTL carries no payload in either
/// direction.  The only precondition is that the shared capture slot exists
/// (STATUS_UNSUCCESSFUL, LastError=0xD otherwise) -- whichever family prepared
/// it and in any slot state: the DLL fires this against a video-prepared,
/// video-started slot (START_VIDEO with value 0), never an imager-prepared
/// one.  One arm per shot, re-registered event per shot; sequencing is the
/// client's responsibility.
// === END SECTION 6 ===

/// SECTION 7: Initialize / property (feature) get-set
///   IOCTLs: INITIALIZE, PROPERTY_GET, PROPERTY_SET

/// PHOTONIC_IOCTL_INITIALIZE -- 0x222114
///   Input : none (0 bytes; driver ignores Irp buffer and OutLength entirely)
///   Output: none
///   Handler: PhotonicIoctlInitialize (stub, returns STATUS_NOT_IMPLEMENTED)
///   API(s): not exercised by the DLL (PlInitialize allocates a DLL object and
///           calls a vtable Open method that calls CreateFileA on the device
///           path; it does NOT send this IOCTL)
///
///   Resets the camera to a known state: writes 0x80000000 to DCAM CsrBase+0
///   (power / initialize register).  This driver caches no format state, so
///   there is nothing to clear on the host side.
///   No shared buffer structure -- this IOCTL carries no payload.

/// PHOTONIC_IOCTL_PROPERTY_GET -- 0x222118
///   Input : 0xC bytes -- PHOTONIC_PROPERTY_IN
///   Output: 0xC bytes -- PHOTONIC_PROPERTY_OUT  (same buffer, METHOD_BUFFERED)
///   Handler: PhotonicIoctlPropertyGet (stub, returns STATUS_NOT_IMPLEMENTED)
///   API(s): PlGetFeature
///
///   The DLL passes the same 0xC-byte buffer as both lpInBuffer and
///   lpOutBuffer; the driver reads featureId from buf[0] on entry and
///   overwrites the whole buffer on success (buf[0]=0, buf[1]=flags,
///   buf[2]=value).

/// Feature flags shared by PROPERTY_GET / PROPERTY_SET (buf[1]).
/// These mirror the DCAM feature-control register bit definitions.
#define PHOTONIC_FEATURE_FLAG_PRESENT  0x80000000UL ///< bit 31: feature present
#define PHOTONIC_FEATURE_FLAG_ABSOLUTE 0x40000000UL ///< bit 30: absolute control mode
#define PHOTONIC_FEATURE_FLAG_ONE_PUSH 0x04000000UL ///< bit 26: one-push (SET maps it to register bit 28)
#define PHOTONIC_FEATURE_FLAG_ON_OFF   0x02000000UL ///< bit 25: feature on/off
#define PHOTONIC_FEATURE_FLAG_AUTO     0x01000000UL ///< bit 24: auto mode

/// DCAM feature IDs used in buf[0].
/// The driver removes the gaps at IDs 4, 13, 15, and 16 when mapping an ID to
/// its 0-based feature index.
/// Valid range: 0-18 after gap removal (any index > 18 returns error 7).
#define PHOTONIC_FEATURE_BRIGHTNESS       0  ///< DCAM BRIGHTNESS
#define PHOTONIC_FEATURE_EXPOSURE         1  ///< DCAM AUTO_EXPOSURE
#define PHOTONIC_FEATURE_SHARPNESS        2  ///< DCAM SHARPNESS
#define PHOTONIC_FEATURE_WHITE_BALANCE    3  ///< DCAM WHITE_BALANCE (UB in bits[23:12])
#define PHOTONIC_FEATURE_WHITE_BAL_VR     4  ///< DCAM WHITE_BALANCE (VR in bits[11:0])
#define PHOTONIC_FEATURE_HUE              5  ///< DCAM HUE
#define PHOTONIC_FEATURE_SATURATION       6  ///< DCAM SATURATION
#define PHOTONIC_FEATURE_GAMMA            7  ///< DCAM GAMMA
#define PHOTONIC_FEATURE_SHUTTER          8  ///< DCAM SHUTTER
#define PHOTONIC_FEATURE_GAIN             9  ///< DCAM GAIN
#define PHOTONIC_FEATURE_IRIS             10 ///< DCAM IRIS
#define PHOTONIC_FEATURE_FOCUS            11 ///< DCAM FOCUS
#define PHOTONIC_FEATURE_TEMPERATURE      12 ///< DCAM TEMPERATURE (read-only; SET rejects id==12)
#define PHOTONIC_FEATURE_TEMP_TARGET      13 ///< DCAM TEMPERATURE target (bits[23:12], gap removed)
#define PHOTONIC_FEATURE_TRIGGER_PARAM    14 ///< DCAM TRIGGER parameter (bits[11:0], gap removed)
#define PHOTONIC_FEATURE_TRIGGER_POLARITY 15 ///< DCAM TRIGGER polarity (bit 24, gap removed)
#define PHOTONIC_FEATURE_TRIGGER_MODE     16 ///< DCAM TRIGGER mode (bits[19:16], gap removed)
/// IDs 0x11-0x16 map to the LO-block features (indices 0xD-0x12 after gap removal)

/// Input buffer for PROPERTY_GET; overlapped with the output on return.
typedef struct _PHOTONIC_PROPERTY_IN {
    UINT32 FeatureId; ///< offset 0x00: DCAM feature identifier (PHOTONIC_FEATURE_*)
    UINT32 Flags;     ///< offset 0x04: caller-supplied flags (ignored on GET; set to
                      ///<              0x80000000 by driver on return -- see OUT)
    UINT32 Value;     ///< offset 0x08: unused on input for GET; receives raw value on
                      ///< return
} PHOTONIC_PROPERTY_IN;

/// Output buffer for PROPERTY_GET (METHOD_BUFFERED -- same memory as IN).
/// The driver writes only Flags and Value: offset 0 still holds the caller's
/// FeatureId on return, it is not a status field.
typedef struct _PHOTONIC_PROPERTY_OUT {
    UINT32 FeatureId; ///< offset 0x00: caller's feature id, not written by the driver
    UINT32 Flags;     ///< offset 0x04: DCAM feature-control flags
                      ///<   bit 31 (0x80000000): always set on success
                      ///<   bit 30 (0x40000000): absolute control mode active
                      ///<   bit 26 (0x04000000): one-push active (not copied for IDs 14,15,16)
                      ///<   bit 25 (0x02000000): feature on (copied for all IDs)
                      ///<   bit 24 (0x01000000): auto mode active (not copied for IDs 14,15,16)
    UINT32 Value;     ///< offset 0x08: feature value
                      ///<   absolute mode (Flags & 0x40000000): IEEE-754 float read
                      ///<     from the camera absolute-CSR at Feature[idx][2]+8
                      ///<   relative mode: integer extracted from DCAM control register
                      ///<     - most features: bits[11:0] (12-bit field)
                      ///<     - ID  3 (White Balance UB):   bits[23:12]
                      ///<     - ID 13 (Temperature target): bits[23:12]
                      ///<     - ID 15 (Trigger polarity):   bit 24 (0 or 1)
                      ///<     - ID 16 (Trigger mode):       bits[19:16] (4-bit field)
} PHOTONIC_PROPERTY_OUT;

/// PHOTONIC_IOCTL_PROPERTY_SET -- 0x22211C
///   Input : 0xC bytes -- PHOTONIC_PROPERTY_SET_IN
///   Output: none (DLL passes NULL / 0 as lpOutBuffer / nOutBufferSize)
///   Handler: PhotonicIoctlPropertySet (stub, returns STATUS_NOT_IMPLEMENTED)
///   API(s): PlSetFeature
///
///   Sets a DCAM camera feature.  ID 12 (TEMPERATURE) is rejected.  The
///   driver performs a read-modify-write on the DCAM control register for
///   relative mode; for absolute mode it also writes the float value to the
///   camera absolute-CSR.  On success buf[0] is set to 0.
typedef struct _PHOTONIC_PROPERTY_SET_IN {
    UINT32 FeatureId; ///< offset 0x00: DCAM feature identifier (PHOTONIC_FEATURE_*)
    UINT32 Flags;     ///< offset 0x04: control flags (same bit definitions as
                      ///< PROPERTY_OUT.Flags)
                      ///<   bit 31 (0x80000000): include the feature-present bit
                      ///<     in the register write
                      ///<   bit 30 (0x40000000): absolute mode, the float Value
                      ///<     goes to the absolute CSR
                      ///<   bit 26 (0x04000000): one-push, written to register
                      ///<     bit 28 (hardware quirk)
                      ///<   bit 25 (0x02000000): on/off
                      ///<   bit 24 (0x01000000): auto mode
    UINT32
    Value; ///< offset 0x08: new feature value
           ///<   absolute mode (Flags & 0x40000000): IEEE-754 float written to
           ///<     the camera absolute-CSR at Feature[idx][2]+8
           ///<   relative mode: integer packed into the DCAM register per
           ///<   feature:
           ///<     - most features: bits[11:0] of the control register
           ///<     - ID  3 (White Balance UB):   bits[23:12] (VR preserved)
           ///<     - ID  4 (White Balance VR):   bits[11:0]  (UB preserved)
           ///<     - ID 13 (Temperature target): bits[23:12]
           ///<     - ID 14 (Trigger parameter):  bits[11:0]  (upper bits
           ///<     preserved)
           ///<     - ID 15 (Trigger polarity):   bit 24 (non-zero sets it)
           ///<     - ID 16 (Trigger mode):       bits[19:16] (4-bit field)
} PHOTONIC_PROPERTY_SET_IN;
// === END SECTION 7 ===

/// SECTION 8: mailbox / external I2C register access
///   IOCTLs: MAILBOX

/// IOCTL:        PHOTONIC_IOCTL_MAILBOX  (0x223718)
/// Handler:      PhotonicIoctlMailbox  (ioctl/mailbox.c)
/// Pl* APIs:     PlReadExtI2cRegister, PlWriteExtI2cRegister (and internally
///               Photonic1394WriteRegister, Photonic1394ReadRegister,
///               Photonic1394WriteBlock, Photonic1394ReadBlock for raw
///               mailbox packets)
///
/// The MAILBOX IOCTL is a multiplexed command gate: the first UINT32 field of the
/// input buffer selects the operation.  Five command variants are defined.
/// The buffer is shared (METHOD_BUFFERED); in/out occupy the same memory.
///
/// mailbox commands 0x10 and 0x14 require buf.flags == 0xFFFF (SHORT -1); they
/// access the camera's 1394 CSR registers directly via Photonic1394WriteRegister /
/// Photonic1394ReadRegister.
///
/// Commands >= 0x1000 are raw mailbox packets written verbatim (after DWORD
/// byte-swap) to the camera's mailbox CSR base address MAILBOX_BASE_CSR_ADDR
/// (0xF0204000) via Photonic1394WriteBlock; if the output length is non-zero the
/// response is read back from the same address via Photonic1394ReadBlock and
/// byte-swapped back.

/// mailbox command selectors (PHOTONIC_MAILBOX_CMD_HDR.Command).
#define MAILBOX_CMD_GET_ADDRESSES  0x0000 ///< return camera serial-number words
#define MAILBOX_CMD_WRITE_REGISTER 0x0010 ///< write a camera CSR register
#define MAILBOX_CMD_READ_REGISTER  0x0014 ///< read a camera CSR register
#define MAILBOX_CMD_RAW_MIN        0x1000 ///< >= RAW_MIN: raw mailbox packet
#define MAILBOX_CMD_IMAGER_READ    0x1008 ///< camera-head register read
#define MAILBOX_CMD_IMAGER_CONFIG  0x1009 ///< imager configuration packet / camera-head register write
#define MAILBOX_CMD_WRITE_EXT_I2C  0x100C ///< external I2C register write
#define MAILBOX_CMD_READ_EXT_I2C   0x100D ///< external I2C register read

/// Camera mailbox CSR: quadlet address within the 1394 initial register
/// space (high 16 address bits 0xFFFF) where raw mailbox packets are exchanged.
#define MAILBOX_BASE_CSR_ADDR 0xF0204000UL

/// Command 0x00: GET_ADDRESSES
///   DLL: no dedicated DLL wrapper; nothing issues this command and the
///        handler returns STATUS_NOT_IMPLEMENTED
///   in  : 4 bytes  (PHOTONIC_MAILBOX_CMD_HDR only)
///   out : 12 bytes (PHOTONIC_MAILBOX_GET_ADDRESSES_OUT)
typedef struct _PHOTONIC_MAILBOX_CMD_HDR {
    UINT32
    Command; ///< offset 0x00 -- mailbox command selector (see MAILBOX_CMD_* constants)
} PHOTONIC_MAILBOX_CMD_HDR;

typedef struct _PHOTONIC_MAILBOX_GET_ADDRESSES_OUT {
    UINT32 Command;         ///< offset 0x00 -- echoed command (0x00000000)
    UINT32 SerialNumberLow; ///< offset 0x04 -- lower 32 bits of camera serial number
    UINT32
    SerialNumberHigh; ///< offset 0x08 -- upper 32 bits of camera serial number
} PHOTONIC_MAILBOX_GET_ADDRESSES_OUT;

/// Command 0x10: WRITE_REGISTER
///   DLL: internal P1394 register write path
///   in  : 16 bytes (PHOTONIC_MAILBOX_REGISTER_IN)
///   out :  0 bytes

/// Command 0x14: READ_REGISTER
///   DLL: internal P1394 register read path
///   in  : 12 bytes (min; Command, Flags and Address are consumed)
///   out : 16 bytes (PHOTONIC_MAILBOX_REGISTER_IN reused for output)
typedef struct _PHOTONIC_MAILBOX_REGISTER_IN {
    UINT32 Command;  ///< offset 0x00 -- 0x00000010 (write) or 0x00000014 (read)
    INT16 Flags;     ///< offset 0x04 -- must be -1 (0xFFFF) to select CSR access
    UINT16 Reserved; ///< offset 0x06 -- unused; must be 0
    UINT32 Address;  ///< offset 0x08 -- 1394 CSR register address (offset from base)
    UINT32
    Value; ///< offset 0x0C -- write: value to write; read: receives value read
} PHOTONIC_MAILBOX_REGISTER_IN;

/// Command 0x1008: raw mailbox -- camera-head register read
///   DLL: no DLL wrapper; issued by the test program's camera-head
///        register accessors (test/ioctl/i2c.cpp)
///   in  :  8 bytes (PHOTONIC_MAILBOX_PKT1008_IN)
///   out : 12 bytes (PHOTONIC_MAILBOX_PKT1008_OUT)
///   Reads one 8-bit register of an I2C device on the camera head
///   (register map: common/camera_regs.h; packet layout:
///   docs/camera-head-registers.md).  Counterpart of the 0x1009 write.
typedef struct _PHOTONIC_MAILBOX_PKT1008_IN {
    UINT32 Command;   ///< offset 0x00 -- 0x00001008
    UINT8 DeviceAddr; ///< offset 0x04 -- I2C device address (CAMREG_I2C_DEV_ADDR)
    UINT8 SubCommand; ///< offset 0x05 -- register address
    UINT8 Mode;       ///< offset 0x06 -- always 1
    UINT8 Pad;        ///< offset 0x07 -- always 0
} PHOTONIC_MAILBOX_PKT1008_IN;

typedef struct _PHOTONIC_MAILBOX_PKT1008_OUT {
    UINT32 Word0;      ///< offset 0x00 -- response word 0 (ignored)
    UINT32 Word1;      ///< offset 0x04 -- response word 1 (ignored)
    UINT8 Value;       ///< offset 0x08 -- register value read
    UINT8 Reserved[3]; ///< offset 0x09 -- padding to DWORD align
} PHOTONIC_MAILBOX_PKT1008_OUT;

/// Command 0x1009: raw mailbox -- imager configuration packet
///   DLL: issued during imager initialization / mode change
///   in  : 12 bytes (PHOTONIC_MAILBOX_PKT1009_IN)
///   out :  8 bytes (PHOTONIC_MAILBOX_PKT_OUT8)
///   Note: cmd >= 0x1000 → sent as a raw mailbox packet;
///         all DWORDs are byte-swapped before Photonic1394WriteBlock and
///         byte-swapped back after Photonic1394ReadBlock.
typedef struct _PHOTONIC_MAILBOX_PKT1009_IN {
    UINT32 Command;    ///< offset 0x00 -- 0x00001009
    UINT8 DeviceAddr;  ///< offset 0x04 -- I2C device address
    UINT8 SubCommand;  ///< offset 0x05 -- sub-command / register selector
    UINT8 Mode;        ///< offset 0x06 -- always 1 in observed call sites
    UINT8 Pad;         ///< offset 0x07 -- always 0
    UINT8 Value;       ///< offset 0x08 -- byte payload / register value
    UINT8 Reserved[3]; ///< offset 0x09 -- padding to DWORD align
} PHOTONIC_MAILBOX_PKT1009_IN;

typedef struct _PHOTONIC_MAILBOX_PKT_OUT8 {
    UINT32 Word0; ///< offset 0x00 -- response word 0 (command echo or status)
    UINT32 Word1; ///< offset 0x04 -- response word 1
} PHOTONIC_MAILBOX_PKT_OUT8;

/// Command 0x100C: WRITE_EXT_I2C -- external I2C register write
///   DLL: PlWriteExtI2cRegister
///   in  : variable -- PHOTONIC_MAILBOX_EXT_I2C_WRITE_IN header (8 bytes)
///         followed by DataLength bytes of register data
///   out :  0 bytes
///   Buffer layout: total size = (DataLength + 0x0B) & ~3  (DWORD-aligned)
typedef struct _PHOTONIC_MAILBOX_EXT_I2C_WRITE_IN {
    UINT32 Command;    ///< offset 0x00 -- 0x0000100C
    UINT8 DataLength;  ///< offset 0x04 -- number of data bytes that follow
    UINT8 Reserved[3]; ///< offset 0x05 -- padding
    UINT8 Data[1];     ///< offset 0x08 -- DataLength bytes of register data (variable)
} PHOTONIC_MAILBOX_EXT_I2C_WRITE_IN;

/// Command 0x100D: READ_EXT_I2C -- external I2C register read
///   DLL: PlReadExtI2cRegister
///   in  : 8 bytes (PHOTONIC_MAILBOX_EXT_I2C_READ_IN)
///   out : variable -- PHOTONIC_MAILBOX_EXT_I2C_READ_OUT header (8 bytes)
///         followed by ActualLength bytes; buffer size =
///         (RequestedLength + 0x0B) & ~3  (DWORD-aligned, ≥ 8 bytes)
typedef struct _PHOTONIC_MAILBOX_EXT_I2C_READ_IN {
    UINT32 Command;        ///< offset 0x00 -- 0x0000100D
    UINT8 RequestedLength; ///< offset 0x04 -- number of data bytes to read
    UINT8 Reserved[3];     ///< offset 0x05 -- padding
} PHOTONIC_MAILBOX_EXT_I2C_READ_IN;

typedef struct _PHOTONIC_MAILBOX_EXT_I2C_READ_OUT {
    UINT32 Command;     ///< offset 0x00 -- 0x0000100D (echoed)
    UINT8 Reserved0;    ///< offset 0x04 -- padding / unused
    UINT8 ActualLength; ///< offset 0x05 -- number of data bytes returned (must equal
                        ///< RequestedLength)
    UINT8 Reserved1[2]; ///< offset 0x06 -- padding
    UINT8 Data[1];      ///< offset 0x08 -- ActualLength bytes of register data (variable)
} PHOTONIC_MAILBOX_EXT_I2C_READ_OUT;
// === END SECTION 8 ===

#pragma pack(pop)
