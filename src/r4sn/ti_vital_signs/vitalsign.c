/*
* Copyright (C) 2024 Texas Instruments Incorporated
*
* All rights reserved not granted herein.
* Limited License.  
*
* Texas Instruments Incorporated grants a world-wide, royalty-free, 
* non-exclusive license under copyrights and patents it now or hereafter 
* owns or controls to make, have made, use, import, offer to sell and sell ("Utilize")
* this software subject to the terms herein.  With respect to the foregoing patent 
* license, such license is granted  solely to the extent that any such patent is necessary 
* to Utilize the software alone.  The patent license shall not apply to any combinations which 
* include this software, other than combinations with devices manufactured by or for TI ("TI Devices").  
* No hardware patent is licensed hereunder.
*
* Redistributions must preserve existing copyright notices and reproduce this license (including the 
* above copyright notice and the disclaimer and (if applicable) source code license limitations below) 
* in the documentation and/or other materials provided with the distribution
*
* Redistribution and use in binary form, without modification, are permitted provided that the following
* conditions are met:
*
*	* No reverse engineering, decompilation, or disassembly of this software is permitted with respect to any 
*     software provided in binary form.
*	* any redistribution and use are licensed by TI for use only with TI Devices.
*	* Nothing shall obligate TI to provide you with source code for the software licensed and provided to you in object code.
*
* If software source code is provided to you, modification and redistribution of the source code are permitted 
* provided that the following conditions are met:
*
*   * any redistribution and use of the source code, including any resulting derivative works, are licensed by 
*     TI for use only with TI Devices.
*   * any redistribution and use of any object code compiled from the source code and any resulting derivative 
*     works, are licensed by TI for use only with TI Devices.
*
* Neither the name of Texas Instruments Incorporated nor the names of its suppliers may be used to endorse or 
* promote products derived from this software without specific prior written permission.
*
* DISCLAIMER.
*
* THIS SOFTWARE IS PROVIDED BY TI AND TI'S LICENSORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, 
* BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. 
* IN NO EVENT SHALL TI AND TI'S LICENSORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR 
* CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, 
* OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, 
* OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE 
* POSSIBILITY OF SUCH DAMAGE.
*/

/**************************************************************************
 *************************** Include Files ********************************
 **************************************************************************/
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

#include "atan2sp_i.h"
#include "DSPF_sp_fftSPxSP_cn.h"
#include "vitalsign.h"
#include "cplx_types.h"
#include <common/sys_types.h>

/**************************vital signs locations*******************************************/
vsFeature vitalSignsOutput;
vsAntennaGeometry vitalSignsAntenna;
float      vsDataAngleFftOutBufLogAbsSum[VS_NUM_ANGLE_FFT * VS_NUM_ANGLE_FFT];
uint16_t   vsLastFramePeakIdxI;
uint16_t   vsLastFramePeakIdxJ;

uint16_t heartHistIndex       = 0;
uint16_t breathHistIndex      = 0;
uint16_t previousHeartPeak[4] = { 0 };
uint16_t vsMeanCntOffset0     = 0;
uint16_t vsMeanCntOffset1     = 30;

//Bit reversed order input for the FFT computation
unsigned char brevFft[64] = {
    0x0,
    0x20,
    0x10,
    0x30,
    0x8,
    0x28,
    0x18,
    0x38,
    0x4,
    0x24,
    0x14,
    0x34,
    0xc,
    0x2c,
    0x1c,
    0x3c,
    0x2,
    0x22,
    0x12,
    0x32,
    0xa,
    0x2a,
    0x1a,
    0x3a,
    0x6,
    0x26,
    0x16,
    0x36,
    0xe,
    0x2e,
    0x1e,
    0x3e,
    0x1,
    0x21,
    0x11,
    0x31,
    0x9,
    0x29,
    0x19,
    0x39,
    0x5,
    0x25,
    0x15,
    0x35,
    0xd,
    0x2d,
    0x1d,
    0x3d,
    0x3,
    0x23,
    0x13,
    0x33,
    0xb,
    0x2b,
    0x1b,
    0x3b,
    0x7,
    0x27,
    0x17,
    0x37,
    0xf,
    0x2f,
    0x1f,
    0x3f
};

extern uint32_t vsLoop;
extern uint32_t vsDataCount;
extern uint16_t vsRangeBin;
extern uint16_t indicateNoTarget;

cplxf_t vsDataMeanBuf[80] = { 0 };

