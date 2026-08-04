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

/// @file
/// KS property sets for the Photonic stream minidriver.
///
/// This file owns the driver's KS properties: the per-stream connection
/// properties (allocator framing) the capture pin advertises, and the device
/// (filter) level VideoControl / VideoProcAmp / CameraControl property sets that
/// expose the camera's DCAM image-control feature registers through
/// IAMVideoProcAmp / IAMCameraControl. The image controls and their ranges are
/// discovered from the camera during initialization
/// (PhotonicBuildDevicePropertySets), so only the controls the camera implements
/// are advertised. The property tables are advertised on the stream and in the
/// stream descriptor by the stream-descriptor builder in stream/formats.c through the
/// accessors at the end of this file.

// clang-format off
// properties.tmh (WPP-generated) must come last.
#include "photonic.h"
#include "properties.h"
#include "capture.h"
#include "p1394.h"
#include "properties.tmh"
// clang-format on

/// KS / DirectShow property-set GUIDs this driver needs, defined locally (same
/// bytes as the standard ks.h / ksmedia.h definitions) so it links no GUID
/// library.

/// KSPROPSETID_Connection {1D58C920-AC9B-11CF-A5D6-28DB04C10000}
static const GUID PHOTONIC_KSPROPSETID_Connection = {
    0x1d58c920, 0xac9b, 0x11cf, {0xa5, 0xd6, 0x28, 0xdb, 0x04, 0xc1, 0x00, 0x00}};

/// PROPSETID_VIDCAP_VIDEOCONTROL {6A2E0670-28E4-11D0-A18C-00A0C9118956}
static const GUID PHOTONIC_PROPSETID_VIDCAP_VIDEOCONTROL = {
    0x6a2e0670, 0x28e4, 0x11d0, {0xa1, 0x8c, 0x00, 0xa0, 0xc9, 0x11, 0x89, 0x56}};

/// PROPSETID_VIDCAP_VIDEOPROCAMP {C6E13360-30AC-11D0-A18C-00A0C9118956}
static const GUID PHOTONIC_PROPSETID_VIDCAP_VIDEOPROCAMP = {
    0xc6e13360, 0x30ac, 0x11d0, {0xa1, 0x8c, 0x00, 0xa0, 0xc9, 0x11, 0x89, 0x56}};

/// PROPSETID_VIDCAP_CAMERACONTROL {C6E13370-30AC-11D0-A18C-00A0C9118956}
static const GUID PHOTONIC_PROPSETID_VIDCAP_CAMERACONTROL = {
    0xc6e13370, 0x30ac, 0x11d0, {0xa1, 0x8c, 0x00, 0xa0, 0xc9, 0x11, 0x89, 0x56}};

static DEFINE_KSPROPERTY_TABLE(g_PhotonicConnectionProperties){
    DEFINE_KSPROPERTY_ITEM(KSPROPERTY_CONNECTION_ALLOCATORFRAMING,
                           TRUE,                        // GetSupported
                           sizeof(KSPROPERTY),          // MinProperty
                           sizeof(KSALLOCATOR_FRAMING), // MinData
                           FALSE,                       // SetSupported
                           NULL,                        // Values
                           0,                           // RelationsCount
                           NULL,                        // Relations
                           NULL,                        // SupportHandler
                           sizeof(ULONG)                // SerializedSize
                           ),
};

static DEFINE_KSPROPERTY_SET_TABLE(g_PhotonicStreamProperties){
    DEFINE_KSPROPERTY_SET(&PHOTONIC_KSPROPSETID_Connection, RTL_NUMBER_OF(g_PhotonicConnectionProperties),
                          g_PhotonicConnectionProperties, 0, NULL),
};

static DEFINE_KSPROPERTY_TABLE(g_PhotonicVideoControlProperties){
    DEFINE_KSPROPERTY_ITEM(KSPROPERTY_VIDEOCONTROL_CAPS,
                           TRUE,                                   // GetSupported
                           sizeof(KSPROPERTY_VIDEOCONTROL_CAPS_S), // MinProperty
                           sizeof(KSPROPERTY_VIDEOCONTROL_CAPS_S), // MinData
                           FALSE,                                  // SetSupported
                           NULL,                                   // Values
                           0,                                      // RelationsCount
                           NULL,                                   // Relations
                           NULL,                                   // SupportHandler
                           0                                       // SerializedSize
                           ),
};

/// KSPROPTYPESETID_General {97E99BA0-BDEA-11CF-A5D6-28DB04C10000}: the property
/// type of every image-control value (a signed LONG, VT_I4).
static const GUID PHOTONIC_KSPROPTYPESETID_General = {
    0x97e99ba0, 0xbdea, 0x11cf, {0xa5, 0xd6, 0x28, 0xdb, 0x04, 0xc1, 0x00, 0x00}};

