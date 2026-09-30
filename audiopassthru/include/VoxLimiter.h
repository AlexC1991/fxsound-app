// SPDX-License-Identifier: MIT
//
// VoxLimiter.h - Copyright (c) 2026 AlexC1991
//
// This file is licensed under the MIT License (see LICENSING.md). It is an
// original work written for this fork. Note that the project as a whole is
// distributed under the GNU AGPL v3 (see LICENSE), because it is derived from
// FxSound, which is AGPL-3.0; distributing any build of this project is subject
// to the AGPL regardless of this file's MIT terms.
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.
//
// VoxLimiter - a live look-ahead limiter + adaptive normalizer for the FxSound
// audio path.
//
// PROVENANCE
// This file is a port of the author's own Rust audio engine, written as the
// mastering backend for their website (the vox-auto-master-v3 processing
// recipe). The Rust engine is the origin of this design and of every constant
// below - they are carried over verbatim, not re-derived here:
//   LIMITER_THRESHOLD_DB  -2.0    LIMITER_TARGET_DB  -1.0
//   LIMITER_KNEE_DB        2.0    LIMITER_LOOKAHEAD_MS 1.0
//   LIMITER_RELEASE_MS    20.0
// That engine is used to master the audio tracks users make on the site, so its
// chain is the reference this port follows rather than anything invented here.
// That recipe is: linked look-ahead limiter -> bass/treble shelves ->
// normalization to a target peak. The limiter comes FIRST in that order, and
// that ordering is the whole point: it is what lets you drive a signal hard and
// still get LOUDER sound instead of distortion.
//
// The Rust engine's own comment records why -2 dB of drive was chosen over -4.5:
//   -4.5 flattened every song by about 2 dB more, which listeners heard as a
//   muffled mix.
//
// WHY THIS EXISTS ON LINUX
// On Windows, FxSound's volume is applied by the OS at the audio endpoint
// (IAudioEndpointVolume, in sndDevices/sndDevicesVolCallbacks.cpp), i.e. after
// the DSP, so a boost lands where it can be heard. The Linux/PipeWire port had
// no equivalent volume stage at all, so the only loudness control - FxSound's
// own volume - was applied inside the DSP, where the engine's normalization
// absorbs it. That is why gaining volume was much harder here than on Windows.
//
// This limiter restores the missing behaviour: it is applied AFTER the DSP and
// AFTER FxSound's own volume, so a hard boost (volume above 100%, or a bass /
// sound-boost curve) is absorbed here instead of clipping at the device.
//
// Safety: any non-finite sample is repaired in place, and VOX_LIMITER=0
// disables processing entirely.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace vox {

inline bool limiterDisabledByEnv()
{
    const char* v = std::getenv("VOX_LIMITER");
    return v != nullptr && v[0] == '0';
}

class Limiter
{
public:
    // Constants carried over verbatim from the Rust recipe.
    static constexpr float THRESHOLD_DB = -2.0f;
    static constexpr float TARGET_DB    = -1.0f;
    static constexpr float KNEE_DB      = 2.0f;
    static constexpr float LOOKAHEAD_MS = 1.0f;
    static constexpr float RELEASE_MS   = 20.0f;

    // Adaptive normalizer: rides a slow makeup gain so quiet material comes up
    // rather than only loud material being held down.
    static constexpr float NORM_TARGET_DB = -14.0f;   // long-term RMS aim
    static constexpr float NORM_MAX_DB    = 9.0f;     // never add more than this
    static constexpr float NORM_RATE_DB_S = 1.5f;     // slow, to avoid pumping

    void prepare(int sampleRate, int channels)
    {
        sample_rate_ = std::max(1, sampleRate);
        channels_    = std::max(1, channels);
        makeup_db_   = TARGET_DB - THRESHOLD_DB;
        delay_       = std::max(1, (int)std::floor(sample_rate_ * LOOKAHEAD_MS / 1000.0f));
        release_alpha_ = 1.0f - std::exp(-1.0f / (sample_rate_ * RELEASE_MS / 1000.0f));
        norm_step_   = NORM_RATE_DB_S / sample_rate_;
        norm_gain_db_ = 0.0f;
        envelope_.assign(1 << 16, 0.0f);
        prepared_ = true;
    }

    // channels: array of `nch` float buffers, each `frames` long
    void process(float* const* channels, int nch, int frames)
    {
        processStrided(channels, nch, frames, 1);
    }