void MmwDemo_computeVitalSignProcessing(cplxf_t *vsDataAngleFftOutBuf, uint16_t indicateNoTarget)
{

    uint16_t rangeBinIdx, peakCmputeIdx;
    uint32_t angleBinIdx;
	uint32_t frameSizeIdx;
	uint32_t spectrumBinIdx;
	
	//Breath rate array for computation of devation
    float    breathCircularBufferFull[100];

    uint16_t comparePreviousPeak    = 0;
    uint32_t addressRearrangeOffset = 0;
    uint16_t compareIndex           = 0;
	
	//Variable to compare the 5 peaks from the spectrum with preiovus peak
    uint16_t presentPeak[5]         = { 0 };
	
	//Contains the differnece between the present 5 peaks with the previous peak
    uint16_t peakDifferenceArray[5] = { 0 };
	float    compareValue           = 0;

    uint16_t       breathPeakIdx;
    volatile float breathPeakValue        = 0;
    uint16_t       heartPeakIdx;
	volatile float heartPeakValue         = 0;
	
	//Breath rate array containg Rangebin X Angle no breath rate estimate
    uint16_t       breathRateArray[45]    = { 0 };
	
	//Heart rate array containg Rangebin X Angle no breath rate estimate
    uint16_t       heartrate_array[45]    = { 0 };
    uint16_t       heartRateSub1Array[45] = { 0 };
    uint16_t       heartRateSub2Array[45] = { 0 };
    uint16_t       heartPeakDiff1         = 0;

    cplxf_t pVitalSignsBreathCircularBuffer[PHASE_FFT_SIZE]; // Circular Buffer for Breathing Waveform
    cplxf_t pVitalSignsHeartCircularBuffer[PHASE_FFT_SIZE]; // Circular Buffer for Heart Waveform
    cplxf_t pVitalSignsSpectrumCplx[PHASE_FFT_SIZE]; // Circular Buffer

    //Spectrum containing the FFT output after phase unwrap
    float pVitalSignsBreathAbsSpectrum[PHASE_FFT_SIZE];
    float pVitalSignsBreathAbsSpectrumStorage[PHASE_FFT_SIZE/2];
    float pVitalSignsHeartAbsSpectrum[PHASE_FFT_SIZE];
    float pVitalSignsHeartAbsSpectrumStorage[PHASE_FFT_SIZE / 2];
    float pVitalSignsHeartAbsSpectrumTemp[PHASE_FFT_SIZE / 2];
	
	//Decimated spectrum product containing the product of required frequency and its harmonic
    float decimatedSpectrumProduct[PHASE_FFT_SIZE / 2];

    float selPointPhase;
    float outputFilterBreathOut = 0; // Breathing waveform after the IIR-Filter
    float outputFilterHeartOut  = 0; // Cardiac waveform after the IIR-Filter

    volatile float phaseUsedComputationPrev = 0;
    volatile float breathWaveformDeviation;

    float         phaseUsedComputation; // Unwrapped Phase value used for computation
    float         computePhaseUnwrapPhasePeak;
    float         phasePrevFrame;
    float         diffPhaseCorrectionCum;
    uint16_t      cntbAddr;
    uint16_t      selAddr;
    float         aTanReal;
    float         aTanImag;
    int32_t       rad2D;
	float         tempBufferVitals[14] = { 0 };

    rad2D = 2;

    selAddr = 0;
    cntbAddr = 0;

    memset(&pVitalSignsHeartAbsSpectrumStorage[0], 0, (PHASE_FFT_SIZE/2) * sizeof(float));
    memset(&pVitalSignsBreathAbsSpectrumStorage[0], 0, (PHASE_FFT_SIZE/2) * sizeof(float));

    //Compute Arc Tan computation for all range bins/angle bins and all samples in each VS frame
    for (angleBinIdx = 0; angleBinIdx < VS_NUM_ANGLE_SEL_BIN; angleBinIdx++)
    {
        cntbAddr = angleBinIdx;

        for (rangeBinIdx = 0; rangeBinIdx < VS_NUM_RANGE_SEL_BIN; rangeBinIdx++)
        {

            memset(&pVitalSignsBreathCircularBuffer[0], 0, (VS_FFT_SIZE) * sizeof(float) * 2);
            memset(&pVitalSignsHeartCircularBuffer[0], 0, (VS_FFT_SIZE) * sizeof(float) * 2);

            diffPhaseCorrectionCum = 0;

            selAddr = cntbAddr;
            cntbAddr = (VS_NUM_ANGLE_SEL_BIN) + cntbAddr;

            addressRearrangeOffset = selAddr + vsDataCount * VS_NUM_RANGE_SEL_BIN * VS_NUM_ANGLE_SEL_BIN;
            if (addressRearrangeOffset >= VS_NUM_RANGE_SEL_BIN * VS_NUM_ANGLE_SEL_BIN * VS_TOTAL_FRAME)
            {
                addressRearrangeOffset = addressRearrangeOffset - (VS_NUM_RANGE_SEL_BIN * VS_NUM_ANGLE_SEL_BIN * VS_TOTAL_FRAME);
            }

            aTanReal = (float)vsDataAngleFftOutBuf[addressRearrangeOffset].real;
            aTanImag = (float)vsDataAngleFftOutBuf[addressRearrangeOffset].imag;

            phasePrevFrame = atan2sp_i((float)(aTanImag), (float)(aTanReal));

            phaseUsedComputationPrev = phasePrevFrame;
            selAddr                     = selAddr + VS_NUM_RANGE_SEL_BIN * VS_NUM_ANGLE_SEL_BIN;

            for (frameSizeIdx = 0; frameSizeIdx < VS_TOTAL_FRAME - 1; frameSizeIdx++)
            {

                addressRearrangeOffset = selAddr + vsDataCount * VS_NUM_RANGE_SEL_BIN * VS_NUM_ANGLE_SEL_BIN;
                if (addressRearrangeOffset >= VS_NUM_RANGE_SEL_BIN * VS_NUM_ANGLE_SEL_BIN * VS_TOTAL_FRAME)
                {
                    addressRearrangeOffset = addressRearrangeOffset - (VS_NUM_RANGE_SEL_BIN * VS_NUM_ANGLE_SEL_BIN * VS_TOTAL_FRAME);
                }

                aTanReal = (float)vsDataAngleFftOutBuf[addressRearrangeOffset].real;
                aTanImag = (float)vsDataAngleFftOutBuf[addressRearrangeOffset].imag;

                selAddr = selAddr + VS_NUM_RANGE_SEL_BIN * VS_NUM_ANGLE_SEL_BIN;

                selPointPhase = atan2sp_i((float)(aTanImag), (float)(aTanReal));

                computePhaseUnwrapPhasePeak = MmwDemo_computePhaseUnwrap(selPointPhase, phasePrevFrame, &diffPhaseCorrectionCum);
                phasePrevFrame  = selPointPhase;

                phaseUsedComputation     = computePhaseUnwrapPhasePeak - phaseUsedComputationPrev;
                phaseUsedComputationPrev = computePhaseUnwrapPhasePeak;

                outputFilterBreathOut = phaseUsedComputation;
                outputFilterHeartOut  = phaseUsedComputation;

                pVitalSignsBreathCircularBuffer[frameSizeIdx].real = outputFilterBreathOut;
                pVitalSignsHeartCircularBuffer[frameSizeIdx].real  = outputFilterHeartOut;
            }

            /*
            if (angleBinIdx == 5 && rangeBinIdx == 3)
            {
                // output to GUI for visualizer
                for (frameSizeIdx = 0; frameSizeIdx < 15; frameSizeIdx++)
                {

                    vitalSignsOutput.VitalSignsBreathCircularBuffer[frameSizeIdx + 0] = pVitalSignsBreathCircularBuffer[frameSizeIdx].real / 3;
                    vitalSignsOutput.VitalSignsHeartCircularBuffer[frameSizeIdx + 0]  = pVitalSignsHeartCircularBuffer[frameSizeIdx * 3].real / 3;
                }
            }
			*/
			
            // last 100 breath data points for breathing deviation calculation
            if (angleBinIdx == 5 && rangeBinIdx == 3)
            {
                // output to GUI for visualizer
                for (frameSizeIdx = 0; frameSizeIdx < 100; frameSizeIdx++)
                {

                    breathCircularBufferFull[frameSizeIdx + 0] = pVitalSignsBreathCircularBuffer[frameSizeIdx].real;
                }
            }


            // N pts FFT for long time  breath waveform.
            //  Input to the FFT needs to be complex
            memset((uint8_t *)&pVitalSignsSpectrumCplx[0], 0, (VS_FFT_SIZE) * sizeof(cplx32_t));

            DSPF_sp_fftSPxSP_cn(
                VS_FFT_SIZE,
                (float *)&pVitalSignsBreathCircularBuffer,
                (float *)vitalSignsSpectrumTwiddle,
                (float *)&pVitalSignsSpectrumCplx,
                brevFft,
                rad2D,
                0,
                VS_FFT_SIZE);

            MmwDemo_computeMagnitudeSquared(
                pVitalSignsSpectrumCplx,
                pVitalSignsBreathAbsSpectrum,
                VS_FFT_SIZE);


            // Find the breath peak and SNR
            breathPeakValue = 0;
            for (spectrumBinIdx = BREATH_INDEX_START; spectrumBinIdx < BREATH_INDEX_END; spectrumBinIdx++)
            {
                compareValue = pVitalSignsBreathAbsSpectrum[spectrumBinIdx] + pVitalSignsBreathAbsSpectrum[spectrumBinIdx + 1] + pVitalSignsBreathAbsSpectrum[spectrumBinIdx - 1];
                if (compareValue > breathPeakValue)
                {
                    breathPeakValue = compareValue;
                    breathPeakIdx   = spectrumBinIdx;
                }
            }


            for (spectrumBinIdx = 0; spectrumBinIdx < (PHASE_FFT_SIZE/4); spectrumBinIdx++)
            {
                decimatedSpectrumProduct[spectrumBinIdx] = pVitalSignsBreathAbsSpectrum[2 * spectrumBinIdx] * pVitalSignsBreathAbsSpectrum[spectrumBinIdx];
            }

            for (spectrumBinIdx = BREATH_INDEX_START; spectrumBinIdx < BREATH_INDEX_END; spectrumBinIdx++)
            {
                pVitalSignsBreathAbsSpectrumStorage[spectrumBinIdx] = pVitalSignsBreathAbsSpectrumStorage[spectrumBinIdx] + pVitalSignsBreathAbsSpectrum[spectrumBinIdx];
            }


            for (spectrumBinIdx = HEART_INDEX_START; spectrumBinIdx < HEART_INDEX_END; spectrumBinIdx++)
            {
                pVitalSignsHeartAbsSpectrumStorage[spectrumBinIdx] = pVitalSignsHeartAbsSpectrumStorage[spectrumBinIdx] + decimatedSpectrumProduct[spectrumBinIdx];
            }


            
            heartPeakValue = 0;

            //Decimated spectrum product contains the product of 
            for (spectrumBinIdx = HEART_INDEX_START; spectrumBinIdx < HEART_INDEX_END; spectrumBinIdx++)
            {
                compareValue = decimatedSpectrumProduct[spectrumBinIdx] + decimatedSpectrumProduct[spectrumBinIdx + 1] + decimatedSpectrumProduct[spectrumBinIdx - 1];
                if (compareValue > heartPeakValue)
                {
                    heartPeakValue = compareValue;
                    heartPeakIdx   = spectrumBinIdx;
                }
            }

            breathRateArray[rangeBinIdx + angleBinIdx * 5] = (breathPeakIdx);

            heartrate_array[rangeBinIdx + angleBinIdx * 5] = (heartPeakIdx);

            decimatedSpectrumProduct[heartPeakIdx]     = 0;
            decimatedSpectrumProduct[heartPeakIdx + 1] = 0;
            decimatedSpectrumProduct[heartPeakIdx - 1] = 0;

            heartPeakValue = 0;
            for (spectrumBinIdx = HEART_INDEX_START; spectrumBinIdx < HEART_INDEX_END; spectrumBinIdx++)
            {

                compareValue = decimatedSpectrumProduct[spectrumBinIdx] + decimatedSpectrumProduct[spectrumBinIdx + 1] + decimatedSpectrumProduct[spectrumBinIdx - 1];
                if (compareValue > heartPeakValue)
                {
                    heartPeakValue = compareValue;
                    heartPeakIdx   = spectrumBinIdx;
                }
            }

            heartRateSub1Array[rangeBinIdx + angleBinIdx * 5] = (heartPeakIdx);

            decimatedSpectrumProduct[heartPeakIdx]     = 0;
            decimatedSpectrumProduct[heartPeakIdx + 1] = 0;
            decimatedSpectrumProduct[heartPeakIdx - 1] = 0;

            heartPeakValue = 0;
            for (spectrumBinIdx = HEART_INDEX_START; spectrumBinIdx < HEART_INDEX_END; spectrumBinIdx++)
            {

                compareValue = decimatedSpectrumProduct[spectrumBinIdx] + decimatedSpectrumProduct[spectrumBinIdx + 1] + decimatedSpectrumProduct[spectrumBinIdx - 1];
                if (compareValue > heartPeakValue)
                {
                    heartPeakValue = compareValue;
                    heartPeakIdx   = spectrumBinIdx;
                }
            }

            heartRateSub2Array[rangeBinIdx + angleBinIdx * 5] = (heartPeakIdx);
        }
    }


    breathPeakValue = 0;
    for (spectrumBinIdx = BREATH_INDEX_START; spectrumBinIdx < BREATH_INDEX_END; spectrumBinIdx++)
    {
        compareValue = pVitalSignsBreathAbsSpectrumStorage[spectrumBinIdx] + pVitalSignsBreathAbsSpectrumStorage[spectrumBinIdx + 1] + pVitalSignsBreathAbsSpectrumStorage[spectrumBinIdx - 1];

        if (compareValue > breathPeakValue)
        {
            breathPeakValue = compareValue;
            breathPeakIdx   = spectrumBinIdx;
        }
    }


    memset(&pVitalSignsBreathAbsSpectrum[0], 0, VS_FFT_SIZE * sizeof(float));
    // count the peak values for all range/angle bins
    for (peakCmputeIdx = 0; peakCmputeIdx < 45; peakCmputeIdx++)
    {
        pVitalSignsBreathAbsSpectrum[breathRateArray[peakCmputeIdx]]++;
    }

    breathPeakValue = 0;

    compareValue = 0;
    for (spectrumBinIdx = BREATH_INDEX_START; spectrumBinIdx < BREATH_INDEX_END; spectrumBinIdx++)
    {
        compareValue = pVitalSignsBreathAbsSpectrum[spectrumBinIdx] + pVitalSignsBreathAbsSpectrum[spectrumBinIdx - 1] + pVitalSignsBreathAbsSpectrum[spectrumBinIdx + 1];
        if (compareValue > breathPeakValue)
        {
            breathPeakValue = compareValue;
            breathPeakIdx   = spectrumBinIdx;
        }
    }
    breathHistIndex = breathPeakIdx;

    //Conisder only 3 bins for heart rate determination. Zero out the other range bins
    for (peakCmputeIdx = 0; peakCmputeIdx < 9; peakCmputeIdx++)
    {
        heartrate_array[5 * peakCmputeIdx]          = 0;
        heartrate_array[5 * peakCmputeIdx + 4]      = 0;
        heartRateSub1Array[5 * peakCmputeIdx]     = 0;
        heartRateSub1Array[5 * peakCmputeIdx + 4] = 0;
        heartRateSub2Array[5 * peakCmputeIdx]     = 0;
        heartRateSub2Array[5 * peakCmputeIdx + 4] = 0;
    }


    memset(&pVitalSignsHeartAbsSpectrum[0], 0, VS_FFT_SIZE * sizeof(float));

    // count the peak values for all range/angle bins
    for (peakCmputeIdx = 0; peakCmputeIdx < 45; peakCmputeIdx++)
    {
        pVitalSignsHeartAbsSpectrum[heartrate_array[peakCmputeIdx]]++;
        pVitalSignsHeartAbsSpectrum[heartRateSub1Array[peakCmputeIdx]]++;
        // pVitalSignsHeartAbsSpectrum[heartRateSub2Array[peakCmputeIdx]]++;
    }


    heartPeakValue = 0;
    for (spectrumBinIdx = HEART_INDEX_START; spectrumBinIdx < HEART_INDEX_END; spectrumBinIdx++)
    {
        compareValue = pVitalSignsHeartAbsSpectrum[spectrumBinIdx] + pVitalSignsHeartAbsSpectrum[spectrumBinIdx + 1] + pVitalSignsHeartAbsSpectrum[spectrumBinIdx - 1] + pVitalSignsHeartAbsSpectrum[spectrumBinIdx - 2] + pVitalSignsHeartAbsSpectrum[spectrumBinIdx + 2];
        if (compareValue > heartPeakValue)
        {
            heartPeakValue = compareValue;
            heartPeakIdx   = spectrumBinIdx;
        }
    }

    heartHistIndex = heartPeakIdx;


    memcpy((uint8_t *)&pVitalSignsHeartAbsSpectrumTemp[0], (uint8_t *)&pVitalSignsHeartAbsSpectrumStorage[0], (PHASE_FFT_SIZE/2) * sizeof(float));

    for (peakCmputeIdx = 0; peakCmputeIdx < 5; peakCmputeIdx++)
    {

        heartPeakValue = 0;
        for (spectrumBinIdx = HEART_INDEX_START; spectrumBinIdx < HEART_INDEX_END; spectrumBinIdx++)
        {
            compareValue = pVitalSignsHeartAbsSpectrumTemp[spectrumBinIdx] + pVitalSignsHeartAbsSpectrumTemp[spectrumBinIdx - 1] + pVitalSignsHeartAbsSpectrumTemp[spectrumBinIdx + 1];
            if (compareValue > heartPeakValue)
            {
                heartPeakValue = compareValue;
                heartPeakIdx   = spectrumBinIdx;
            }
        }
        presentPeak[peakCmputeIdx]                        = heartPeakIdx;
        pVitalSignsHeartAbsSpectrumTemp[heartPeakIdx]     = 0;
        pVitalSignsHeartAbsSpectrumTemp[heartPeakIdx + 1] = 0;
        pVitalSignsHeartAbsSpectrumTemp[heartPeakIdx - 1] = 0;
    }

    //Find correlation with previous peaks
    comparePreviousPeak = previousHeartPeak[3];

    if (presentPeak[0] > comparePreviousPeak)
        peakDifferenceArray[0] = presentPeak[0] - comparePreviousPeak;
    else
        peakDifferenceArray[0] = comparePreviousPeak - presentPeak[0];

    if (presentPeak[1] > comparePreviousPeak)
        peakDifferenceArray[1] = presentPeak[1] - comparePreviousPeak;
    else
        peakDifferenceArray[1] = comparePreviousPeak - presentPeak[1];

    if (presentPeak[2] > comparePreviousPeak)
        peakDifferenceArray[2] = presentPeak[2] - comparePreviousPeak;
    else
        peakDifferenceArray[2] = comparePreviousPeak - presentPeak[2];

    if (presentPeak[3] > comparePreviousPeak)
        peakDifferenceArray[3] = presentPeak[3] - comparePreviousPeak;
    else
        peakDifferenceArray[3] = comparePreviousPeak - presentPeak[3];

    if (presentPeak[4] > comparePreviousPeak)
        peakDifferenceArray[4] = presentPeak[4] - comparePreviousPeak;
    else
        peakDifferenceArray[4] = comparePreviousPeak - presentPeak[4];


    compareValue = 100;
    compareIndex = 0;
    for (peakCmputeIdx = 0; peakCmputeIdx < 5; peakCmputeIdx++)
    {
        if (peakDifferenceArray[peakCmputeIdx] < compareValue)
        {
            compareValue = peakDifferenceArray[peakCmputeIdx];
            compareIndex = peakCmputeIdx;
        }
    }

    //If the correlation with previous peaks is less than the decision threshold then take the peak from histogram peak
    if (compareValue < HEART_RATE_DECISION_THRESH)
    {
        heartPeakIdx = presentPeak[compareIndex];
    }
    else
    {
        heartPeakIdx = heartHistIndex;
    }

    heartPeakDiff1 = 0;
    if (heartPeakIdx > previousHeartPeak[0])
    {
        heartPeakDiff1 = heartPeakIdx - previousHeartPeak[0];
    }
    else
    {
        heartPeakDiff1 = previousHeartPeak[0] - heartPeakIdx;
    }

    //Prevent jumps in data by placing the jump limit
    if (heartPeakDiff1 > HEART_RATE_JUMP_LIMIT & vsLoop > VITALS_MASK_LOOP_NO)
    {
        if (heartPeakIdx > previousHeartPeak[0])
        {
            heartPeakIdx = previousHeartPeak[0] + HEART_RATE_JUMP_LIMIT;
        }
        else
        {
            heartPeakIdx = previousHeartPeak[0] - HEART_RATE_JUMP_LIMIT;
        }
    }

    if (vsLoop > 4)
    {
        previousHeartPeak[3] = previousHeartPeak[2];
        previousHeartPeak[2] = previousHeartPeak[1];
        previousHeartPeak[1] = previousHeartPeak[0];
        previousHeartPeak[0] = heartPeakIdx;
    }
    else if (vsLoop == 0)
    {
        previousHeartPeak[3] = 0;
        previousHeartPeak[2] = 0;
        previousHeartPeak[1] = 0;
        previousHeartPeak[0] = 0;
    }


    breathWaveformDeviation           = MmwDemo_computeMyDeviation(&breathCircularBufferFull[59], 40);
    vitalSignsOutput.breathingDeviation = breathWaveformDeviation;

    vitalSignsOutput.heartRate     = (heartPeakIdx)*SPECTRUM_MULTIPLICATION_FACTOR;
    vitalSignsOutput.breathingRate = (breathHistIndex)*SPECTRUM_MULTIPLICATION_FACTOR;

    vitalSignsOutput.rangebin = vsRangeBin;
    vitalSignsOutput.id       = 0;

    if (indicateNoTarget == 1)
    {
        vitalSignsOutput.id                 = 0;
        vitalSignsOutput.rangebin           = 0;
        vitalSignsOutput.breathingRate      = 0;
        vitalSignsOutput.heartRate          = 0;
        vitalSignsOutput.breathingDeviation = 0;
	    memset(&vitalSignsOutput.VitalSignsBreathCircularBuffer[0], 0, 15 * sizeof(float));
		memset(&vitalSignsOutput.VitalSignsHeartCircularBuffer[0], 0, 15 * sizeof(float));
    }

    if (vsLoop < VITALS_MASK_LOOP_NO)
    {
        vitalSignsOutput.breathingRate = 0;
        vitalSignsOutput.heartRate     = 0;
    }

    if(vsLoop >= VITALS_MASK_LOOP_NO)
	{
	    memcpy((uint8_t *)&tempBufferVitals, (uint8_t *)&vitalSignsOutput.VitalSignsBreathCircularBuffer[0], 14 * sizeof(float));
	    memcpy((uint8_t *)&vitalSignsOutput.VitalSignsBreathCircularBuffer[1], (uint8_t *)&tempBufferVitals, 14 * sizeof(float));
	
	    memcpy((uint8_t *)&tempBufferVitals, (uint8_t *)&vitalSignsOutput.VitalSignsHeartCircularBuffer[0], 14 * sizeof(float));
	    memcpy((uint8_t *)&vitalSignsOutput.VitalSignsHeartCircularBuffer[1], (uint8_t *)&tempBufferVitals, 14 * sizeof(float));
	
	    vitalSignsOutput.VitalSignsBreathCircularBuffer[0] = vitalSignsOutput.breathingRate;
	    vitalSignsOutput.VitalSignsHeartCircularBuffer[0] = vitalSignsOutput.heartRate;
	}

    return;
}