/// VideoProcAmp / CameraControl image controls.
///
/// Each DirectShow control maps to one DCAM feature register on the camera. The
/// inquiry register lives at the offset below and the live value register sits
/// PHOTONIC_DCAM_FEATURE_VALUE_OFFSET (0x300) above it (the same layout the fake
/// camera implements). Which controls exist and what range each supports differs
/// per camera -- the fake implements every control with the full 12-bit range,
/// the real camera only a few with narrower ranges (brightness is a 4-bit ADC
/// offset) -- so PhotonicBuildDevicePropertySets discovers both from the camera
/// during initialization and builds the advertised property sets in the device
/// extension accordingly.
#define PHOTONIC_DCAM_FEATURE_VALUE_OFFSET 0x300

/// Feature value-register words: presence + on/off + auto/manual + 12-bit value.
#define PHOTONIC_FEATURE_PRESENCE   0x80000000 ///< Presence_Inq: control implemented
#define PHOTONIC_FEATURE_ON         0x02000000 ///< ON/OFF: control enabled
#define PHOTONIC_FEATURE_AUTO       0x01000000 ///< A_M_Mode: 1 = auto, 0 = manual
#define PHOTONIC_FEATURE_VALUE_MASK 0x00000fff

#define PHOTONIC_FEATURE_STEP 1

/// Feature inquiry-register words: presence + capability bits + packed range.
#define PHOTONIC_FEATINQ_PRESENCE  0x80000000 ///< Presence_Inq: control implemented
#define PHOTONIC_FEATINQ_READOUT   0x08000000 ///< ReadOut_Inq: value is readable
#define PHOTONIC_FEATINQ_AUTO      0x02000000 ///< Auto_Inq: auto mode supported
#define PHOTONIC_FEATINQ_MANUAL    0x01000000 ///< Manual_Inq: manual mode supported
#define PHOTONIC_FEATINQ_MIN(Word) (((Word) >> 12) & 0xfff)
#define PHOTONIC_FEATINQ_MAX(Word) ((Word) & 0xfff)

/// DCAM feature inquiry-register offsets (value register = + 0x300).
#define PHOTONIC_FEATINQ_BRIGHTNESS   0x500
#define PHOTONIC_FEATINQ_SHARPNESS    0x508
#define PHOTONIC_FEATINQ_WHITEBALANCE 0x50c
#define PHOTONIC_FEATINQ_HUE          0x510
#define PHOTONIC_FEATINQ_SATURATION   0x514
#define PHOTONIC_FEATINQ_GAMMA        0x518
#define PHOTONIC_FEATINQ_EXPOSURE     0x51c
#define PHOTONIC_FEATINQ_CONTRAST     0x520
#define PHOTONIC_FEATINQ_IRIS         0x524
#define PHOTONIC_FEATINQ_FOCUS        0x528
#define PHOTONIC_FEATINQ_ZOOM         0x580
#define PHOTONIC_FEATINQ_PAN          0x584
#define PHOTONIC_FEATINQ_TILT         0x588

/// Map each image control the driver can expose to the DCAM feature inquiry
/// register backing it. The value register written/read is this offset plus the
/// value offset above. Index i in this map owns Extension->ImagerFeatures[i].
typedef struct _PHOTONIC_FEATURE_MAP {
    const GUID *Set;
    ULONG Id;
    ULONG InquiryRegister;
    const char *Name;
} PHOTONIC_FEATURE_MAP;

