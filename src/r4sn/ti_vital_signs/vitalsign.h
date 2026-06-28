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

// Vital Signs
#include "cplx_types.h"
#include <common/sys_types.h>


/**
 * @brief
 *  radarProcess DPU Handle
 *
 */
/**********vs_add**************/
#define REFRESH_RATE 32
#define PI           3.1415926535897

#define VS_TOTAL_FRAME 128
#define VIRTUAL_ANTENNA_DATA_OFFSET 512

#define VS_FFT_SIZE 512

#define VS_NUM_ANGLE_FFT       16
#define VS_NUM_ANGLE_SEL_BIN   9
#define VS_NUM_RANGE_SEL_BIN   5
#define VS_NUM_VIRTUAL_CHANNEL 6

#define PHASE_FFT_SIZE (512) // FFT size for each of the Breathing and Cardiac waveform

#define HEART_INDEX_START          68
#define HEART_INDEX_END            128
#define HEART_RATE_DECISION_THRESH 3
#define BREATH_INDEX_START         3
#define BREATH_INDEX_END           50
#define SPECTRUM_MULTIPLICATION_FACTOR 0.882
#define HEART_RATE_JUMP_LIMIT 12
#define VITALS_MASK_LOOP_NO 7

typedef struct
{
    uint16_t id;
    uint16_t rangebin;
    float    breathingDeviation;
    float    heartRate;
    float    breathingRate;
    float    VitalSignsHeartCircularBuffer[15];
    float    VitalSignsBreathCircularBuffer[15];
} vsFeature;

typedef struct VsDemo_antennaGeometryAnt_t
{
    /*! @brief  row index in steps of lambda/2 */
    int8_t row;

    /*! @brief  row index in steps of lambda/2 */
    int8_t col;

} VsDemo_antennaGeometryAnt;


typedef struct
{
    VsDemo_antennaGeometryAnt  vsActiveAntennaGeometryCfg[6];
    uint16_t numAntRow;
    uint16_t numAntCol;
    uint16_t numTxAntennas;
    uint16_t numRxAntennas;
    uint32_t numRangeBins;
} vsAntennaGeometry;


#define MMWDEMO_OUTPUT_MSG_VS 0x410

//Input structrure where data from each frame is input
cplxf_t vsDataPerFrame[VS_NUM_RANGE_SEL_BIN * VS_NUM_VIRTUAL_CHANNEL];
float   angleFFTSpectrumTwiddle[2 * VS_NUM_ANGLE_FFT];
float   vitalSignsSpectrumTwiddle[2 * PHASE_FFT_SIZE];

//Contains the azimuth fft X Elevation fft data
cplxf_t vsDataAngleFftOutBuf[VS_TOTAL_FRAME * VS_NUM_RANGE_SEL_BIN * VS_NUM_ANGLE_SEL_BIN];


//Unwrap phase after arctan computation
float    MmwDemo_computePhaseUnwrap(float phase, float phasePrev, float *diffPhaseCorrectionCum);

//Compute arctan and unwrap and spectrum FFT and estimage breath and heart rate
void     MmwDemo_computeVitalSignProcessing(cplxf_t *vsDataAngleFftOutBuf, uint16_t indicateNoTarget);

//Pre proseccisng function called after every frame to pick data for the selected range bins and angle bins
void     MmwDemo_runPreProcess(cplxf_t *pDataIn, uint32_t vsDataCount);

//Generate twiddle factors for angle fft and spectrum fft
int      MmwDemo_genTwiddle(float *w, int n);

//Top function called in vitalsign_with_tracking. This function calls all other related VS functions
uint32_t MmwDemo_runVitalSigns(uint32_t vsBaseAddr, uint16_t indicateNoTarget, uint32_t vsLoop, vsAntennaGeometry vitalSignsAntenna);

//Compute Magnituded squared for the radar cross section 
void     MmwDemo_computeMagnitudeSquared(cplxf_t *inpBuff, float *magSqrdBuff, uint32_t numSamples);

//Compute devation of breath rate to determine if the patient exist or not
float    MmwDemo_computeMyDeviation(float *a, int n);

//Perfrom transpose before evelation fft
uint32_t MmwDemo_runCopyTranspose64b(uint64_t *src, uint64_t *dest, uint32_t size, int32_t offset, uint32_t stride, uint32_t pairs);