void MmwDemo_computeMagnitudeSquared(cplxf_t *inpBuff, float *magSqrdBuff, uint32_t numSamples)
{
    uint32_t i;
    for (i = 0; i < numSamples; i++)
    {
        magSqrdBuff[i] = inpBuff[i].real * inpBuff[i].real +
            (float)inpBuff[i].imag * (float)inpBuff[i].imag;
    }
}

float MmwDemo_computeMyDeviation(float *a, int n)
{
    if (a == NULL || n < 1)
        return -1.0;
    float sumX  = 0.0;
    float sumX2 = 0.0;
    int   i     = 0;
    for (i = 0; i < n; i++)
    {
        sumX += a[i];
        sumX2 += a[i] * a[i];
    }
    return sumX2 / n - (sumX / n) * (sumX / n);
}

uint32_t MmwDemo_runCopyTranspose64b(uint64_t *src, uint64_t *dest, uint32_t size, int32_t offset, uint32_t stride, uint32_t pairs)
{
    int32_t i, j, k;
    j = 0;
    for (i = 0; i < (int32_t)size; i++)
    {
        for (k = 0; k < (int32_t)pairs; k++)
        {
            dest[j + k + i * offset] = src[pairs * i + k];
        }
        j += (int32_t)stride;
    }
    return (1);
}


