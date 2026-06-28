#pragma once

namespace r4sn::firmware
{
    // Derived from r4fn.elf.c (FUN_0040d6f0 print literals, FUN_0040a678 indexing).
    //
    // Range bin ↔ distance (FUN_0040c090 / line ~12227):
    //   distance_m = range_bin * kRangeResolutionM
    //
    // Range cube byte layout (same strides as dopplerOutputBufH in FUN_0040a678):
    //   offset = tile*0x100000 + range*0x1000 + chirp*0x40 + sub_ant*4
    //   [tile=12][range=256][chirp=64][sub_ant=16][I:int16 @+2, Q:int16 @+0]
    //
    // rangeOutputBufH @ 0x8BE00000 is filled after SPT range FFT with per-bin correction
    // from calibrationCoeffsBufH (FUN_0041d598 → sincos(fIdx*delay+phase)*ampli).

    inline constexpr float kRangeResolutionM = 0.07156503945589066f;
    inline constexpr float kVelocityResolutionMps = 0.07242187857627869f;

    // AoA FFT model (FUN_0040d6f0) — uniform λ-spacing, no per-VA geometry LUT in firmware.
    inline constexpr float kAntSpacingLambda = 0.49018988013267517f;
    inline constexpr float kAzmScale = 62.74430465698242f;
    inline constexpr unsigned kAoaFftSize = 128;

    // rangeOutputBufH samples already include EEPROM range calibration from SPT.
    inline constexpr bool kRangeCubeIsCalibrated = true;
}  // namespace r4sn::firmware