static const PHOTONIC_FEATURE_MAP g_PhotonicFeatureMap[] = {
    {&PHOTONIC_PROPSETID_VIDCAP_VIDEOPROCAMP, KSPROPERTY_VIDEOPROCAMP_BRIGHTNESS, PHOTONIC_FEATINQ_BRIGHTNESS,
     "brightness"},
    {&PHOTONIC_PROPSETID_VIDCAP_VIDEOPROCAMP, KSPROPERTY_VIDEOPROCAMP_CONTRAST, PHOTONIC_FEATINQ_CONTRAST, "contrast"},
    {&PHOTONIC_PROPSETID_VIDCAP_VIDEOPROCAMP, KSPROPERTY_VIDEOPROCAMP_HUE, PHOTONIC_FEATINQ_HUE, "hue"},
    {&PHOTONIC_PROPSETID_VIDCAP_VIDEOPROCAMP, KSPROPERTY_VIDEOPROCAMP_SATURATION, PHOTONIC_FEATINQ_SATURATION,
     "saturation"},
    {&PHOTONIC_PROPSETID_VIDCAP_VIDEOPROCAMP, KSPROPERTY_VIDEOPROCAMP_SHARPNESS, PHOTONIC_FEATINQ_SHARPNESS,
     "sharpness"},
    {&PHOTONIC_PROPSETID_VIDCAP_VIDEOPROCAMP, KSPROPERTY_VIDEOPROCAMP_GAMMA, PHOTONIC_FEATINQ_GAMMA, "gamma"},
    {&PHOTONIC_PROPSETID_VIDCAP_VIDEOPROCAMP, KSPROPERTY_VIDEOPROCAMP_WHITEBALANCE, PHOTONIC_FEATINQ_WHITEBALANCE,
     "whitebalance"},
    {&PHOTONIC_PROPSETID_VIDCAP_CAMERACONTROL, KSPROPERTY_CAMERACONTROL_PAN, PHOTONIC_FEATINQ_PAN, "pan"},
    {&PHOTONIC_PROPSETID_VIDCAP_CAMERACONTROL, KSPROPERTY_CAMERACONTROL_TILT, PHOTONIC_FEATINQ_TILT, "tilt"},
    {&PHOTONIC_PROPSETID_VIDCAP_CAMERACONTROL, KSPROPERTY_CAMERACONTROL_ZOOM, PHOTONIC_FEATINQ_ZOOM, "zoom"},
    {&PHOTONIC_PROPSETID_VIDCAP_CAMERACONTROL, KSPROPERTY_CAMERACONTROL_EXPOSURE, PHOTONIC_FEATINQ_EXPOSURE,
     "exposure"},
    {&PHOTONIC_PROPSETID_VIDCAP_CAMERACONTROL, KSPROPERTY_CAMERACONTROL_IRIS, PHOTONIC_FEATINQ_IRIS, "iris"},
    {&PHOTONIC_PROPSETID_VIDCAP_CAMERACONTROL, KSPROPERTY_CAMERACONTROL_FOCUS, PHOTONIC_FEATINQ_FOCUS, "focus"},
};

C_ASSERT(RTL_NUMBER_OF(g_PhotonicFeatureMap) == PHOTONIC_IMAGER_FEATURE_COUNT);

// The GET/SET handlers use the two payload layouts interchangeably.
C_ASSERT(sizeof(KSPROPERTY_CAMERACONTROL_S) == sizeof(KSPROPERTY_VIDEOPROCAMP_S));

/// Find the image-control feature backing a (property set, id): its index into
/// g_PhotonicFeatureMap / Extension->ImagerFeatures, or -1 if the pair is not
/// one of the VideoProcAmp / CameraControl controls the driver maps to a DCAM
/// feature register.
///
/// @param Set  Property set GUID to look up.
/// @param Id   Property identifier within the set.
/// @return     Zero-based index into g_PhotonicFeatureMap, or -1 if not found.
static LONG PhotonicFindFeature(_In_ const GUID *Set, _In_ ULONG Id) {
    ULONG i;

    for (i = 0; i < RTL_NUMBER_OF(g_PhotonicFeatureMap); i++) {
        if (g_PhotonicFeatureMap[i].Id == Id && IsEqualGUID(Set, g_PhotonicFeatureMap[i].Set)) {
            return (LONG) i;
        }
    }
    return -1;
}