void MmwDemo_runPreProcess(cplxf_t *pDataIn, uint32_t vsDataCount)
{


    cplxf_t *pDataIn_buf_CPLXF;


    cplxf_t vsDataAngleFftOutBufTemp[VS_NUM_ANGLE_FFT * VS_NUM_ANGLE_FFT];
    cplxf_t pDataTemp[64];

    cplxf_t  pDataTempFftout[64];
    float    VS_log2abs_buf_temp[16];
    uint16_t vsLastFramePeakIdxI1;
    uint16_t vsLastFramePeakIdxJ1;
    uint16_t vsLastFramePeakIdxI3;
    uint16_t vsLastFramePeakIdxJ3;
    uint16_t vsLastFramePeakIdxI2;
    uint16_t vsLastFramePeakIdxJ2;

	uint16_t azimFftIdx;
	uint16_t elevFftIdx;
	uint16_t dataMeanIdx;
    uint32_t rangeBinIdx;

    float    fftLogAbsPeakValue = 0;
    int32_t  rad2D              = 2;
 
    pDataIn_buf_CPLXF = (cplxf_t *)&pDataIn[0];

    uint16_t dataIdx;
    uint32_t dataSetIdx;
    uint16_t fftDataIdx;
	uint16_t dataArrangeIdxCol;
	uint16_t virtualAntennaValid;
	uint16_t virtualAntennaIdx;
	uint16_t virtualAntennaPointer;
	uint16_t numberVirtualAntenna;


    dataIdx = 0;
    dataSetIdx = 0;

    rad2D = 4;

    pDataIn_buf_CPLXF = (cplxf_t *)&pDataIn[0];


    for (dataMeanIdx = 0; dataMeanIdx < VS_NUM_RANGE_SEL_BIN * VS_NUM_VIRTUAL_CHANNEL; dataMeanIdx++)
    {
        vsDataMeanBuf[vsMeanCntOffset0 + dataMeanIdx].real += pDataIn_buf_CPLXF[dataIdx].real;
        vsDataMeanBuf[vsMeanCntOffset0 + dataMeanIdx].imag += pDataIn_buf_CPLXF[dataIdx].imag;
        dataIdx++;
    }


    pDataIn_buf_CPLXF = (cplxf_t *)&pDataIn[0];
    dataIdx              = 0;


    for (dataMeanIdx = 0; dataMeanIdx < VS_NUM_RANGE_SEL_BIN * VS_NUM_VIRTUAL_CHANNEL; dataMeanIdx++)
    {
        pDataIn_buf_CPLXF[dataIdx].real -= vsDataMeanBuf[dataMeanIdx + vsMeanCntOffset1].real;
        pDataIn_buf_CPLXF[dataIdx].imag -= vsDataMeanBuf[dataMeanIdx + vsMeanCntOffset1].imag;
        dataIdx++;
    }


    pDataIn_buf_CPLXF = (cplxf_t *)&pDataIn[0];

    rangeBinIdx = 0;
    azimFftIdx = 0;
    dataIdx  = 0;
	numberVirtualAntenna = vitalSignsAntenna.numRxAntennas * vitalSignsAntenna.numRxAntennas;

    switch (vsLastFramePeakIdxI)
    {
        case 0:
            vsLastFramePeakIdxI1 = 15;
            vsLastFramePeakIdxI2 = 0;
            vsLastFramePeakIdxI3 = 1;
            break;
        case VS_NUM_ANGLE_FFT - 1:
            vsLastFramePeakIdxI1 = VS_NUM_ANGLE_FFT - 2;
            vsLastFramePeakIdxI2 = VS_NUM_ANGLE_FFT - 1;
            vsLastFramePeakIdxI3 = 0;
            break;
        default:
            vsLastFramePeakIdxI1 = vsLastFramePeakIdxI - 1;
            vsLastFramePeakIdxI2 = vsLastFramePeakIdxI;
            vsLastFramePeakIdxI3 = vsLastFramePeakIdxI + 1;
            break;
    }
    switch (vsLastFramePeakIdxJ)
    {
        case 0:
            vsLastFramePeakIdxJ1 = 15;
            vsLastFramePeakIdxJ2 = 0;
            vsLastFramePeakIdxJ3 = 1;
            break;
        case VS_NUM_ANGLE_FFT - 1:
            vsLastFramePeakIdxJ1 = VS_NUM_ANGLE_FFT - 2;
            vsLastFramePeakIdxJ2 = VS_NUM_ANGLE_FFT - 1;
            vsLastFramePeakIdxJ3 = 0;
            break;
        default:
            vsLastFramePeakIdxJ1 = vsLastFramePeakIdxJ - 1;
            vsLastFramePeakIdxJ2 = vsLastFramePeakIdxJ;
            vsLastFramePeakIdxJ3 = vsLastFramePeakIdxJ + 1;
            break;
    }


    dataSetIdx = vsDataCount * VS_NUM_RANGE_SEL_BIN * VS_NUM_ANGLE_SEL_BIN;
    fftDataIdx = 0;
    for (rangeBinIdx = 0; rangeBinIdx < VS_NUM_RANGE_SEL_BIN; rangeBinIdx++)
    {

        //----------1st row FFT---------------------------------------------------
        for (azimFftIdx = 0; azimFftIdx < vitalSignsAntenna.numAntRow; azimFftIdx++)
		//for (azimFftIdx = 0; azimFftIdx < 2; azimFftIdx++)
        {

            memset((uint8_t *)&pDataTemp[0], 0, VS_NUM_ANGLE_FFT * sizeof(cplx32_t));
			for(dataArrangeIdxCol = 0; dataArrangeIdxCol<VS_NUM_ANGLE_FFT; dataArrangeIdxCol++)
		    {   virtualAntennaPointer=0;
		        virtualAntennaValid = 0;
			    for(virtualAntennaIdx=0; virtualAntennaIdx<numberVirtualAntenna; virtualAntennaIdx++)
			    {
			        if ((vitalSignsAntenna.vsActiveAntennaGeometryCfg[virtualAntennaIdx].row == azimFftIdx) && (vitalSignsAntenna.vsActiveAntennaGeometryCfg[virtualAntennaIdx].col == dataArrangeIdxCol))
					{
						virtualAntennaPointer = virtualAntennaIdx;
						virtualAntennaValid = 1;
					}
			    }
				
				if(virtualAntennaValid == 1)
				{
			       memcpy((uint8_t *)&pDataTemp[dataArrangeIdxCol], (uint8_t *)&pDataIn_buf_CPLXF[virtualAntennaPointer + dataIdx ], sizeof(cplx32_t));
				}
			}
			
			
			/*
            switch (azimFftIdx)
            {
                case 1:
                    memset((uint8_t *)&pDataTemp[2], 0, (VS_NUM_ANGLE_FFT - 2) * sizeof(cplx32_t));
                    memcpy((uint8_t *)&pDataTemp[0], (uint8_t *)&pDataIn_buf_CPLXF[dataIdx], 2 * sizeof(cplx32_t));
                    dataIdx = dataIdx + 2;
                    break;
                case 0:
                    memset((uint8_t *)&pDataTemp[4], 0, (VS_NUM_ANGLE_FFT - 4) * sizeof(cplx32_t));
                    memcpy((uint8_t *)&pDataTemp[0], (uint8_t *)&pDataIn_buf_CPLXF[dataIdx], 4 * sizeof(cplx32_t));
                    dataIdx = dataIdx + 4;
                    break;
            }
			*/


            DSPF_sp_fftSPxSP_cn(
                VS_NUM_ANGLE_FFT,
                (float *)&pDataTemp[0],
                (float *)angleFFTSpectrumTwiddle,
                (float *)&pDataTemp[VS_NUM_ANGLE_FFT],
                brevFft,
                rad2D,
                0,
                VS_NUM_ANGLE_FFT);


            MmwDemo_runCopyTranspose64b((uint64_t *)&pDataTemp[VS_NUM_ANGLE_FFT], (uint64_t *)&pDataTempFftout[azimFftIdx], VS_NUM_ANGLE_FFT, 0, 2, 1);
        }

        dataIdx = dataIdx + 6;

        fftDataIdx = 0;


        //----------2nd column FFT---------------------------------------------------

        for (elevFftIdx = 0; elevFftIdx < VS_NUM_ANGLE_FFT; elevFftIdx++)
        {
            memset((uint8_t *)&pDataTemp[2], 0, (VS_NUM_ANGLE_FFT - 2) * sizeof(cplx32_t));

            memcpy((uint8_t *)&pDataTemp[0], (uint8_t *)&pDataTempFftout[fftDataIdx], 2 * sizeof(cplx32_t));
            fftDataIdx = fftDataIdx + 2;


            DSPF_sp_fftSPxSP_cn(
                VS_NUM_ANGLE_FFT,
                (float *)&pDataTemp[0],
                (float *)angleFFTSpectrumTwiddle,
                (float *)&pDataTemp[VS_NUM_ANGLE_FFT],
                brevFft,
                rad2D,
                0,
                VS_NUM_ANGLE_FFT);


            memcpy((uint8_t *)&vsDataAngleFftOutBufTemp[elevFftIdx * VS_NUM_ANGLE_FFT], (uint8_t *)&pDataTemp[VS_NUM_ANGLE_FFT], VS_NUM_ANGLE_FFT * sizeof(cplx32_t));


            MmwDemo_computeMagnitudeSquared(
                (cplxf_t *)&pDataTemp[VS_NUM_ANGLE_FFT],
                (float *)&VS_log2abs_buf_temp[0],
                VS_NUM_ANGLE_FFT);
            //------------------------------------------------------------

            for (azimFftIdx = 0; azimFftIdx < VS_NUM_ANGLE_FFT; azimFftIdx++)
            {
                vsDataAngleFftOutBufLogAbsSum[elevFftIdx * VS_NUM_ANGLE_FFT + azimFftIdx] += VS_log2abs_buf_temp[azimFftIdx];
            }
        }


        //----------3rd save 9 ptrs in L3------------------------------------------------------------------------------

        vsDataAngleFftOutBuf[dataSetIdx] = vsDataAngleFftOutBufTemp[vsLastFramePeakIdxJ1 * VS_NUM_ANGLE_FFT + vsLastFramePeakIdxI1];
        dataSetIdx++;
        vsDataAngleFftOutBuf[dataSetIdx] = vsDataAngleFftOutBufTemp[vsLastFramePeakIdxJ1 * VS_NUM_ANGLE_FFT + vsLastFramePeakIdxI2];
        dataSetIdx++;
        vsDataAngleFftOutBuf[dataSetIdx] = vsDataAngleFftOutBufTemp[vsLastFramePeakIdxJ1 * VS_NUM_ANGLE_FFT + vsLastFramePeakIdxI3];
        dataSetIdx++;
        vsDataAngleFftOutBuf[dataSetIdx] = vsDataAngleFftOutBufTemp[vsLastFramePeakIdxJ2 * VS_NUM_ANGLE_FFT + vsLastFramePeakIdxI1];
        dataSetIdx++;
        vsDataAngleFftOutBuf[dataSetIdx] = vsDataAngleFftOutBufTemp[vsLastFramePeakIdxJ2 * VS_NUM_ANGLE_FFT + vsLastFramePeakIdxI2];
        dataSetIdx++;
        vsDataAngleFftOutBuf[dataSetIdx] = vsDataAngleFftOutBufTemp[vsLastFramePeakIdxJ2 * VS_NUM_ANGLE_FFT + vsLastFramePeakIdxI3];
        dataSetIdx++;
        vsDataAngleFftOutBuf[dataSetIdx] = vsDataAngleFftOutBufTemp[vsLastFramePeakIdxJ3 * VS_NUM_ANGLE_FFT + vsLastFramePeakIdxI1];
        dataSetIdx++;
        vsDataAngleFftOutBuf[dataSetIdx] = vsDataAngleFftOutBufTemp[vsLastFramePeakIdxJ3 * VS_NUM_ANGLE_FFT + vsLastFramePeakIdxI2];
        dataSetIdx++;
        vsDataAngleFftOutBuf[dataSetIdx] = vsDataAngleFftOutBufTemp[vsLastFramePeakIdxJ3 * VS_NUM_ANGLE_FFT + vsLastFramePeakIdxI3];
        dataSetIdx++;
    }

    if ((vsDataCount == 127) | (vsLoop == 0 & vsDataCount == 1))
    {
        //----------4th search the 2D peak index as next frame data input---------------------------------------------------
        fftLogAbsPeakValue = 0;

        for (azimFftIdx = 0; azimFftIdx < VS_NUM_ANGLE_FFT; azimFftIdx++)
        {
            for (elevFftIdx = 0; elevFftIdx < VS_NUM_ANGLE_FFT; elevFftIdx++)
            {
                if (vsDataAngleFftOutBufLogAbsSum[azimFftIdx * VS_NUM_ANGLE_FFT + elevFftIdx] > fftLogAbsPeakValue)
                {
                    fftLogAbsPeakValue = vsDataAngleFftOutBufLogAbsSum[azimFftIdx * VS_NUM_ANGLE_FFT + elevFftIdx];
                    vsLastFramePeakIdxI  = elevFftIdx;
                    vsLastFramePeakIdxJ  = azimFftIdx;
                }
            }
        }
        memset((uint8_t *)&vsDataAngleFftOutBufLogAbsSum[0], 0, (VS_NUM_ANGLE_FFT * VS_NUM_ANGLE_FFT) * sizeof(float));
    }


    if (vsDataCount == 127)
    {


        for (dataMeanIdx = 0; dataMeanIdx < VS_NUM_RANGE_SEL_BIN * VS_NUM_VIRTUAL_CHANNEL; dataMeanIdx++)
        {
            vsDataMeanBuf[dataMeanIdx + vsMeanCntOffset0].real /= VS_TOTAL_FRAME;
            vsDataMeanBuf[dataMeanIdx + vsMeanCntOffset0].imag /= VS_TOTAL_FRAME;
        }


        memset((uint8_t *)&vsDataMeanBuf[vsMeanCntOffset1], 0, sizeof(120));


        if (vsMeanCntOffset0 == 0)
        {
            vsMeanCntOffset0 = 30;
            vsMeanCntOffset1 = 0;
        }
        else
        {
            vsMeanCntOffset0 = 0;
            vsMeanCntOffset1 = 30;
        }
    }
}