    // Same, but each channel's samples start `stride` floats apart (used for the
    // interleaved L/R buffer, where channel 1 starts at offset 1 and advances by 2).
    void processStrided(float* const* channels, int nch, int frames, int stride)
    {
        if (!prepared_ || limiterDisabledByEnv() || channels == nullptr || frames <= 0)
            return;
        if ((size_t)frames > envelope_.size())
            envelope_.resize(frames);

        // 1) linked sidechain envelope, instant attack / 20 ms release.
        //    Any non-finite sample is treated as 0 for the sidechain AND
        //    repaired in place below - never bail out, because bailing would
        //    leave the whole block unlimited (and hard-clipping downstream).
        float state = 0.0f;
        for (int f = 0; f < frames; ++f)
        {
            float side = 0.0f;
            for (int c = 0; c < nch; ++c)
            {
                float* p = channels[c] + (size_t)f * stride;
                if (!std::isfinite(*p)) { *p = 0.0f; continue; }
                const float a = std::fabs(*p);
                if (a > side) side = a;
            }
            const float wanted = gainForLevel(side);
            const float diff = wanted - state;
            state += (diff < 0.0f) ? diff : release_alpha_ * diff;
            envelope_[f] = state;
#ifdef VOX_LIMITER_PROBE
            if (side > probe_in_peak_) probe_in_peak_ = side;
            if (state < probe_min_gain_db_) probe_min_gain_db_ = state;
#endif
        }

        // 2) linear look-ahead ramp into each upcoming reduction
        for (int peak = 0; peak < frames; ++peak)
        {
            const float reduction = envelope_[peak];
            if (reduction >= 0.0f)
                continue;
            const int start = std::max(0, peak - delay_);
            const float width = (float)std::max(1, peak - start);
            for (int f = start; f < peak; ++f)
            {
                const float ramp = reduction * ((float)(f - start + 1) / width);
                if (ramp < envelope_[f])
                    envelope_[f] = ramp;
            }
        }

        // 3) adaptive makeup so quiet passages come up as well
        float peak_out = 0.0f, sum_sq = 0.0f;
        for (int f = 0; f < frames; ++f)
        {
            const float g = dbToLin(envelope_[f] + makeup_db_ + norm_gain_db_);
            for (int c = 0; c < nch; ++c)
            {
                float* p = channels[c] + (size_t)f * stride;
                float v = *p * g;
                if (v > 0.999f)  v = 0.999f;      // hard safety net; the limiter
                if (v < -0.999f) v = -0.999f;     // should already prevent this
                *p = v;
                peak_out = std::max(peak_out, std::fabs(v));
                sum_sq += v * v;
            }
        }

        // steer the slow makeup toward the RMS target
        const long n = (long)frames * nch;
        if (n > 0)
        {
            const float rms = std::sqrt(sum_sq / (float)n);
            const float err_db = NORM_TARGET_DB - linToDb(rms);
            const float step = std::clamp(err_db, -1.0f, 1.0f) * norm_step_;
            norm_gain_db_ = std::clamp(norm_gain_db_ + step, 0.0f, NORM_MAX_DB);
        }

        (void)peak_out;

#ifdef VOX_LIMITER_PROBE
        // Diagnostic: record what the limiter saw and did, about once a second.
        if ((++probe_blocks_ % 90) == 0) {
            if (FILE* f = std::fopen("/tmp/voxlimiter.log", "a")) {
                std::fprintf(f, "blocks=%lld max_in=%.4f max_out=%.4f min_gain_db=%.2f norm=%+.2f\n",
                             (long long)probe_blocks_, (double)probe_in_peak_,
                             (double)peak_out, (double)probe_min_gain_db_,
                             (double)norm_gain_db_);
                std::fclose(f);
            }
            probe_in_peak_ = 0.0f;
            probe_min_gain_db_ = 0.0f;
        }
#endif
    }

    bool bypassed() const { return bypass_; }

private:
    // soft-knee reduction curve, identical in shape to the Rust version
    static float gainForLevel(float level_lin)
    {
        const float level_db = linToDb(level_lin);
        const float overshoot = level_db - THRESHOLD_DB;
        const float half_knee = KNEE_DB * 0.5f;
        if (overshoot <= -half_knee) return 0.0f;
        if (overshoot <=  half_knee) return -0.5f * (overshoot + half_knee) * (overshoot + half_knee) / KNEE_DB;
        return -overshoot;
    }

    static float dbToLin(float db) { return std::pow(10.0f, db * 0.05f); }
    static float linToDb(float lin) { return (lin > 1e-9f) ? 20.0f * std::log10(lin) : -180.0f; }

    int   sample_rate_ = 48000;
    int   channels_    = 2;
    int   delay_       = 48;
    float release_alpha_ = 0.001f;
    float makeup_db_ = 1.0f;
    float norm_step_ = 0.0f;
    float norm_gain_db_ = 0.0f;
    bool  prepared_ = false;
    bool  bypass_ = false;
#ifdef VOX_LIMITER_PROBE
    long long probe_blocks_ = 0;
    float probe_in_peak_ = 0.0f;
    float probe_min_gain_db_ = 0.0f;
#endif
    std::vector<float> envelope_;
};

} // namespace vox