/// Discover one image control from the camera. A usable control must read its
/// value register back with the presence bit set: the real camera rejects reads
/// of registers it does not implement (STATUS_DEVICE_DATA_ERROR) or returns a
/// word without the presence bit.
///
/// The range and auto capability come from the DCAM feature inquiry register
/// when the camera implements it (the fake camera does). The inquiry word is
/// trusted only when it is fully coherent -- presence, readout and manual-mode
/// bits set, non-degenerate range -- because the real camera returns junk for
/// some registers it does not implement. Without a usable inquiry word the range
/// is probed empirically: writing the full-scale 12-bit value and reading back
/// the value the camera kept yields its maximum (the real camera clamps
/// brightness to 15 and gain to 383), and writing the auto bit and reading it
/// back tells whether auto mode is honoured. The probe restores the control's
/// original mode and value afterwards. A control whose full-scale write is
/// rejected or whose value never reads back (the real camera's shutter register
/// always reads zero) cannot honour GET and is treated as absent.
///
/// @param Extension  Device extension for the camera being queried.
/// @param Map        Feature map entry describing the DCAM register to probe.
/// @param Feature    Receives the discovered control state (zeroed if absent).
static VOID PhotonicDiscoverImagerFeature(_In_ PPHOTONIC_DEVICE_EXTENSION Extension,
                                          _In_ const PHOTONIC_FEATURE_MAP *Map,
                                          _Out_ PPHOTONIC_IMAGER_FEATURE Feature) {
    ULONG valueRegister = Map->InquiryRegister + PHOTONIC_DCAM_FEATURE_VALUE_OFFSET;
    ULONG word = 0;
    ULONG inquiry = 0;
    ULONG probe = 0;
    NTSTATUS status;

    RtlZeroMemory(Feature, sizeof(*Feature));

    status = Photonic1394ReadRegister(Extension, valueRegister, &word);
    if (!NT_SUCCESS(status) || (word & PHOTONIC_FEATURE_PRESENCE) == 0) {
        TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_FLAG_DISPATCH,
                    "imager feature %s (reg 0x%03x) absent: %!STATUS!, word 0x%08x\n", Map->Name, valueRegister, status,
                    word);
        return;
    }

    status = Photonic1394ReadRegister(Extension, Map->InquiryRegister, &inquiry);
    if (NT_SUCCESS(status) && (inquiry & PHOTONIC_FEATINQ_PRESENCE) != 0 && (inquiry & PHOTONIC_FEATINQ_READOUT) != 0 &&
        (inquiry & PHOTONIC_FEATINQ_MANUAL) != 0 && PHOTONIC_FEATINQ_MIN(inquiry) < PHOTONIC_FEATINQ_MAX(inquiry)) {
        Feature->Minimum = (LONG) PHOTONIC_FEATINQ_MIN(inquiry);
        Feature->Maximum = (LONG) PHOTONIC_FEATINQ_MAX(inquiry);
        Feature->AutoSupported = (inquiry & PHOTONIC_FEATINQ_AUTO) != 0;
    } else {
        //
        // No usable inquiry word: probe. Write full scale and read back the
        // value the camera kept -- its maximum whether it clamps or masks.
        //
        status = Photonic1394WriteRegister(
            Extension, valueRegister, PHOTONIC_FEATURE_PRESENCE | PHOTONIC_FEATURE_ON | PHOTONIC_FEATURE_VALUE_MASK);
        if (!NT_SUCCESS(status)) {
            TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_FLAG_DISPATCH,
                        "imager feature %s (reg 0x%03x) absent: full-scale probe write failed %!STATUS!\n", Map->Name,
                        valueRegister, status);
            return;
        }
        status = Photonic1394ReadRegister(Extension, valueRegister, &probe);
        if (NT_SUCCESS(status)) {
            Feature->Minimum = 0;
            Feature->Maximum = (LONG) (probe & PHOTONIC_FEATURE_VALUE_MASK);
        }

        //
        // Probe auto mode: write the auto bit (with the original value) and see
        // whether it sticks. The final write restores the register exactly as
        // it was first read: the ON/OFF state (a feature the client had
        // disabled must not come out of the probe forced on) and any second
        // value field the probes stomped (white balance packs one in bits
        // 23:12, see dcam-registers.md).
        //
        if (NT_SUCCESS(Photonic1394WriteRegister(Extension, valueRegister,
                                                 PHOTONIC_FEATURE_PRESENCE | PHOTONIC_FEATURE_ON |
                                                     PHOTONIC_FEATURE_AUTO | (word & PHOTONIC_FEATURE_VALUE_MASK))) &&
            NT_SUCCESS(Photonic1394ReadRegister(Extension, valueRegister, &probe))) {
            Feature->AutoSupported = (probe & PHOTONIC_FEATURE_AUTO) != 0;
        }
        Photonic1394WriteRegister(Extension, valueRegister, word);

        if (Feature->Maximum <= Feature->Minimum) {
            TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_FLAG_DISPATCH,
                        "imager feature %s (reg 0x%03x) absent: value does not read back (probe 0x%08x)\n", Map->Name,
                        valueRegister, probe);
            return;
        }
    }

    Feature->DefaultValue = (LONG) (word & PHOTONIC_FEATURE_VALUE_MASK);
    if (Feature->DefaultValue < Feature->Minimum) {
        Feature->DefaultValue = Feature->Minimum;
    }
    if (Feature->DefaultValue > Feature->Maximum) {
        Feature->DefaultValue = Feature->Maximum;
    }
    Feature->Present = TRUE;

    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_FLAG_DISPATCH,
                "imager feature %s (reg 0x%03x) present: range [%d..%d] default %d auto %u (inquiry 0x%08x)\n",
                Map->Name, valueRegister, Feature->Minimum, Feature->Maximum, Feature->DefaultValue,
                Feature->AutoSupported, inquiry);
}