int MmwDemo_genTwiddle(float *w, int n)
{
    int i, j, k;

    for (j = 1, k = 0; j < n >> 2; j = j << 2)
    {
        for (i = 0; i < n >> 2; i += j)
        {
            w[k + 5] = sin(6.0 * PI * i / n);
            w[k + 4] = cos(6.0 * PI * i / n);

            w[k + 3] = sin(4.0 * PI * i / n);
            w[k + 2] = cos(4.0 * PI * i / n);

            w[k + 1] = sin(2.0 * PI * i / n);
            w[k + 0] = cos(2.0 * PI * i / n);

            k += 6;
        }
    }

    return k;
}


float MmwDemo_computePhaseUnwrap(float phase, float phasePrev, float *diffPhaseCorrectionCum)
{
    float modFactorF;
    float diffPhase;
    float diffPhaseMod;
    float diffPhaseCorrection;
    float phaseOut;

    // incremental phase variation
    diffPhase = phase - phasePrev;

    if (diffPhase > PI)
        modFactorF = 1;
    else if (diffPhase < -PI)
        modFactorF = -1;
    else
        modFactorF = 0;

    diffPhaseMod = diffPhase - modFactorF * 2 * PI;

    // preserve variation sign for +pi vs. -pi
    if ((diffPhaseMod == -PI) && (diffPhase > 0))
        diffPhaseMod = PI;

    // incremental phase correction
    diffPhaseCorrection = diffPhaseMod - diffPhase;

    // Ignore correction when incremental variation is smaller than cutoff
    if (((diffPhaseCorrection < PI) && (diffPhaseCorrection > 0)) ||
        ((diffPhaseCorrection > -PI) && (diffPhaseCorrection < 0)))
        diffPhaseCorrection = 0;

    // Find cumulative sum of deltas
    *diffPhaseCorrectionCum = *diffPhaseCorrectionCum + diffPhaseCorrection;
    phaseOut                = phase + *diffPhaseCorrectionCum;
    return phaseOut;
}


