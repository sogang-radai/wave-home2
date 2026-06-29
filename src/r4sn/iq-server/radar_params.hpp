#pragma once

// Firmware constants (r4fn.elf.c).
inline constexpr float kRangeResolutionM = 0.07156503945589066f;
inline constexpr float kVelocityResolutionMps = 0.07242187857627869f;
inline constexpr float kAntSpacingLambda = 0.49018988013267517f;
inline constexpr float kAzmScale = 62.74430465698242f;
inline constexpr unsigned kAoaFftSize = 128;
inline constexpr bool kRangeCubeIsCalibrated = true;

inline constexpr float kSpeedOfLight = 299792458.0f;
inline constexpr float kCarrierHz = 77.0e9f;
inline constexpr float kWavelength = kSpeedOfLight / kCarrierHz;
inline constexpr float kHalfLambda = kWavelength * 0.5f;