/// Fill one present feature's KS range data (stepped range + default) and its
/// advertised property item. The stream class driver answers IAMVideoProcAmp /
/// IAMCameraControl GetRange from the range data, so only GET/SET reach the
/// minidriver; the item routes both to PhotonicImagerGet/SetProperty.
///
/// @param Feature  Image control whose range data and item are to be filled.
/// @param Id       KS property identifier for this control.
/// @param Item     Property item to initialize for this control.
static VOID PhotonicInitImagerPropertyItem(_Inout_ PPHOTONIC_IMAGER_FEATURE Feature, _In_ ULONG Id,
                                           _Out_ PKSPROPERTY_ITEM Item) {
    Feature->Stepping.SteppingDelta = PHOTONIC_FEATURE_STEP;
    Feature->Stepping.Reserved = 0;
    Feature->Stepping.Bounds.SignedMinimum = Feature->Minimum;
    Feature->Stepping.Bounds.SignedMaximum = Feature->Maximum;

    Feature->Members[0].MembersHeader.MembersFlags = KSPROPERTY_MEMBER_STEPPEDRANGES;
    Feature->Members[0].MembersHeader.MembersSize = sizeof(KSPROPERTY_STEPPING_LONG);
    Feature->Members[0].MembersHeader.MembersCount = 1;
    Feature->Members[0].MembersHeader.Flags = 0;
    Feature->Members[0].Members = &Feature->Stepping;
    Feature->Members[1].MembersHeader.MembersFlags = KSPROPERTY_MEMBER_VALUES;
    Feature->Members[1].MembersHeader.MembersSize = sizeof(LONG);
    Feature->Members[1].MembersHeader.MembersCount = 1;
    Feature->Members[1].MembersHeader.Flags = KSPROPERTY_MEMBER_FLAG_DEFAULT;
    Feature->Members[1].Members = &Feature->DefaultValue;

    Feature->Values.PropTypeSet.Set = PHOTONIC_KSPROPTYPESETID_General;
    Feature->Values.PropTypeSet.Id = VT_I4;
    Feature->Values.PropTypeSet.Flags = 0;
    Feature->Values.MembersListCount = RTL_NUMBER_OF(Feature->Members);
    Feature->Values.MembersList = Feature->Members;

    RtlZeroMemory(Item, sizeof(*Item));
    Item->PropertyId = Id;
    Item->GetSupported = TRUE;
    Item->MinProperty = sizeof(KSPROPERTY_VIDEOPROCAMP_S);
    Item->MinData = sizeof(KSPROPERTY_VIDEOPROCAMP_S);
    Item->SetSupported = TRUE;
    Item->Values = &Feature->Values;
}

/// PhotonicBuildDevicePropertySets -- discover the image controls this camera
/// implements and build the device (filter) property sets from them in the device
/// extension: VideoControl always, then VideoProcAmp / CameraControl each
/// advertised only when at least one of its controls is present, listing only the
/// present controls. Runs once during SRB_INITIALIZE_DEVICE, after camera
/// bring-up; when bring-up failed (no CSR base) discovery is skipped and only
/// VideoControl is advertised.
///
/// @param Extension  Device extension to populate with the discovered property sets.
VOID PhotonicBuildDevicePropertySets(_In_ PPHOTONIC_DEVICE_EXTENSION Extension) {
    static const GUID *setOrder[] = {&PHOTONIC_PROPSETID_VIDCAP_VIDEOPROCAMP, &PHOTONIC_PROPSETID_VIDCAP_CAMERACONTROL};
    ULONG itemCount = 0;
    PKSPROPERTY_SET set;
    ULONG pass;
    ULONG i;

    FuncEntry(TRACE_FLAG_DISPATCH);

    if (Extension->CsrBaseAddress != 0) {
        for (i = 0; i < RTL_NUMBER_OF(g_PhotonicFeatureMap); i++) {
            PhotonicDiscoverImagerFeature(Extension, &g_PhotonicFeatureMap[i], &Extension->ImagerFeatures[i]);
        }
    } else {
        TraceEvents(TRACE_LEVEL_WARNING, TRACE_FLAG_DISPATCH,
                    "no CSR base; skipping imager feature discovery, advertising no image controls\n");
        RtlZeroMemory(Extension->ImagerFeatures, sizeof(Extension->ImagerFeatures));
    }

    Extension->DevicePropertySetCount = 0;

    set = &Extension->DevicePropertySets[Extension->DevicePropertySetCount++];
    RtlZeroMemory(set, sizeof(*set));
    set->Set = &PHOTONIC_PROPSETID_VIDCAP_VIDEOCONTROL;
    set->PropertiesCount = RTL_NUMBER_OF(g_PhotonicVideoControlProperties);
    set->PropertyItem = g_PhotonicVideoControlProperties;

    for (pass = 0; pass < RTL_NUMBER_OF(setOrder); pass++) {
        ULONG first = itemCount;

        for (i = 0; i < RTL_NUMBER_OF(g_PhotonicFeatureMap); i++) {
            if (!IsEqualGUID(g_PhotonicFeatureMap[i].Set, setOrder[pass]) || !Extension->ImagerFeatures[i].Present) {
                continue;
            }
            PhotonicInitImagerPropertyItem(&Extension->ImagerFeatures[i], g_PhotonicFeatureMap[i].Id,
                                           &Extension->ImagerPropertyItems[itemCount]);
            itemCount++;
        }

        if (itemCount > first) {
            set = &Extension->DevicePropertySets[Extension->DevicePropertySetCount++];
            RtlZeroMemory(set, sizeof(*set));
            set->Set = setOrder[pass];
            set->PropertiesCount = itemCount - first;
            set->PropertyItem = &Extension->ImagerPropertyItems[first];
        }
    }

    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_FLAG_DISPATCH,
                "advertising %u device property set(s), %u image control(s)\n", Extension->DevicePropertySetCount,
                itemCount);
}