uint32_t MmwDemo_runVitalSigns(uint32_t vsBaseAddr, uint16_t indicateNoTarget, uint32_t vsLoop, vsAntennaGeometry vitalSignsAntenna)
{
    uint32_t rangeBinIdx;
    uint32_t dataIdx, antennaIdx;
    cplxf_t *dataInInter;
    uint32_t TwidStatus;

    dataIdx = 0;
    //antennaIdx = VIRTUAL_ANTENNA_DATA_OFFSET;
    antennaIdx = vitalSignsAntenna.numRangeBins*4;

    if (vsDataCount < VS_TOTAL_FRAME)
    {
        for (rangeBinIdx = 0; rangeBinIdx < 5; rangeBinIdx++)
        {
			/*
            vsDataPerFrame[6 * rangeBinIdx].imag     = -(float)*(volatile int16_t *)(vsBaseAddr + antennaIdx + dataIdx);
            vsDataPerFrame[6 * rangeBinIdx].real     = -(float)*(volatile int16_t *)(vsBaseAddr + antennaIdx + 2 + dataIdx);
            vsDataPerFrame[6 * rangeBinIdx + 1].imag = (float)*(volatile int16_t *)(vsBaseAddr + (antennaIdx * 4) + dataIdx);
            vsDataPerFrame[6 * rangeBinIdx + 1].real = (float)*(volatile int16_t *)(vsBaseAddr + (antennaIdx * 4) + 2 + dataIdx);
            vsDataPerFrame[6 * rangeBinIdx + 2].imag = (float)*(volatile int16_t *)(vsBaseAddr + dataIdx);
            vsDataPerFrame[6 * rangeBinIdx + 2].real = (float)*(volatile int16_t *)(vsBaseAddr + 2 + dataIdx);
            vsDataPerFrame[6 * rangeBinIdx + 3].imag = -(float)*(volatile int16_t *)(vsBaseAddr + (antennaIdx * 3) + dataIdx);
            vsDataPerFrame[6 * rangeBinIdx + 3].real = -(float)*(volatile int16_t *)(vsBaseAddr + (antennaIdx * 3) + 2 + dataIdx);
            vsDataPerFrame[6 * rangeBinIdx + 4].imag = (float)*(volatile int16_t *)(vsBaseAddr + (antennaIdx * 2) + dataIdx);
            vsDataPerFrame[6 * rangeBinIdx + 4].real = (float)*(volatile int16_t *)(vsBaseAddr + (antennaIdx * 2) + 2 + dataIdx);
            vsDataPerFrame[6 * rangeBinIdx + 5].imag = -(float)*(volatile int16_t *)(vsBaseAddr + (antennaIdx * 5) + dataIdx);
            vsDataPerFrame[6 * rangeBinIdx + 5].real = -(float)*(volatile int16_t *)(vsBaseAddr + (antennaIdx * 5) + 2 + dataIdx);
			*/
			
			/*
            vsDataPerFrame[6 * rangeBinIdx].imag      = (float)*(volatile int16_t *)(vsBaseAddr + dataIdx);
            vsDataPerFrame[6 * rangeBinIdx].real      = (float)*(volatile int16_t *)(vsBaseAddr + 2 + dataIdx);
            vsDataPerFrame[6 * rangeBinIdx + 1].imag  = -(float)*(volatile int16_t *)(vsBaseAddr + (antennaIdx * 3) + dataIdx);
            vsDataPerFrame[6 * rangeBinIdx + 1].real  = -(float)*(volatile int16_t *)(vsBaseAddr + (antennaIdx * 3) + 2 + dataIdx);
            vsDataPerFrame[6 * rangeBinIdx + 2].imag  = (float)*(volatile int16_t *)(vsBaseAddr + (antennaIdx * 2) + dataIdx);
            vsDataPerFrame[6 * rangeBinIdx + 2].real  = (float)*(volatile int16_t *)(vsBaseAddr + (antennaIdx * 2) + 2 + dataIdx);
            vsDataPerFrame[6 * rangeBinIdx + 3].imag  = -(float)*(volatile int16_t *)(vsBaseAddr + (antennaIdx * 5) + dataIdx);
            vsDataPerFrame[6 * rangeBinIdx + 3].real  = -(float)*(volatile int16_t *)(vsBaseAddr + (antennaIdx * 5) + 2 + dataIdx);
            vsDataPerFrame[6 * rangeBinIdx + 4].imag  = -(float)*(volatile int16_t *)(vsBaseAddr + antennaIdx + dataIdx);
            vsDataPerFrame[6 * rangeBinIdx + 4].real  = -(float)*(volatile int16_t *)(vsBaseAddr + antennaIdx + 2 + dataIdx);
            vsDataPerFrame[6 * rangeBinIdx + 5].imag  = (float)*(volatile int16_t *)(vsBaseAddr + (antennaIdx * 4) + dataIdx);
            vsDataPerFrame[6 * rangeBinIdx + 5].real  = (float)*(volatile int16_t *)(vsBaseAddr + (antennaIdx * 4) + 2 + dataIdx);
			*/
            vsDataPerFrame[6 * rangeBinIdx].imag      = (float)*(volatile int16_t *)(vsBaseAddr + dataIdx);
            vsDataPerFrame[6 * rangeBinIdx].real      = (float)*(volatile int16_t *)(vsBaseAddr + 2 + dataIdx);
            vsDataPerFrame[6 * rangeBinIdx + 1].imag  = -(float)*(volatile int16_t *)(vsBaseAddr + antennaIdx  + dataIdx);
            vsDataPerFrame[6 * rangeBinIdx + 1].real  = -(float)*(volatile int16_t *)(vsBaseAddr + antennaIdx  + 2 + dataIdx);
            vsDataPerFrame[6 * rangeBinIdx + 2].imag  = (float)*(volatile int16_t *)(vsBaseAddr + (antennaIdx * 2) + dataIdx);
            vsDataPerFrame[6 * rangeBinIdx + 2].real  = (float)*(volatile int16_t *)(vsBaseAddr + (antennaIdx * 2) + 2 + dataIdx);
            vsDataPerFrame[6 * rangeBinIdx + 3].imag  = -(float)*(volatile int16_t *)(vsBaseAddr + (antennaIdx * 3) + dataIdx);
            vsDataPerFrame[6 * rangeBinIdx + 3].real  = -(float)*(volatile int16_t *)(vsBaseAddr + (antennaIdx * 3) + 2 + dataIdx);
            vsDataPerFrame[6 * rangeBinIdx + 4].imag  = (float)*(volatile int16_t *)(vsBaseAddr + (antennaIdx * 4)+ dataIdx);
            vsDataPerFrame[6 * rangeBinIdx + 4].real  = (float)*(volatile int16_t *)(vsBaseAddr + (antennaIdx * 4)+ 2 + dataIdx);
            vsDataPerFrame[6 * rangeBinIdx + 5].imag  = -(float)*(volatile int16_t *)(vsBaseAddr + (antennaIdx * 5) + dataIdx);
            vsDataPerFrame[6 * rangeBinIdx + 5].real  = -(float)*(volatile int16_t *)(vsBaseAddr + (antennaIdx * 5) + 2 + dataIdx);
			
            dataIdx                              = dataIdx + 4;
        }

        dataInInter = vsDataPerFrame;

        if (vsLoop == 0 && vsDataCount == 0)
        {
            TwidStatus = MmwDemo_genTwiddle(angleFFTSpectrumTwiddle, VS_NUM_ANGLE_FFT);
            TwidStatus = MmwDemo_genTwiddle(vitalSignsSpectrumTwiddle, PHASE_FFT_SIZE);
        }

        MmwDemo_runPreProcess((cplxf_t *)dataInInter, vsDataCount);

        vsDataCount++;
    }


    if (vsDataCount == 128)
    {
        vsDataCount = 0;
    }

    if ((vsDataCount % REFRESH_RATE) == 0)
    {
        MmwDemo_computeVitalSignProcessing((cplxf_t *)vsDataAngleFftOutBuf, indicateNoTarget);
        vsLoop++;
    }


    return vsLoop;
}
