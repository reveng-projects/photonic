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
/// FrameStats — thread-safe accumulator for the pixel content of streamed video
/// frames.
///
/// It implements ISampleGrabberCB so it can be plugged straight into qedit's
/// Sample Grabber (windowed path) and is also driven directly by InspectRenderer
/// (headless path).  Every delivered frame's bytes are subsampled and reduced to
/// running min/mean/max statistics; the DirectShow sweep then uses those numbers
/// to decide whether a (format, frame-rate) combination produced real image data
/// or only black / empty frames.  A single non-black frame proves real image
/// data is flowing.
///
/// The instance is stack-owned for the program lifetime, so its IUnknown
/// refcount is inert (AddRef/Release are no-ops).
#pragma once

#include <windows.h>

#include <vector>

#include "../../common/frame_meta.h"
#include "samplegrabber.h"

/// Verdict of the embedded frame-header verification accumulated across a
/// streaming run (see common/frame_meta.h).  The fake camera stamps each frame
/// with its state plus a whole-frame checksum and a 0-based index that restarts
/// on every stream; this records whether every delivered frame carried a valid,
/// checksum-correct header and whether the indices ran 0,1,2,... with no gaps.
struct FrameMetaStats {
    long withHeader;                            ///< frames carrying a valid magic+version header
    long badHeader;                             ///< frames missing/invalid header (no magic or version)
    long badChecksum;                           ///< frames whose stored checksum did not match
    long badSize;                               ///< frames whose meta.frame_bytes != delivered length
    long badFeatures;                           ///< frames whose feature_count != FRAME_META_NUM_FEATURES
                                                ///< or i2c_reg_count != CAMREG_COUNT (layout drift)
    long firstIndex;                            ///< index of the first delivered frame (-1 if none)
    long lastIndex;                             ///< index of the most recent delivered frame (-1 if none)
    long outOfOrder;                            ///< frames whose index != previous index + 1
    bool indexContiguous;                       ///< every delivered index ran 0,1,2,... with no gaps
    uint32_t features[FRAME_META_NUM_FEATURES]; ///< last frame's feature words
    uint8_t i2cRegs[CAMREG_COUNT];              ///< last frame's camera-head register file
                                                ///< (common/camera_regs.h)
};

class FrameStats : public ISampleGrabberCB {
public:
    FrameStats() {
        InitializeCriticalSection(&m_cs);
        m_verifyMeta = true;
        m_captureLastFrame = false;
        Reset();
    }
    ~FrameStats() {
        DeleteCriticalSection(&m_cs);
    }

    void Reset() {
        EnterCriticalSection(&m_cs);
        m_frames = 0;
        m_maxByte = 0;
        m_minByte = 255;
        m_sum = 0;
        m_sampled = 0;
        m_withHeader = 0;
        m_badHeader = 0;
        m_badChecksum = 0;
        m_badSize = 0;
        m_badFeatures = 0;
        m_firstIndex = -1;
        m_lastIndex = -1;
        m_outOfOrder = 0;
        m_indexContiguous = true;
        memset(m_lastFeatures, 0, sizeof(m_lastFeatures));
        memset(m_lastI2cRegs, 0, sizeof(m_lastI2cRegs));
        m_lastFrame.clear();
        LeaveCriticalSection(&m_cs);
    }

    /// Enable/disable parsing of the embedded per-frame test header.  A real
    /// camera does not stamp one, so the sweep turns this off for live hardware.
    /// Not touched by Reset() — it is configuration, not per-run state.
    void SetVerifyMeta(bool enable) {
        EnterCriticalSection(&m_cs);
        m_verifyMeta = enable;
        LeaveCriticalSection(&m_cs);
    }

    /// Enable/disable keeping a copy of the most recent delivered frame (for
    /// the --dump-dir PNG samples).  Off by default: the copy costs one
    /// frame-sized memcpy per delivery.  Configuration, not per-run state, so
    /// Reset() only drops the copied bytes, not the setting.
    void SetCaptureLastFrame(bool enable) {
        EnterCriticalSection(&m_cs);
        m_captureLastFrame = enable;
        LeaveCriticalSection(&m_cs);
    }

    /// Copy the most recent delivered frame into out.  out is empty when
    /// capture is off or no frame arrived since the last Reset().
    void GetLastFrame(std::vector<BYTE> &out) {
        EnterCriticalSection(&m_cs);
        out = m_lastFrame;
        LeaveCriticalSection(&m_cs);
    }

    void Get(long &frames, int &maxByte, int &minByte, double &mean) {
        EnterCriticalSection(&m_cs);
        frames = m_frames;
        maxByte = m_maxByte;
        minByte = (m_frames > 0) ? m_minByte : 0;
        mean = m_sampled ? (double) m_sum / (double) m_sampled : 0.0;
        LeaveCriticalSection(&m_cs);
    }

    void GetMeta(FrameMetaStats &out) {
        EnterCriticalSection(&m_cs);
        out.withHeader = m_withHeader;
        out.badHeader = m_badHeader;
        out.badChecksum = m_badChecksum;
        out.badSize = m_badSize;
        out.badFeatures = m_badFeatures;
        out.firstIndex = m_firstIndex;
        out.lastIndex = m_lastIndex;
        out.outOfOrder = m_outOfOrder;
        out.indexContiguous = m_indexContiguous;
        memcpy(out.features, m_lastFeatures, sizeof(out.features));
        memcpy(out.i2cRegs, m_lastI2cRegs, sizeof(out.i2cRegs));
        LeaveCriticalSection(&m_cs);
    }