/// GET one image control: read the camera's DCAM feature value register and
/// report the 12-bit value plus the auto/manual flag back through the shared
/// KSPROPERTY_VIDEOPROCAMP_S / KSPROPERTY_CAMERACONTROL_S layout (identical:
/// KSPROPERTY header, then LONG Value, ULONG Flags, ULONG Capabilities).
///
/// @param Extension        Device extension for the camera being queried.
/// @param Property         KS property descriptor identifying the control.
/// @param BytesTransferred Receives the number of bytes written to the output buffer.
/// @return                 STATUS_SUCCESS on success, or an error code.
static NTSTATUS PhotonicImagerGetProperty(_In_ PPHOTONIC_DEVICE_EXTENSION Extension,
                                          _In_ PSTREAM_PROPERTY_DESCRIPTOR Property, _Out_ PULONG BytesTransferred) {
    const PHOTONIC_FEATURE_MAP *map;
    PPHOTONIC_IMAGER_FEATURE feature;
    PKSPROPERTY_VIDEOPROCAMP_S value;
    ULONG word = 0;
    NTSTATUS status;
    LONG index;

    *BytesTransferred = 0;

    index = PhotonicFindFeature(&Property->Property->Set, Property->Property->Id);
    if (index < 0 || !Extension->ImagerFeatures[index].Present) {
        return STATUS_NOT_FOUND;
    }
    map = &g_PhotonicFeatureMap[index];
    feature = &Extension->ImagerFeatures[index];
    if (Property->PropertyOutputSize < sizeof(KSPROPERTY_VIDEOPROCAMP_S)) {
        return STATUS_BUFFER_TOO_SMALL;
    }

    status = Photonic1394ReadRegister(Extension, map->InquiryRegister + PHOTONIC_DCAM_FEATURE_VALUE_OFFSET, &word);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    value = (PKSPROPERTY_VIDEOPROCAMP_S) Property->PropertyInfo;
    value->Value = (LONG) (word & PHOTONIC_FEATURE_VALUE_MASK);
    value->Flags =
        (word & PHOTONIC_FEATURE_AUTO) ? KSPROPERTY_VIDEOPROCAMP_FLAGS_AUTO : KSPROPERTY_VIDEOPROCAMP_FLAGS_MANUAL;
    value->Capabilities = KSPROPERTY_VIDEOPROCAMP_FLAGS_MANUAL;
    if (feature->AutoSupported) {
        value->Capabilities |= KSPROPERTY_VIDEOPROCAMP_FLAGS_AUTO;
    }
    *BytesTransferred = sizeof(KSPROPERTY_VIDEOPROCAMP_S);

    TraceEvents(TRACE_LEVEL_VERBOSE, TRACE_FLAG_DISPATCH, "imager get reg 0x%03x -> value=%d flags=0x%x\n",
                map->InquiryRegister + PHOTONIC_DCAM_FEATURE_VALUE_OFFSET, value->Value, value->Flags);
    return STATUS_SUCCESS;
}

/// SET one image control: clamp the requested value into the camera's discovered
/// range and update the DCAM feature value register (presence + on + auto/manual
/// + 12-bit value, with the register's remaining bits preserved by a
/// read-modify-write), so the new setting is reflected by the camera and in
/// streamed frames. Auto mode is requested only when discovery showed the camera
/// honours it.
///
/// @param Extension  Device extension for the camera being updated.
/// @param Property   KS property descriptor carrying the new value and flags.
/// @return           STATUS_SUCCESS on success, or an error code.
static NTSTATUS PhotonicImagerSetProperty(_In_ PPHOTONIC_DEVICE_EXTENSION Extension,
                                          _In_ PSTREAM_PROPERTY_DESCRIPTOR Property) {
    const PHOTONIC_FEATURE_MAP *map;
    PPHOTONIC_IMAGER_FEATURE feature;
    PKSPROPERTY_VIDEOPROCAMP_S value;
    LONG requested;
    ULONG current = 0;
    ULONG word;
    LONG index;

    index = PhotonicFindFeature(&Property->Property->Set, Property->Property->Id);
    if (index < 0 || !Extension->ImagerFeatures[index].Present) {
        return STATUS_NOT_FOUND;
    }
    map = &g_PhotonicFeatureMap[index];
    feature = &Extension->ImagerFeatures[index];

    //
    // The payload is dereferenced from PropertyInfo, whose extent is
    // PropertyOutputSize (the GET handler checks the same bound). The input
    // size describes the KSPROPERTY header buffer, not the value payload.
    //
    if (Property->PropertyOutputSize < sizeof(KSPROPERTY_VIDEOPROCAMP_S)) {
        return STATUS_BUFFER_TOO_SMALL;
    }

    value = (PKSPROPERTY_VIDEOPROCAMP_S) Property->PropertyInfo;
    requested = value->Value;
    if (requested < feature->Minimum) {
        requested = feature->Minimum;
    }
    if (requested > feature->Maximum) {
        requested = feature->Maximum;
    }

    //
    // Read-modify-write: only the mode and value fields are composed, the
    // rest of the register is preserved. White balance packs a second 12-bit
    // field in bits 23:12 (dcam-registers.md), and composing the whole word
    // from scratch would zero it on every set. A failed read leaves current
    // at zero, which degrades to the from-scratch composition.
    //
    (VOID) Photonic1394ReadRegister(Extension, map->InquiryRegister + PHOTONIC_DCAM_FEATURE_VALUE_OFFSET, &current);
    word = (current & ~(PHOTONIC_FEATURE_ON | PHOTONIC_FEATURE_AUTO | PHOTONIC_FEATURE_VALUE_MASK)) |
           PHOTONIC_FEATURE_PRESENCE | PHOTONIC_FEATURE_ON | ((ULONG) requested & PHOTONIC_FEATURE_VALUE_MASK);
    if ((value->Flags & KSPROPERTY_VIDEOPROCAMP_FLAGS_AUTO) != 0 && feature->AutoSupported) {
        word |= PHOTONIC_FEATURE_AUTO;
    }

    TraceEvents(TRACE_LEVEL_VERBOSE, TRACE_FLAG_DISPATCH,
                "imager set reg 0x%03x <- value=%d flags=0x%x (word 0x%08x)\n",
                map->InquiryRegister + PHOTONIC_DCAM_FEATURE_VALUE_OFFSET, requested, value->Flags, word);
    return Photonic1394WriteRegister(Extension, map->InquiryRegister + PHOTONIC_DCAM_FEATURE_VALUE_OFFSET, word);
}

