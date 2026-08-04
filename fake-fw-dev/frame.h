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
/// Raw frame generation: renders the animated test pattern for the current
/// pixel format and stamps the frame-verification metadata header
/// (common/frame_meta.h) that the test program checks at the receiving end.

#ifndef FRAME_H
#define FRAME_H

#include "dcam.h"

#include <stdint.h>

/// Renders an animated test pattern into one frame slot, formatted for the
/// current pixel format, then stamps the frame-verification header over its
/// first bytes.  A diagonal moving luminance gradient plus a bright vertical
/// bar sweeping left-to-right makes motion obvious in any mode.  Each pixel
/// has a colour (R,G,B) so colour codings (YUV411, YUV422, RGB24, Bayer RAW8)
/// show hue; greyscale codings (MONO8/MONO16) use the luminance only.
///
/// @param cam       Camera whose streaming state drives the pixel format.
/// @param dst       Destination buffer; must be at least cam->frame_bytes bytes.
/// @param frame_no  Zero-based frame index (drives animation and the header).
void dcam_fill_frame_image(dcam_camera_t *cam, uint8_t *dst, unsigned frame_no);

#endif // FRAME_H