    /// IUnknown — stack-owned for the program lifetime, so refcount is inert.
    STDMETHODIMP QueryInterface(REFIID riid, void **ppv) {
        if (ppv == nullptr) {
            return E_POINTER;
        }
        if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, __uuidof(ISampleGrabberCB))) {
            *ppv = static_cast<ISampleGrabberCB *>(this);
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() {
        return 2;
    }
    STDMETHODIMP_(ULONG) Release() {
        return 1;
    }

    /// ISampleGrabberCB
    STDMETHODIMP SampleCB(double, IMediaSample *) {
        return E_NOTIMPL;
    }
    STDMETHODIMP BufferCB(double, BYTE *buf, long len) {
        if (buf == nullptr || len <= 0) {
            return S_OK;
        }
        BYTE localMax = 0, localMin = 255;
        unsigned long long localSum = 0;
        long n = 0;
        // Subsample with a prime stride: cheap and channel/format-agnostic
        // (a bright pixel yields a high byte in RGB8/UYVY/RGB24 alike).
        for (long i = 0; i < len; i += 37) {
            BYTE b = buf[i];
            if (b > localMax) {
                localMax = b;
            }
            if (b < localMin) {
                localMin = b;
            }
            localSum += b;
            n++;
        }
        EnterCriticalSection(&m_cs);
        m_frames++;
        if (localMax > m_maxByte) {
            m_maxByte = localMax;
        }
        if (localMin < m_minByte) {
            m_minByte = localMin;
        }
        m_sum += localSum;
        m_sampled += n;
        if (m_verifyMeta) {
            VerifyMeta(buf, len);
        }
        if (m_captureLastFrame) {
            m_lastFrame.assign(buf, buf + len);
        }
        LeaveCriticalSection(&m_cs);
        return S_OK;
    }

private:
    /// Validate the frame-verification header (common/frame_meta.h) stamped by
    /// the fake camera at the front of every frame, and fold the result into the
    /// running counters.  Called under m_cs.
    void VerifyMeta(const BYTE *buf, long len) {
        if (len < (long) sizeof(frame_meta_t)) {
            m_badHeader++;
            m_indexContiguous = false;
            return;
        }

        frame_meta_t meta;
        memcpy(&meta, buf, sizeof(meta));

        if (meta.magic != FRAME_META_MAGIC || meta.version != FRAME_META_VERSION) {
            m_badHeader++;
            m_indexContiguous = false;
            return;
        }
        m_withHeader++;

        // Layout-drift guard: the producer must agree on the feature-set and
        // head-register-file sizes.  When it does, snapshot the live control
        // state (brightness, contrast, ...) and the camera-head register file
        // carried by this frame so the verdict can report them.
        if (meta.feature_count != FRAME_META_NUM_FEATURES || meta.i2c_reg_count != CAMREG_COUNT) {
            m_badFeatures++;
            m_indexContiguous = false;
        } else {
            memcpy(m_lastFeatures, meta.features, sizeof(m_lastFeatures));
            memcpy(m_lastI2cRegs, meta.i2c_regs, sizeof(m_lastI2cRegs));
        }

        // The header is only trustworthy if it describes the bytes delivered:
        // its frame_bytes must equal the sample length, otherwise the checksum
        // covers a different extent than what arrived.
        if (meta.frame_bytes != (uint32_t) len) {
            m_badSize++;
            m_indexContiguous = false;
        } else if (frame_meta_checksum(buf, (size_t) len) != meta.checksum) {
            m_badChecksum++;
            m_indexContiguous = false;
        }

        // Strict ordering: the first delivered frame must be index 0 and each
        // subsequent frame must be exactly one past the last.
        long index = (long) meta.frame_index;
        if (m_firstIndex < 0) {
            m_firstIndex = index;
            if (index != 0) {
                m_outOfOrder++;
                m_indexContiguous = false;
            }
        } else if (index != m_lastIndex + 1) {
            m_outOfOrder++;
            m_indexContiguous = false;
        }
        m_lastIndex = index;
    }

    CRITICAL_SECTION m_cs;
    bool m_verifyMeta;             ///< parse/validate the embedded test header (see SetVerifyMeta)
    bool m_captureLastFrame;       ///< keep a copy of the newest frame (see SetCaptureLastFrame)
    std::vector<BYTE> m_lastFrame; ///< copy of the most recent delivered frame (empty == none)
    long m_frames;
    int m_maxByte;
    int m_minByte;
    unsigned long long m_sum;
    unsigned long long m_sampled;

    /// Embedded frame-header verification (see GetMeta / FrameMetaStats).
    long m_withHeader;
    long m_badHeader;
    long m_badChecksum;
    long m_badSize;
    long m_badFeatures;
    long m_firstIndex;
    long m_lastIndex;
    long m_outOfOrder;
    bool m_indexContiguous;
    uint32_t m_lastFeatures[FRAME_META_NUM_FEATURES];
    uint8_t m_lastI2cRegs[CAMREG_COUNT];
};