/// SRB_GET_DEVICE_PROPERTY -- answer the device (filter) property sets advertised
/// in Extension->DevicePropertySets: PROPSETID_VIDCAP_VIDEOCONTROL /
/// KSPROPERTY_VIDEOCONTROL_CAPS (no special video-control capabilities), plus the
/// VideoProcAmp / CameraControl image controls backed by the DCAM feature
/// registers. Anything else returns STATUS_NOT_FOUND so ksproxy skips it instead
/// of treating it as fatal. (GetRange is answered by the stream class driver from
/// the property tables' range data, so only the value GET reaches here.)
///
/// @param Srb  The SRB_GET_DEVICE_PROPERTY request block.
/// @return     STATUS_SUCCESS on success, or an error code.
NTSTATUS PhotonicGetDeviceProperty(_In_ PHW_STREAM_REQUEST_BLOCK Srb) {
    PSTREAM_PROPERTY_DESCRIPTOR property = Srb->CommandData.PropertyInfo;

    FuncEntry(TRACE_FLAG_DISPATCH);

    if (IsEqualGUID(&property->Property->Set, &PHOTONIC_PROPSETID_VIDCAP_VIDEOCONTROL) &&
        property->Property->Id == KSPROPERTY_VIDEOCONTROL_CAPS) {
        PKSPROPERTY_VIDEOCONTROL_CAPS_S caps = (PKSPROPERTY_VIDEOCONTROL_CAPS_S) property->PropertyInfo;

        if (property->PropertyOutputSize < sizeof(KSPROPERTY_VIDEOCONTROL_CAPS_S)) {
            return STATUS_BUFFER_TOO_SMALL;
        }

        RtlZeroMemory(caps, sizeof(*caps));
        caps->VideoControlCaps = 0;
        Srb->ActualBytesTransferred = sizeof(KSPROPERTY_VIDEOCONTROL_CAPS_S);
        return STATUS_SUCCESS;
    }

    return PhotonicImagerGetProperty((PPHOTONIC_DEVICE_EXTENSION) Srb->HwDeviceExtension, property,
                                     &Srb->ActualBytesTransferred);
}

/// SRB_SET_DEVICE_PROPERTY -- apply a settable device property. The only settable
/// device-level sets are the VideoProcAmp / CameraControl image controls; every
/// other set is reported absent with STATUS_NOT_FOUND.
///
/// @param Srb  The SRB_SET_DEVICE_PROPERTY request block.
/// @return     STATUS_SUCCESS on success, or an error code.
NTSTATUS PhotonicSetDeviceProperty(_In_ PHW_STREAM_REQUEST_BLOCK Srb) {
    PSTREAM_PROPERTY_DESCRIPTOR property = Srb->CommandData.PropertyInfo;

    FuncEntry(TRACE_FLAG_DISPATCH);

    return PhotonicImagerSetProperty((PPHOTONIC_DEVICE_EXTENSION) Srb->HwDeviceExtension, property);
}

/// SRB_GET_STREAM_PROPERTY -- answer the per-stream KS property sets advertised
/// in g_PhotonicStreamProperties. Currently only KSPROPSETID_Connection /
/// KSPROPERTY_CONNECTION_ALLOCATORFRAMING, which ksproxy needs to size the
/// capture pin's frame buffers. Returns STATUS_NOT_FOUND for anything else so
/// the KS proxy skips it rather than treating it as fatal.
///
/// @param Srb  The SRB_GET_STREAM_PROPERTY request block.
/// @return     STATUS_SUCCESS on success, or an error code.
NTSTATUS PhotonicGetStreamProperty(_In_ PHW_STREAM_REQUEST_BLOCK Srb) {
    PPHOTONIC_DEVICE_EXTENSION extension = (PPHOTONIC_DEVICE_EXTENSION) Srb->HwDeviceExtension;
    PSTREAM_PROPERTY_DESCRIPTOR property = Srb->CommandData.PropertyInfo;

    FuncEntry(TRACE_FLAG_STREAM);

    if (IsEqualGUID(&property->Property->Set, &PHOTONIC_KSPROPSETID_Connection) &&
        property->Property->Id == KSPROPERTY_CONNECTION_ALLOCATORFRAMING) {
        PKSALLOCATOR_FRAMING framing = (PKSALLOCATOR_FRAMING) property->PropertyInfo;

        if (property->PropertyOutputSize < sizeof(KSALLOCATOR_FRAMING)) {
            return STATUS_BUFFER_TOO_SMALL;
        }

        RtlZeroMemory(framing, sizeof(*framing));
        framing->RequirementsFlags = KSALLOCATOR_REQUIREMENTF_SYSTEM_MEMORY |
                                     KSALLOCATOR_REQUIREMENTF_INPLACE_MODIFIER |
                                     KSALLOCATOR_REQUIREMENTF_PREFERENCES_ONLY;
        framing->PoolType = PagedPool;
        //
        // A small ring of page-aligned frames. Zero-copy attaches each frame's MDL
        // straight to the bus as the DMA target, so page alignment keeps the mapping
        // simple, and the frame count matches the engine's descriptor pool so every
        // buffer can be attached at once.
        //
        framing->Frames = PHOTONIC_CAPTURE_FRAME_COUNT;
        //
        // Size frames to the largest advertised mode so a single allocator can
        // back any format the pin may be connected with.
        //
        framing->FrameSize = extension->MaxSampleSize;
        framing->FileAlignment = PAGE_SIZE - 1;

        Srb->ActualBytesTransferred = sizeof(KSALLOCATOR_FRAMING);
        return STATUS_SUCCESS;
    }

    return STATUS_NOT_FOUND;
}

/// SRB_SET_STREAM_PROPERTY -- no settable stream-level KS property sets. Report
/// absent (STATUS_NOT_FOUND) so the KS proxy skips the set instead of treating
/// it as a fatal error (see the device property note in dispatch.c).
///
/// @param Srb  The SRB_SET_STREAM_PROPERTY request block.
/// @return     STATUS_NOT_FOUND (no settable stream properties are advertised).
NTSTATUS PhotonicSetStreamProperty(_In_ PHW_STREAM_REQUEST_BLOCK Srb) {
    FuncEntry(TRACE_FLAG_STREAM);

    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_FLAG_STREAM, "%s (0x%x) no settable stream properties\n",
                PhotonicSrbCommandName(Srb->Command), Srb->Command);
    return STATUS_NOT_FOUND;
}

/// PhotonicGetStreamPropertySet -- hand the per-stream KS property sets
/// (currently just KSPROPSETID_Connection) to PhotonicStreamFormatBuild, which
/// advertises them on the capture pin.
///
/// @param Set    Receives a pointer to the stream property set table.
/// @param Count  Receives the number of entries in the table.
VOID PhotonicGetStreamPropertySet(_Out_ PKSPROPERTY_SET *Set, _Out_ PULONG Count) {
    FuncEntry(TRACE_FLAG_STREAM);

    *Set = (PKSPROPERTY_SET) g_PhotonicStreamProperties;
    *Count = RTL_NUMBER_OF(g_PhotonicStreamProperties);
}

/// PhotonicGetDevicePropertySet -- hand the device (filter) KS property sets
/// (VideoControl plus the discovered VideoProcAmp / CameraControl controls, built
/// per device by PhotonicBuildDevicePropertySets) to PhotonicStreamGetInfo, which
/// reports them in the stream descriptor header.
///
/// @param Extension  Device extension holding the built property sets.
/// @param Set        Receives a pointer to the device property set table.
/// @param Count      Receives the number of entries in the table.
VOID PhotonicGetDevicePropertySet(_In_ PPHOTONIC_DEVICE_EXTENSION Extension, _Out_ PKSPROPERTY_SET *Set,
                                  _Out_ PULONG Count) {
    FuncEntry(TRACE_FLAG_STREAM);

    *Set = Extension->DevicePropertySets;
    *Count = Extension->DevicePropertySetCount;
}
