/*
 * Copyright (C) 2022-24 Texas Instruments Incorporated
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 *   Redistributions of source code must retain the above copyright
 *   notice, this list of conditions and the following disclaimer.
 *
 *   Redistributions in binary form must reproduce the above copyright
 *   notice, this list of conditions and the following disclaimer in the
 *   documentation and/or other materials provided with the
 *   distribution.
 *
 *   Neither the name of Texas Instruments Incorporated nor the names of
 *   its contributors may be used to endorse or promote products derived
 *   from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/**************************************************************************
 *************************** Include Files ********************************
 **************************************************************************/

/* Standard Include Files. */
#include <stdint.h>
#include <stdlib.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <assert.h>
/* MCU Plus Include Files. */
#include <kernel/dpl/SemaphoreP.h>
#include <kernel/dpl/CacheP.h>
#include <kernel/dpl/ClockP.h>
#include <kernel/dpl/DebugP.h>
#include <kernel/dpl/HwiP.h>
#include <kernel/dpl/AddrTranslateP.h>
#include "FreeRTOS.h"
#include "task.h"
/* mmwave SDK files */
#include <control/mmwave/mmwave.h>
#include "source/mmw_cli.h"
#include "ti_drivers_config.h"
#include "ti_drivers_open_close.h"
#include "ti_board_open_close.h"
#include "ti_board_config.h"
#include <FreeRTOS.h>
#include <task.h>
#include <semphr.h>
#include "drivers/power.h"
#include <drivers/prcm.h>
#include <utils/mathutils/mathutils.h>

#include "source/motion_detect.h"
#include "source/mmw_res.h"
#include "source/dpc/dpc.h"
#include "source/mmwave_control/interrupts.h"
#include "source/calibrations/range_phase_bias_measurement.h"
#include "source/utils/mmw_demo_utils.h"

#include "vitalsign.h"
#include "cplx_types.h"

#define MAXSPISIZEFTDI (65536U)
extern uint32_t gpioBaseAddrLed, pinNumLed;

#define HWA_MAX_NUM_DMA_TRIG_CHANNELS 16

#define MMW_DEMO_MAJOR_MODE 0
#define MMW_DEMO_MINOR_MODE 1

#define LOW_PWR_MODE_DISABLE (0)
#define LOW_PWR_MODE_ENABLE (1)
#define LOW_PWR_TEST_MODE (2)

#define MAX_NUM_DETECTIONS          (MMWDEMO_OUTPUT_POINT_CLOUD_LIST_MAX_SIZE)

#define FRAME_REF_TIMER_CLOCK_MHZ  40

#define MMWDEMO_RFPARSER_SPEED_OF_LIGHT_IN_METERS_PER_SEC (3e8)

#define DPC_DPU_DPIF_DETMATRIX_FORMAT_2 2

#define DPC_OBJDET_HWA_WINDOW_RAM_OFFSET 0
#define DPC_DPU_RANGEPROC_FFT_WINDOW_TYPE MATHUTILS_WIN_BLACKMAN
#define DPC_OBJDET_QFORMAT_RANGE_FFT 17
#define MMW_DEMO_TEST_ADC_BUFF_SIZE 1024  //maximum 128 real samples (int16_t), 3 Rx channels

extern MmwDemo_MSS_MCB gMmwMssMCB; 
extern MMWave_temperatureStats  tempStats;
extern uint8_t pgVersion;
extern float gTestMinMpdCentroid;

extern void mmwDemo_ProfileSwitchStateMachine();

extern uint8_t gMmwL3[L3_MEM_SIZE]  __attribute((section(".l3")));
/*! Local RAM buffer for object detection DPC */
extern uint8_t gMmwCoreLocMem[MMWDEMO_OBJDET_CORE_LOCAL_MEM_SIZE];
/*! Local RAM buffer for tracker */
extern uint8_t gMmwCoreLocMem2[MMWDEMO_OBJDET_CORE_LOCAL_MEM2_SIZE];

/* User defined heap memory and handle */
uint8_t gMmwCoreLocMem3[MMWDEMO_OBJDET_CORE_LOCAL_MEM3_SIZE] __attribute__((aligned(HeapP_BYTE_ALIGNMENT)));

void mmwDemo_dpcTask();

volatile unsigned long long test;
HWA_Handle hwaHandle;
DPU_DoaProc_HW_Resources  *hwRes;

DPU_CFARProcHWA_Config cfarProcDpuCfg;
DPU_DoaProc_Config doaProcDpuCfg;
DPU_AoasvcProc_Config aoasvcProcDpuCfg;
DPU_RangeProcHWA_Config rangeProcDpuCfg;
DPU_MpdProc_Config mpdProcDpuCfg;
DPU_uDopProc_Config uDopProcDpuCfg;


/*! @brief     EDMA interrupt objects for DPUs */
Edma_IntrObject intrObj_cfarProc;
Edma_IntrObject intrObj_doaProc;
Edma_IntrObject     intrObj_rangeProc[2];
Edma_IntrObject  intrObj_uDopProc;

uint32_t vsDataCount = 0;
uint32_t vsBaseAddr  = 0;
uint32_t vsLoop      = 0;

extern vsFeature vitalSignsOutput;
float             radialDistance;
float             xDistForVS;
float             yDistForVS;
uint16_t          vsRangeBin      = 0;
uint16_t          indicateNoTarget = 0;
extern vsAntennaGeometry vitalSignsAntenna;
/**
 *  @b Description
 *  @n
 *      The function allocates HWA DMA source channel from the pool
 *
 *  @param[in]  pool Handle to pool object.
 *
 *  @retval
 *      channel Allocated HWA trigger source channel
 */
uint8_t DPC_ObjDet_HwaDmaTrigSrcChanPoolAlloc(HwaDmaTrigChanPoolObj *pool)
{
    uint8_t channel = 0xFF;
    if(pool->dmaTrigSrcNextChan < HWA_MAX_NUM_DMA_TRIG_CHANNELS)
    {
        channel = pool->dmaTrigSrcNextChan;
        pool->dmaTrigSrcNextChan++;
    }
    return channel;
}

/**
 *  @b Description
 *  @n
 *      The function resets HWA DMA source channel pool
 *
 *  @param[in]  pool Handle to pool object.
 *
 *  @retval
 *      none
 */
void DPC_ObjDet_HwaDmaTrigSrcChanPoolReset(HwaDmaTrigChanPoolObj *pool)
{
    pool->dmaTrigSrcNextChan = 0;
}

/**
 *  @b Description
 *  @n
 *      Utility function for reseting memory pool.
 *
 *  @param[in]  pool Handle to pool object.
 *
 *  \ingroup DPC_OBJDET__INTERNAL_FUNCTION
 *
 *  @retval
 *      none.
 */
void DPC_ObjDet_MemPoolReset(MemPoolObj *pool)
{
    pool->currAddr = (uintptr_t)pool->cfg.addr;
    pool->maxCurrAddr = pool->currAddr;
}


/**
 *  @b Description
 *  @n
 *      Utility function for setting memory pool to desired address in the pool.
 *      Helps to rewind for example.
 *
 *  @param[in]  pool Handle to pool object.
 *  @param[in]  addr Address to assign to the pool's current address.
 *
 *  \ingroup DPC_OBJDET__INTERNAL_FUNCTION
 *
 *  @retval
 *      None
 */
static void DPC_ObjDet_MemPoolSet(MemPoolObj *pool, void *addr)
{
    pool->currAddr = (uintptr_t)addr;
    pool->maxCurrAddr = MAX(pool->currAddr, pool->maxCurrAddr);
}

/**
 *  @b Description
 *  @n
 *      Utility function for getting memory pool current address.
 *
 *  @param[in]  pool Handle to pool object.
 *
 *  \ingroup DPC_OBJDET__INTERNAL_FUNCTION
 *
 *  @retval
 *      pointer to current address of the pool (from which next allocation will
 *      allocate to the desired alignment).
 */
static void *DPC_ObjDet_MemPoolGet(MemPoolObj *pool)
{
    return((void *)pool->currAddr);
}

/**
 *  @b Description
 *  @n
 *      Utility function for getting maximum memory pool usage.
 *
 *  @param[in]  pool Handle to pool object.
 *
 *  \ingroup DPC_OBJDET__INTERNAL_FUNCTION
 *
 *  @retval
 *      Amount of pool used in bytes.
 */
uint32_t DPC_ObjDet_MemPoolGetMaxUsage(MemPoolObj *pool)
{
    return((uint32_t)(pool->maxCurrAddr - (uintptr_t)pool->cfg.addr));
}

/**
 *  @b Description
 *  @n
 *      Utility function for allocating from a static memory pool.
 *
 *  @param[in]  pool Handle to pool object.
 *  @param[in]  size Size in bytes to be allocated.
 *  @param[in]  align Alignment in bytes
 *
 *  \ingroup DPC_OBJDET__INTERNAL_FUNCTION
 *
 *  @retval
 *      pointer to beginning of allocated block. NULL indicates could not
 *      allocate.
 */
void *DPC_ObjDet_MemPoolAlloc(MemPoolObj *pool,
                              uint32_t size,
                              uint8_t align)
{
    void *retAddr = NULL;
    uintptr_t addr;

    addr = MEM_ALIGN(pool->currAddr, align);
    if ((addr + size) <= ((uintptr_t)pool->cfg.addr + pool->cfg.size))
    {
        retAddr = (void *)addr;
        pool->currAddr = addr + size;
        pool->maxCurrAddr = MAX(pool->currAddr, pool->maxCurrAddr);
    }

    return(retAddr);
}

/**
 *  @b Description
 *  @n
 *      Utility function to do a parabolic/quadratic fit on 3 input points
 *      and return the coordinates of the peak. This is used to accurately estimate
 *      range bias.
 *
 *  @param[in]  x Pointer to array of 3 elements representing the x-coordinate
 *              of the points to fit
 *  @param[in]  y Pointer to array of 3 elements representing the y-coordinate
 *              of the points to fit
 *  @param[out] xv Pointer to output x-coordinate of the peak value
 *  @param[out] yv Pointer to output y-coordinate of the peak value
 *
 *  @retval   None
 *
 */
void rangeBiasRxChPhaseMeasure_quadfit(float *x, float*y, float *xv, float *yv)
{
    float a, b, c, denom;
    float x0 = x[0];
    float x1 = x[1];
    float x2 = x[2];
    float y0 = y[0];
    float y1 = y[1];
    float y2 = y[2];

    denom = (x0 - x1)*(x0 - x2)*(x1 - x2);
    if (denom != 0.)
    {
        a = (x2 * (y1 - y0) + x1 * (y0 - y2) + x0 * (y2 - y1)) / denom;
        b = (x2*x2 * (y0 - y1) + x1*x1 * (y2 - y0) + x0*x0 * (y1 - y2)) / denom;
        c = (x1 * x2 * (x1 - x2) * y0 + x2 * x0 * (x2 - x0) * y1 + x0 * x1 * (x0 - x1) * y2) / denom;
    }
    else
    {
        *xv = x[1];
        *yv = y[1];
        return;
    }
    if (a != 0.)
    {
        *xv = -b/(2*a);
        *yv = c - b*b/(4*a);
    }
    else
    {
        *xv = x[1];
        *yv = y[1];
    }
}

/**
*  @b Description
*  @n
*    Function to construct feature extract heap
*/
void featExtract_heapConstruct()
{
    HeapP_construct(&gMmwMssMCB.CoreLocalFeatExtractHeapObj, (void *) gMmwCoreLocMem3, MMWDEMO_OBJDET_CORE_LOCAL_MEM3_SIZE);
}

/**
*  @b Description
*  @n
*    Function to allocate memory for feature extract heap
*/
void *featExtract_malloc(uint32_t sizeInBytes)
{
    return HeapP_alloc(&gMmwMssMCB.CoreLocalFeatExtractHeapObj, sizeInBytes);
}

/**
*  @b Description
*  @n
*    Function to free memory from feature extract heap
*/
void featExtract_free(void *pFree, uint32_t sizeInBytes)
{
    HeapP_free(&gMmwMssMCB.CoreLocalFeatExtractHeapObj, pFree);
}

/**
*  @b Description
*  @n
*    Function to get memory usage stats of feature extract heap object
*/
uint32_t featExtract_memUsage()
{
    uint32_t usedMemSizeInBytes;
    HeapP_MemStats heapStats;

    HeapP_getHeapStats(&gMmwMssMCB.CoreLocalFeatExtractHeapObj, &heapStats);
    usedMemSizeInBytes = sizeof(gMmwCoreLocMem3) - heapStats.availableHeapSpaceInBytes;

    return usedMemSizeInBytes;
}

/**
*  @b Description
*  @n
*    Select coordinates of active virtual antennas and calculate the size of the 2D virtual antenna pattern,
*    i.e. number of antenna rows and number of antenna columns.
*/
void MmwDemo_calcActiveAntennaGeometry()
{
    int32_t txInd, rxInd, ind;
    int32_t rowMax, colMax;
    int32_t rowMin, colMin;
    /* Select only active antennas */
    ind = 0;
    for (txInd = 0; txInd < gMmwMssMCB.numTxAntennas; txInd++)
    {
        for (rxInd = 0; rxInd < gMmwMssMCB.numRxAntennas; rxInd++)
        {
            gMmwMssMCB.activeAntennaGeometryCfg.ant[ind] = gMmwMssMCB.antennaGeometryCfg.ant[gMmwMssMCB.rxAntOrder[rxInd] + (txInd * SYS_COMMON_NUM_RX_CHANNEL)];
            ind++;
        }
    }

    /* Calculate virtual antenna 2D array size */
    ind = 0;
    rowMax = 0;
    colMax = 0;
    rowMin = 127;
    colMin = 127;
    for (txInd = 0; txInd < gMmwMssMCB.numTxAntennas; txInd++)
    {
        for (rxInd = 0; rxInd < gMmwMssMCB.numRxAntennas; rxInd++)
        {
            if (gMmwMssMCB.activeAntennaGeometryCfg.ant[ind].row > rowMax)
            {
                rowMax = gMmwMssMCB.activeAntennaGeometryCfg.ant[ind].row;
            }
            if (gMmwMssMCB.activeAntennaGeometryCfg.ant[ind].col > colMax)
            {
                colMax = gMmwMssMCB.activeAntennaGeometryCfg.ant[ind].col;
            }
            if (gMmwMssMCB.activeAntennaGeometryCfg.ant[ind].row < rowMin)
            {
                rowMin = gMmwMssMCB.activeAntennaGeometryCfg.ant[ind].row;
            }
            if (gMmwMssMCB.activeAntennaGeometryCfg.ant[ind].col < colMin)
            {
                colMin = gMmwMssMCB.activeAntennaGeometryCfg.ant[ind].col;
            }
            ind++;
        }
    }
    ind = 0;
    for (txInd = 0; txInd < gMmwMssMCB.numTxAntennas; txInd++)
    {
        for (rxInd = 0; rxInd < gMmwMssMCB.numRxAntennas; rxInd++)
        {
            gMmwMssMCB.activeAntennaGeometryCfg.ant[ind].row -= rowMin;
            gMmwMssMCB.activeAntennaGeometryCfg.ant[ind].col -= colMin;
            ind++;
        }
    }
    gMmwMssMCB.numAntRow = rowMax - rowMin + 1;
    gMmwMssMCB.numAntCol = colMax - colMin + 1;
}


/**
*  @b Description
*  @n
*    Based on the activeAntennaGeometryCfg configures the table which used to configure
*    Doppler FFT HWA param sets in DoA DPU. THese param sets perform Doppler FFT and
*    at the same time mapping of input antennas into 2D row-column antenna array where columns
*    are in  azimuth dimension, and rows in elevation dimension.
*    It also calculates the size of 2D antenna array, ie. number of rows and number of columns.
*/
int32_t MmwDemo_cfgDopplerParamMapping(DPU_DoaProc_HWA_Option_Cfg *dopplerParamCfg, uint32_t mappingOption)
{
    int32_t ind, indNext, indNextPrev;
    int32_t row, col;
    int32_t dopParamInd;
    int32_t state;
    int16_t BT[DPU_DOA_PROC_MAX_2D_ANT_ARRAY_ELEMENTS];
    int16_t DT[DPU_DOA_PROC_MAX_2D_ANT_ARRAY_ELEMENTS];
    int16_t SCAL[DPU_DOA_PROC_MAX_2D_ANT_ARRAY_ELEMENTS];
    int8_t  DONE[DPU_DOA_PROC_MAX_2D_ANT_ARRAY_ELEMENTS];
    int32_t retVal = 0;
    int32_t rowOffset;

    if (gMmwMssMCB.numAntRow * gMmwMssMCB.numAntCol > DPU_DOA_PROC_MAX_2D_ANT_ARRAY_ELEMENTS)
    {
        retVal = DPC_OBJECTDETECTION_EANTENNA_GEOMETRY_CFG_FAILED;
        goto exit;
    }

    if (mappingOption == DOPPLER_OUTPUT_MAPPING_DOP_ROW_COL)
    {
        /*For AOA DPU, Output is */
        rowOffset =  gMmwMssMCB.numAntCol;
    }
    else if (mappingOption == DOPPLER_OUTPUT_MAPPING_ROW_DOP_COL)
    {
        rowOffset =  gMmwMssMCB.numDopplerBins * gMmwMssMCB.numAntCol;
    }
    else
    {
        retVal = DPC_OBJECTDETECTION_EANTENNA_GEOMETRY_CFG_FAILED;
        goto exit;
    }

    /* Initialize tables */
    for (ind = 0; ind < (gMmwMssMCB.numAntRow * gMmwMssMCB.numAntCol); ind++)
    {
        BT[ind] = 0;
        SCAL[ind] = 0;
        DONE[ind] = 0;
    }

    for (ind = 0; ind < (gMmwMssMCB.numTxAntennas * gMmwMssMCB.numRxAntennas); ind++)
    {
        row = gMmwMssMCB.activeAntennaGeometryCfg.ant[ind].row;
        col = gMmwMssMCB.activeAntennaGeometryCfg.ant[ind].col;
        BT[row * gMmwMssMCB.numAntCol + col] = ind;
        SCAL[row * gMmwMssMCB.numAntCol + col] = 1;
    }
    for (row = 0; row < gMmwMssMCB.numAntRow; row++)
    {
        for (col = 0; col < gMmwMssMCB.numAntCol; col++)
        {
            ind = row * gMmwMssMCB.numAntCol + col;
            DT[ind] = row * rowOffset + col;
        }
    }


    /* Configure Doppler HWA mapping params for antenna mapping */
    dopParamInd = 0;
    dopplerParamCfg->numDopFftParams = 0;
    for (ind = 0; ind < (gMmwMssMCB.numAntRow * gMmwMssMCB.numAntCol); ind++)
    {
        if (!DONE[ind])
        {
            if(dopParamInd < DPU_DOA_PROC_MAX_NUM_DOP_FFFT_PARAMS)
            {
                DONE[ind] = 1;
                dopplerParamCfg->numDopFftParams++;
                dopplerParamCfg->dopFftCfg[dopParamInd].srcBcnt = 1;
                dopplerParamCfg->dopFftCfg[dopParamInd].scale = SCAL[ind];
                if (dopplerParamCfg->dopFftCfg[dopParamInd].scale == 0)
                {
                    dopplerParamCfg->dopFftCfg[dopParamInd].srcAddrOffset = 0;
                }
                else
                {
                    dopplerParamCfg->dopFftCfg[dopParamInd].srcAddrOffset = BT[ind];
                }
                dopplerParamCfg->dopFftCfg[dopParamInd].dstAddrOffset = DT[ind];
                state = 1;//STATE_SECOND:
                for (indNext = ind+1; indNext < (gMmwMssMCB.numAntRow * gMmwMssMCB.numAntCol); indNext++)
                {

                    if (!DONE[indNext] && (dopplerParamCfg->dopFftCfg[dopParamInd].scale == SCAL[indNext]))
                    {
                        switch (state)
                        {
                            case 1://STATE_SECOND:
                                dopplerParamCfg->dopFftCfg[dopParamInd].srcBcnt++;
                                DONE[indNext] = 1;
                                if (SCAL[indNext] == 1)
                                {
                                    dopplerParamCfg->dopFftCfg[dopParamInd].srcBidx = BT[indNext] - dopplerParamCfg->dopFftCfg[dopParamInd].srcAddrOffset;
                                }
                                else
                                {
                                    dopplerParamCfg->dopFftCfg[dopParamInd].srcBidx = 0;
                                }
                                dopplerParamCfg->dopFftCfg[dopParamInd].dstBidx = DT[indNext] - DT[ind];
                                indNextPrev = indNext;
                                state = 2;//STATE_NEXT:
                                break;
                            case 2://STATE_NEXT:
                                if (SCAL[indNext] == 1)
                                {
                                    if ((dopplerParamCfg->dopFftCfg[dopParamInd].srcBidx == (BT[indNext] - BT[indNextPrev])) &&
                                        (dopplerParamCfg->dopFftCfg[dopParamInd].dstBidx == (DT[indNext] - DT[indNextPrev])))
                                    {
                                        DONE[indNext] = 1;
                                        dopplerParamCfg->dopFftCfg[dopParamInd].srcBcnt++;
                                        indNextPrev = indNext;
                                    }
                                }
                                else
                                {
                                    if (dopplerParamCfg->dopFftCfg[dopParamInd].dstBidx == (DT[indNext] - DT[indNextPrev]))
                                    {
                                        DONE[indNext] = 1;
                                        dopplerParamCfg->dopFftCfg[dopParamInd].srcBcnt++;
                                        indNextPrev = indNext;
                                    }
                                }
                                break;
                        }
                    }
                }
                dopParamInd++;
            }
            else
            {
                retVal = DPC_OBJECTDETECTION_EANTENNA_GEOMETRY_CFG_FAILED;
                goto exit;
            }
        }
    }

    dopplerParamCfg->numDopFftParams = dopParamInd;

exit:
    return retVal;
}

/**
 *  @b Description
 *  @n
 *     Compress point cloud list which is transferred to the Host via UART.
 *     Floating point values are converted to int16
 *
 * @param[out] pointCloudOut        Compressed point cloud list
 * @param[in]  pointCloudUintRecip  Scales used for conversion from float values to integer value
 * @param[in]  pointCloudIn         Input point cloud list, generated by CFAR DPU
 * @param[in]  numPoints            Number of points in the point cloud list
 *
 *  @retval
 *      Not Applicable.
 */
void MmwDemo_compressPointCloudList(MmwDemo_output_message_UARTpointCloud *pointCloudOut,
                                    MmwDemo_output_message_point_unit *pointCloudUintRecip,
                                    DPIF_PointCloudCartesianExt *pointCloudIn,
                                    uint32_t numPoints)
{
    uint32_t i;
    float xyzUnitScale = pointCloudUintRecip->xyzUnit;
    float dopplerScale = pointCloudUintRecip->dopplerUnit;
    float snrScale = pointCloudUintRecip->snrUint;
    float noiseScale = pointCloudUintRecip->noiseUint;
    uint32_t tempVal;

    for (i = 0; i < numPoints; i++)
    {
        pointCloudOut->point[i].x = (int16_t) roundf(pointCloudIn[i].x * xyzUnitScale);
        pointCloudOut->point[i].y = (int16_t) roundf(pointCloudIn[i].y * xyzUnitScale);
        pointCloudOut->point[i].z = (int16_t) roundf(pointCloudIn[i].z * xyzUnitScale);
        pointCloudOut->point[i].doppler = (int16_t) roundf(pointCloudIn[i].velocity * dopplerScale);
        tempVal = (uint32_t) roundf(pointCloudIn[i].snr * snrScale);
        if (tempVal > 255)
        {
            tempVal = 255;
        }
        pointCloudOut->point[i].snr = (uint8_t) tempVal;
        tempVal = (uint32_t) roundf(pointCloudIn[i].noise * noiseScale);
        if (tempVal > 255)
        {
            tempVal = 255;
        }
        pointCloudOut->point[i].noise = (uint8_t) tempVal;
    }
}

/**
*  @b Description
*  @n
*    Range processing DPU Initialization
*/
void rangeProc_dpuInit()
{
    int32_t errorCode = 0;
    DPU_RangeProcHWA_InitParams initParams;
    initParams.hwaHandle = hwaHandle;

    /* generate the dpu handler*/
    gMmwMssMCB.rangeProcDpuHandle = DPU_RangeProcHWA_init(&initParams, &errorCode);
    if (gMmwMssMCB.rangeProcDpuHandle == NULL)
    {
        CLI_write("Error: RangeProc DPU initialization returned error %d\n", errorCode);
        DebugP_assert(0);
        return;
    }
}

/**
*  @b Description
*  @n
*    DOA DPU Initialization
*/
void doaProc_dpuInit()
{
    int32_t  errorCode = 0;
    DPU_DoaProc_InitParams initParams;
    initParams.hwaHandle =  hwaHandle;
    /* generate the dpu handler*/
    gMmwMssMCB.doaProcDpuHandle =  DPU_DoaProc_init(&initParams, &errorCode);
    if (gMmwMssMCB.doaProcDpuHandle == NULL)
    {
        CLI_write ("Error: DoaProc DPU initialization returned error %d\n", errorCode);
        DebugP_assert (0);
        return;
    }
}

/**
*  @b Description
*  @n
*    AOASVC DPU Initialization
*/
void aoasvcProc_dpuInit()
{
    int32_t  errorCode = 0;
    DPU_AoasvcProc_InitParams initParams;
    initParams.hwaHandle =  hwaHandle;
    /* generate the dpu handler*/
    gMmwMssMCB.aoasvcProcDpuHandle =  DPU_AoasvcProc_init(&initParams, &errorCode);
    if (gMmwMssMCB.aoasvcProcDpuHandle == NULL)
    {
        CLI_write ("Error: AoasvcProc DPU initialization returned error %d\n", errorCode);
        DebugP_assert (0);
        return;
    }
}

/**
*  @b Description
*  @n
*    CFAR DPU Initialization
*/
void cfarProc_dpuInit()
{
    int32_t  errorCode = 0;
    DPU_CFARProcHWA_InitParams initParams;
    initParams.hwaHandle =  hwaHandle;
    /* generate the dpu handler*/
    gMmwMssMCB.cfarProcDpuHandle =  DPU_CFARProcHWA_init(&initParams, &errorCode);
    if (gMmwMssMCB.cfarProcDpuHandle == NULL)
    {
        CLI_write ("Error: CFAR Proc DPU initialization returned error %d\n", errorCode);
        DebugP_assert (0);
        return;
    }
}

/**
*  @b Description
*  @n
*    MPD DPU Initialization
*/
void mpdProc_dpuInit()
{
    int32_t  errorCode = 0;

    if (!gMmwMssMCB.oneTimeConfigDone && gMmwMssMCB.profileSwitchCfg.switchCfgEnable != 1)
    {
        gMmwMssMCB.sceneryParams.numBoundaryBoxes = 0;
    }
    
    /* generate the dpu handler*/
    gMmwMssMCB.mpdProcDpuHandle =  DPU_MpdProc_init(&errorCode);
    if (gMmwMssMCB.mpdProcDpuHandle == NULL)
    {
        CLI_write("Error: MPD Proc DPU initialization returned error %d\n", errorCode);
        DebugP_assert (0);
        return;
    }
}

/**
*  @b Description
*  @n
*    Micro-Doppler DPU Initialization
*/
void udopProc_dpuInit()
{
    int32_t  errorCode = 0;
    DPU_uDopProc_InitParams initParams;
    initParams.hwaHandle =  hwaHandle;
    /* generate the dpu handler*/
    gMmwMssMCB.microDopDpuHandle =  DPU_uDopProc_init(&initParams, &errorCode);
    if (gMmwMssMCB.microDopDpuHandle == NULL)
    {
        CLI_write ("Error: Micro-Doppler Proc DPU initialization returned error %d\n", errorCode);
        DebugP_assert (0);
        return;
    }
}

/**
*  @b Description
*  @n
*    Processing chain initialization
*/
void trackerProc_dpuInit()
{
    int32_t  errorCode = 0;
    /* generate the dpu handler*/
    gMmwMssMCB.trackerProcDpuHandle =  DPU_TrackerProc_init(&errorCode);
    if (gMmwMssMCB.trackerProcDpuHandle == NULL)
    {
        CLI_write ("Error: Tracker Proc DPU initialization returned error %d\n", errorCode);
        DebugP_assert (0);
        return;
    }
}

/**
*  @b Description
*  @n
*    Based on the configuration, set up the range processing DPU configurations
*/
int32_t RangeProc_configParser()
{

    int32_t retVal = 0;
    DPU_RangeProcHWA_HW_Resources *pHwConfig = &rangeProcDpuCfg.hwRes;
    DPU_RangeProcHWA_StaticConfig  * params;
    uint32_t index;
    uint32_t bytesPerRxChan;

    /* Rangeproc DPU */
    pHwConfig = &rangeProcDpuCfg.hwRes;
    params = &rangeProcDpuCfg.staticCfg;

    memset((void *)&rangeProcDpuCfg, 0, sizeof(DPU_RangeProcHWA_Config));

    params->enableMajorMotion = gMmwMssMCB.enableMajorMotion;
    params->enableMinorMotion = gMmwMssMCB.enableMinorMotion;

    params->numFramesPerMinorMotProc = gMmwMssMCB.sigProcChainCfg.numFrmPerMinorMotProc;
    params->numMinorMotionChirpsPerFrame = gMmwMssMCB.sigProcChainCfg.numMinorMotionChirpsPerFrame;
    params->frmCntrModNumFramesPerMinorMot = gMmwMssMCB.frmCntrModNumFramesPerMinorMot;
    params->lowPowerMode = gMmwMssMCB.lowPowerMode;

    gMmwMssMCB.frmCntrModNumFramesPerMinorMot++;
    if(gMmwMssMCB.frmCntrModNumFramesPerMinorMot == gMmwMssMCB.sigProcChainCfg.numFrmPerMinorMotProc)
    {
        gMmwMssMCB.frmCntrModNumFramesPerMinorMot = 0;
    }

    /* hwi configuration */
    pHwConfig = &rangeProcDpuCfg.hwRes;

    /* HWA configurations, not related to per test, common to all test */
    pHwConfig->hwaCfg.paramSetStartIdx = 0;
    pHwConfig->hwaCfg.numParamSet = DPU_RANGEPROCHWA_NUM_HWA_PARAM_SETS;
    pHwConfig->hwaCfg.hwaWinRamOffset  = DPC_OBJDET_HWA_WINDOW_RAM_OFFSET;
    pHwConfig->hwaCfg.hwaWinSym = 1;
    pHwConfig->hwaCfg.dataInputMode = DPU_RangeProcHWA_InputMode_ISOLATED;
    pHwConfig->hwaCfg.dmaTrigSrcChan[0] = DPC_ObjDet_HwaDmaTrigSrcChanPoolAlloc(&gMmwMssMCB.HwaDmaChanPoolObj);
    pHwConfig->hwaCfg.dmaTrigSrcChan[1] = DPC_ObjDet_HwaDmaTrigSrcChanPoolAlloc(&gMmwMssMCB.HwaDmaChanPoolObj);


    /* edma configuration */
    pHwConfig->edmaHandle  = gEdmaHandle[0];
    /* edma configuration depends on the interleave or non-interleave */

    /* windowing buffer is fixed, size will change*/
    params->windowSize = sizeof(uint32_t) * ((gMmwMssMCB.profileComCfg.h_NumOfAdcSamples +1 ) / 2); //symmetric window, for real samples
    params->window =  (int32_t *)DPC_ObjDet_MemPoolAlloc(&gMmwMssMCB.CoreLocalRamObj,
                                                         params->windowSize,
                                                         sizeof(uint32_t));
    if (params->window == NULL)
    {
        retVal = DPC_OBJECTDETECTION_ENOMEM__CORE_LOCAL_RAM_RANGE_HWA_WINDOW;
        goto exit;
    }

    /* adc buffer buffer, format fixed, interleave, size will change */
    params->ADCBufData.dataProperty.dataFmt = DPIF_DATAFORMAT_REAL16;
    params->ADCBufData.dataProperty.adcBits = 2U; // 12-bit only
    params->ADCBufData.dataProperty.numChirpsPerChirpEvent = 1U;

    #if (CLI_REMOVAL == 0)
    if(gMmwMssMCB.adcDataSourceCfg.source == 0)
    {
        params->ADCBufData.data = (void *)CSL_APP_HWA_ADCBUF_RD_U_BASE;
    }
    else
    {
        gMmwMssMCB.adcTestBuff  = (uint8_t *) DPC_ObjDet_MemPoolAlloc(&gMmwMssMCB.L3RamObj,
                                                                            MMW_DEMO_TEST_ADC_BUFF_SIZE,
                                                                            sizeof(uint32_t));
        if(gMmwMssMCB.adcTestBuff == NULL)
        {
            retVal = DPC_OBJECTDETECTION_ENOMEM__L3_RAM_ADC_TEST_BUFF;
            goto exit;
        }
        params->ADCBufData.data = (void *)gMmwMssMCB.adcTestBuff;

    }
    #else
    params->ADCBufData.data = (void *)CSL_APP_HWA_ADCBUF_RD_U_BASE;
    #endif

    params->numTxAntennas = (uint8_t) gMmwMssMCB.numTxAntennas;
    params->numVirtualAntennas = (uint8_t) (gMmwMssMCB.numTxAntennas * gMmwMssMCB.numRxAntennas);
    params->numRangeBins = gMmwMssMCB.numRangeBins;
    params->numChirpsPerFrame = gMmwMssMCB.frameCfg.h_NumOfBurstsInFrame * gMmwMssMCB.frameCfg.h_NumOfChirpsInBurst;
    params->numDopplerChirpsPerFrame = params->numChirpsPerFrame/params->numTxAntennas;

    if ((params->numTxAntennas == 1) && (gMmwMssMCB.frameCfg.h_NumOfBurstsInFrame !=1))
    {
        retVal = DPC_OBJECTDETECTION_EINVAL_CFG;
        goto exit;
    }
    if ((params->numTxAntennas == 1) && (gMmwMssMCB.isBpmEnabled))
    {
        retVal = DPC_OBJECTDETECTION_EINVAL_CFG;
        goto exit;
    }

    if (params->enableMajorMotion)
    {
        params->numDopplerChirpsPerProc = params->numDopplerChirpsPerFrame;
    }
    else
    {
        params->numDopplerChirpsPerProc = params->numFramesPerMinorMotProc * params->numMinorMotionChirpsPerFrame;
    }

    params->isBpmEnabled = gMmwMssMCB.isBpmEnabled;
    /* windowing */
    params->ADCBufData.dataProperty.numRxAntennas = (uint8_t) gMmwMssMCB.numRxAntennas;
    params->ADCBufData.dataSize = gMmwMssMCB.profileComCfg.h_NumOfAdcSamples * params->ADCBufData.dataProperty.numRxAntennas * 4 ;
    params->ADCBufData.dataProperty.numAdcSamples = gMmwMssMCB.profileComCfg.h_NumOfAdcSamples;

    if (!gMmwMssMCB.oneTimeConfigDone)
    {
        mathUtils_genWindow((uint32_t *)params->window,
                            (uint32_t) params->ADCBufData.dataProperty.numAdcSamples,
                            params->windowSize/sizeof(uint32_t),
                            DPC_DPU_RANGEPROC_FFT_WINDOW_TYPE,
                            DPC_OBJDET_QFORMAT_RANGE_FFT);
    }
    params->rangeFFTtuning.fftOutputDivShift = 2;
    params->rangeFFTtuning.numLastButterflyStagesToScale = 0; /* no scaling needed as ADC is 16-bit and we have 8 bits to grow */

    params->rangeFftSize = mathUtils_pow2roundup(params->ADCBufData.dataProperty.numAdcSamples);

    bytesPerRxChan = params->ADCBufData.dataProperty.numAdcSamples * sizeof(uint16_t);
    bytesPerRxChan = (bytesPerRxChan + 15) / 16 * 16;

    for (index = 0; index < SYS_COMMON_NUM_RX_CHANNEL; index++)
    {
        params->ADCBufData.dataProperty.rxChanOffset[index] = index * bytesPerRxChan;
    }

    params->ADCBufData.dataProperty.interleave = DPIF_RXCHAN_NON_INTERLEAVE_MODE;
    /* Data Input EDMA */
    pHwConfig->edmaInCfg.dataIn.channel         = DPC_OBJDET_DPU_RANGEPROC_EDMAIN_CH;
    pHwConfig->edmaInCfg.dataIn.channelShadow[0]   = DPC_OBJDET_DPU_RANGEPROC_EDMAIN_SHADOW_PING;
    pHwConfig->edmaInCfg.dataIn.channelShadow[1]   = DPC_OBJDET_DPU_RANGEPROC_EDMAIN_SHADOW_PONG;
    pHwConfig->edmaInCfg.dataIn.eventQueue      = DPC_OBJDET_DPU_RANGEPROC_EDMAIN_EVENT_QUE;
    pHwConfig->edmaInCfg.dataInSignature.channel         = DPC_OBJDET_DPU_RANGEPROC_EDMAIN_SIG_CH;
    pHwConfig->edmaInCfg.dataInSignature.channelShadow   = DPC_OBJDET_DPU_RANGEPROC_EDMAIN_SIG_SHADOW;
    pHwConfig->edmaInCfg.dataInSignature.eventQueue      = DPC_OBJDET_DPU_RANGEPROC_EDMAIN_SIG_EVENT_QUE;
    pHwConfig->intrObj = intrObj_rangeProc;

    /* Data Output EDMA */
    pHwConfig->edmaOutCfg.path[0].evtDecim.channel = DPC_OBJDET_DPU_RANGEPROC_EVT_DECIM_PING_CH;
    pHwConfig->edmaOutCfg.path[0].evtDecim.channelShadow[0] = DPC_OBJDET_DPU_RANGEPROC_EVT_DECIM_PING_SHADOW_0;
    pHwConfig->edmaOutCfg.path[0].evtDecim.channelShadow[1] = DPC_OBJDET_DPU_RANGEPROC_EVT_DECIM_PING_SHADOW_1;
    pHwConfig->edmaOutCfg.path[0].evtDecim.eventQueue = DPC_OBJDET_DPU_RANGEPROC_EVT_DECIM_PING_EVENT_QUE;

    pHwConfig->edmaOutCfg.path[1].evtDecim.channel = DPC_OBJDET_DPU_RANGEPROC_EVT_DECIM_PONG_CH;
    pHwConfig->edmaOutCfg.path[1].evtDecim.channelShadow[0] = DPC_OBJDET_DPU_RANGEPROC_EVT_DECIM_PONG_SHADOW_0;
    pHwConfig->edmaOutCfg.path[1].evtDecim.channelShadow[1] = DPC_OBJDET_DPU_RANGEPROC_EVT_DECIM_PONG_SHADOW_1;
    pHwConfig->edmaOutCfg.path[1].evtDecim.eventQueue = DPC_OBJDET_DPU_RANGEPROC_EVT_DECIM_PONG_EVENT_QUE;

    pHwConfig->edmaOutCfg.path[0].dataOutMinor.channel = DPC_OBJDET_DPU_RANGEPROC_EDMAOUT_MINOR_PING_CH;
    pHwConfig->edmaOutCfg.path[0].dataOutMinor.channelShadow = DPC_OBJDET_DPU_RANGEPROC_EDMAOUT_MINOR_PING_SHADOW;
    pHwConfig->edmaOutCfg.path[0].dataOutMinor.eventQueue = DPC_OBJDET_DPU_RANGEPROC_EDMAOUT_MINOR_PING_EVENT_QUE;

    pHwConfig->edmaOutCfg.path[1].dataOutMinor.channel = DPC_OBJDET_DPU_RANGEPROC_EDMAOUT_MINOR_PONG_CH;
    pHwConfig->edmaOutCfg.path[1].dataOutMinor.channelShadow = DPC_OBJDET_DPU_RANGEPROC_EDMAOUT_MINOR_PONG_SHADOW;
    pHwConfig->edmaOutCfg.path[1].dataOutMinor.eventQueue = DPC_OBJDET_DPU_RANGEPROC_EDMAOUT_MINOR_PONG_EVENT_QUE;

    pHwConfig->edmaOutCfg.path[0].dataOutMajor.channel = DPC_OBJDET_DPU_RANGEPROC_EDMAOUT_MAJOR_PING_CH;
    pHwConfig->edmaOutCfg.path[0].dataOutMajor.channelShadow = DPC_OBJDET_DPU_RANGEPROC_EDMAOUT_MAJOR_PING_SHADOW;
    pHwConfig->edmaOutCfg.path[0].dataOutMajor.eventQueue = DPC_OBJDET_DPU_RANGEPROC_EDMAOUT_MAJOR_PING_EVENT_QUE;

    pHwConfig->edmaOutCfg.path[1].dataOutMajor.channel = DPC_OBJDET_DPU_RANGEPROC_EDMAOUT_MAJOR_PONG_CH;
    pHwConfig->edmaOutCfg.path[1].dataOutMajor.channelShadow = DPC_OBJDET_DPU_RANGEPROC_EDMAOUT_MAJOR_PONG_SHADOW;
    pHwConfig->edmaOutCfg.path[1].dataOutMajor.eventQueue = DPC_OBJDET_DPU_RANGEPROC_EDMAOUT_MAJOR_PONG_EVENT_QUE;

    /* Radar cube Minor Motion*/
    if (params->enableMinorMotion)
    {
        gMmwMssMCB.radarCube[1].dataSize = params->numRangeBins * params->numVirtualAntennas * sizeof(cmplx16ReIm_t) * params->numDopplerChirpsPerProc;
        gMmwMssMCB.radarCube[1].data  = (cmplx16ImRe_t *) DPC_ObjDet_MemPoolAlloc(&gMmwMssMCB.L3RamObj,
                                                                                     gMmwMssMCB.radarCube[1].dataSize,
                                                                                     sizeof(uint32_t));
        if(gMmwMssMCB.radarCube[1].data == NULL)
        {
            retVal = DPC_OBJECTDETECTION_ENOMEM__L3_RAM_RADAR_CUBE;
            goto exit;
        }

    }
    else
    {
        gMmwMssMCB.radarCube[1].data  = NULL;
        gMmwMssMCB.radarCube[1].dataSize = 0;
    }
    gMmwMssMCB.radarCube[1].datafmt = DPIF_RADARCUBE_FORMAT_6;
    rangeProcDpuCfg.hwRes.radarCubeMinMot = gMmwMssMCB.radarCube[1];

    /* Radar cube Major Motion*/
    if (params->enableMajorMotion)
    {

        gMmwMssMCB.radarCube[0].dataSize = params->numRangeBins * params->numVirtualAntennas * sizeof(cmplx16ReIm_t) * params->numDopplerChirpsPerProc;
        gMmwMssMCB.radarCube[0].data  = (cmplx16ImRe_t *) DPC_ObjDet_MemPoolAlloc(&gMmwMssMCB.L3RamObj,
                                                                               gMmwMssMCB.radarCube[0].dataSize,
                                                                               sizeof(uint32_t));
        if(gMmwMssMCB.radarCube[0].data == NULL)
        {
            retVal = DPC_OBJECTDETECTION_ENOMEM__L3_RAM_RADAR_CUBE;
            goto exit;
        }


    }
    else
    {
        gMmwMssMCB.radarCube[0].data  = NULL;
        gMmwMssMCB.radarCube[0].dataSize = 0;
    }
    gMmwMssMCB.radarCube[0].datafmt = DPIF_RADARCUBE_FORMAT_6;
    rangeProcDpuCfg.hwRes.radarCube = gMmwMssMCB.radarCube[0];

exit:
    return retVal;
}

/**
*  @b Description
*  @n
*    Based on the configuration, set up the doa processing DPU configurations
*/
int32_t DoaProc_configParser()
{
    DPIF_DetMatrix dopplerIndexMatrix;
    DPIF_DetMatrix elevationIndexMatrix;

    /* Doaproc DPU */
    DPU_DoaProc_EdmaCfg *edmaCfg;
    DPU_DoaProc_HwaCfg *hwaCfg;
    int32_t winGenLen, i;
    int32_t retVal = 0;
    DPU_DoaProc_StaticConfig  *doaStaticCfg;

    memset((void *)&doaProcDpuCfg, 0, sizeof(DPU_DoaProc_Config));

    hwRes = &doaProcDpuCfg.hwRes;
    doaStaticCfg = &doaProcDpuCfg.staticCfg;
    edmaCfg = &hwRes->edmaCfg;
    hwaCfg = &hwRes->hwaCfg;

    /* Select active antennas from available antennas and calculate number of antennas rows and columns */
    MmwDemo_calcActiveAntennaGeometry();

    doaStaticCfg->numAntRow = gMmwMssMCB.numAntRow;
    doaStaticCfg->numAntCol = gMmwMssMCB.numAntCol;


    /* Angle dimension */
    if ((gMmwMssMCB.numAntRow > 1) && (gMmwMssMCB.numAntCol > 1))
    {
        gMmwMssMCB.angleDimension = 2;
    }
    else if ((gMmwMssMCB.numAntRow == 1) && (gMmwMssMCB.numAntCol > 1))
    {
        gMmwMssMCB.angleDimension = 1;
    }
    else
    {
        gMmwMssMCB.angleDimension = 0;
    }


    doaStaticCfg->enableMajorMotion = gMmwMssMCB.enableMajorMotion;
    doaStaticCfg->enableMinorMotion = gMmwMssMCB.enableMinorMotion;

    doaStaticCfg->numTxAntennas = (uint8_t) gMmwMssMCB.numTxAntennas;
    doaStaticCfg->numRxAntennas = (uint8_t) gMmwMssMCB.numRxAntennas;
    doaStaticCfg->numVirtualAntennas = (uint8_t) (gMmwMssMCB.numTxAntennas * gMmwMssMCB.numRxAntennas);
    doaStaticCfg->numRangeBins = gMmwMssMCB.numRangeBins;


    doaStaticCfg->numMinorMotionChirpsPerFrame = gMmwMssMCB.sigProcChainCfg.numMinorMotionChirpsPerFrame;
    doaStaticCfg->numFrmPerMinorMotProc = gMmwMssMCB.sigProcChainCfg.numFrmPerMinorMotProc;
    if (doaStaticCfg->numTxAntennas > 1)
    {
        if (doaStaticCfg->enableMajorMotion)
        {
            if ((gMmwMssMCB.frameCfg.h_NumOfBurstsInFrame > 1) && (gMmwMssMCB.frameCfg.h_NumOfChirpsInBurst == 2))
            {
                /* Burst mode: h_NumOfBurstsInFrame > 1, h_NumOfChirpsInBurst = 2 */
                doaStaticCfg->numDopplerChirps   = gMmwMssMCB.frameCfg.h_NumOfBurstsInFrame;
            }
            else if (gMmwMssMCB.frameCfg.h_NumOfBurstsInFrame == 1)
            {
                /* Normal mode: h_NumOfBurstsInFrame = 1, h_NumOfChirpsInBurst >= doaStaticCfg->numTxAntennas */
                doaStaticCfg->numDopplerChirps   = gMmwMssMCB.frameCfg.h_NumOfChirpsInBurst / doaStaticCfg->numTxAntennas;
            }
            else
            {
                retVal = DPU_DOAPROC_EINVAL;
                goto exit;
            }
        }
        else
        {
            /* Minor motion */
            doaStaticCfg->numDopplerChirps   = gMmwMssMCB.sigProcChainCfg.numFrmPerMinorMotProc * gMmwMssMCB.sigProcChainCfg.numMinorMotionChirpsPerFrame;
        }
    }
    else
    {
        /* 1Tx antenna */
        if (gMmwMssMCB.frameCfg.h_NumOfBurstsInFrame != 1)
        {
            retVal = DPU_DOAPROC_EINVAL;
            goto exit;
        }

        if (doaStaticCfg->enableMajorMotion)
        {
            doaStaticCfg->numDopplerChirps = gMmwMssMCB.frameCfg.h_NumOfChirpsInBurst;
        }
        else
        {
            /* This is to support 1Tx/1chirpPerFrame/NchirpsAcrossFrames in minor mode */
            doaStaticCfg->numDopplerChirps = gMmwMssMCB.sigProcChainCfg.numFrmPerMinorMotProc * gMmwMssMCB.sigProcChainCfg.numMinorMotionChirpsPerFrame; //These are now chirps, not chirp pairs
        }
    }


    doaStaticCfg->numDopplerBins     = mathUtils_pow2roundup(doaStaticCfg->numDopplerChirps);
    doaStaticCfg->log2NumDopplerBins = mathUtils_ceilLog2(doaStaticCfg->numDopplerBins);

    gMmwMssMCB.numDopplerBins = doaStaticCfg->numDopplerBins;

    doaStaticCfg->selectCoherentPeakInDopplerDim = gMmwMssMCB.sigProcChainCfg.coherentDoppler;
    doaStaticCfg->angleDimension        = gMmwMssMCB.angleDimension;
    doaStaticCfg->isDetMatrixLogScale   = false;
    doaStaticCfg->azimuthFftSize        = gMmwMssMCB.sigProcChainCfg.azimuthFftSize;
    doaStaticCfg->elevationFftSize      = gMmwMssMCB.sigProcChainCfg.elevationFftSize;
    doaStaticCfg->isStaticClutterRemovalEnabled = gMmwMssMCB.staticClutterRemovalEnable;
    doaStaticCfg->isRxChGainPhaseCompensationEnabled   = 1;
    doaStaticCfg->doaRangeLoopType = 0;
    doaStaticCfg->dopElevDimReductOrder = gMmwMssMCB.sigProcChainCfg.dopElevDimReductOrder;

    /* Configure Doppler fft param sets for mapping antennas into 1D/2D virtual antenna array */
    retVal = MmwDemo_cfgDopplerParamMapping(&hwaCfg->doaRngGateCfg, DOPPLER_OUTPUT_MAPPING_DOP_ROW_COL);
    if (retVal < 0)
    {
        goto exit;
    }

    /* L3 - Detection Matrix */
    for (i=0; i<2; i++)
    {
        if (((i==0) && doaStaticCfg->enableMajorMotion) || ((i==1) && (doaStaticCfg->enableMinorMotion)))
        {
            if(doaStaticCfg->isDetMatrixLogScale)
            {
                gMmwMssMCB.detMatrix[i].dataSize = doaStaticCfg->numRangeBins * doaStaticCfg->azimuthFftSize * sizeof(uint16_t);
            }
            else
            {
                gMmwMssMCB.detMatrix[i].dataSize = doaStaticCfg->numRangeBins * doaStaticCfg->azimuthFftSize * sizeof(uint32_t);
            }
            gMmwMssMCB.detMatrix[i].data = DPC_ObjDet_MemPoolAlloc(&gMmwMssMCB.L3RamObj,
                                                        gMmwMssMCB.detMatrix[i].dataSize,
                                                        sizeof(uint32_t));
            if (gMmwMssMCB.detMatrix[i].data == NULL)
            {
                retVal = DPC_OBJECTDETECTION_ENOMEM__L3_RAM_DET_MATRIX;
                goto exit;
            }
            gMmwMssMCB.detMatrix[i].datafmt = DPC_DPU_DPIF_DETMATRIX_FORMAT_2;


            gMmwMssMCB.rangeProfile[i] = DPC_ObjDet_MemPoolAlloc(&gMmwMssMCB.L3RamObj,
                                                                 doaStaticCfg->numRangeBins * sizeof(uint32_t),
                                                                 sizeof(uint32_t));
            if (gMmwMssMCB.rangeProfile[i] == NULL)
            {
                retVal = DPC_OBJECTDETECTION_ENOMEM__CORE_LOCAL_RAM_RANGE_PROFILE;
                goto exit;
            }
        }
        else
        {
            gMmwMssMCB.detMatrix[i].dataSize = 0;
            gMmwMssMCB.detMatrix[i].data = NULL;
            gMmwMssMCB.detMatrix[i].datafmt = 0;
            gMmwMssMCB.rangeProfile[i] = NULL;
        }
    }

    if (doaStaticCfg->dopElevDimReductOrder == 0)
    {
        /* V1: Dimensionality reduction order: Doppler then Elevation */
        /* L3 - Doppler Index Matrix */
        if ((doaStaticCfg->selectCoherentPeakInDopplerDim == 1) ||
            (doaStaticCfg->selectCoherentPeakInDopplerDim == 2))
        {
            if (doaStaticCfg->angleDimension == 2)
            {
                /* 2D-case -  with elevation */
                dopplerIndexMatrix.dataSize = doaStaticCfg->numRangeBins * doaStaticCfg->azimuthFftSize * doaStaticCfg->elevationFftSize * sizeof(uint8_t);
            }
            else
            {
                /* 1D-case -  no elevation */
                dopplerIndexMatrix.dataSize = doaStaticCfg->numRangeBins * doaStaticCfg->azimuthFftSize * sizeof(uint8_t);
            }
            dopplerIndexMatrix.data = DPC_ObjDet_MemPoolAlloc(&gMmwMssMCB.L3RamObj,
                                                              dopplerIndexMatrix.dataSize,
                                                              sizeof(uint8_t));
            if (dopplerIndexMatrix.data == NULL)
            {
                retVal = DPC_OBJECTDETECTION_ENOMEM__L3_RAM_DET_MATRIX;
                goto exit;
            }
            dopplerIndexMatrix.datafmt = DPC_DPU_DPIF_DETMATRIX_FORMAT_2;
        }
        else
        {
            /* Non-coherent combining along Doppler dimension, Doppler output = 0 */
            dopplerIndexMatrix.dataSize = 0;
            dopplerIndexMatrix.data = NULL;
            dopplerIndexMatrix.datafmt = 0;
        }

        if (doaStaticCfg->angleDimension == 2)
        {
            /* L3 - Elevation Index Matrix */
            elevationIndexMatrix.dataSize = doaStaticCfg->numRangeBins * doaStaticCfg->azimuthFftSize * sizeof(uint8_t);
            elevationIndexMatrix.data = DPC_ObjDet_MemPoolAlloc(&gMmwMssMCB.L3RamObj,
                                                                elevationIndexMatrix.dataSize,
                                                                sizeof(uint8_t));
            if (elevationIndexMatrix.data == NULL)
            {
                retVal = DPC_OBJECTDETECTION_ENOMEM__L3_RAM_DET_MATRIX;
                goto exit;
            }
            elevationIndexMatrix.datafmt = DPC_DPU_DPIF_DETMATRIX_FORMAT_2;
        }
        else
        {
            elevationIndexMatrix.dataSize = 0;
            elevationIndexMatrix.data = NULL;
            elevationIndexMatrix.datafmt = 0;
        }
    }
    else
    {
        /* V2: Dimensionality reduction order: Elevation then Doppler */
        /* L3 - Doppler Index Matrix */
        if ((doaStaticCfg->selectCoherentPeakInDopplerDim == 1) ||
            (doaStaticCfg->selectCoherentPeakInDopplerDim == 2))
        {
            if (doaStaticCfg->angleDimension == 2)
            {
                /* 2D-case -  with elevation */
                dopplerIndexMatrix.dataSize = doaStaticCfg->numRangeBins * doaStaticCfg->azimuthFftSize * sizeof(uint8_t);
            }
            else
            {
                /* 1D-case -  no elevation */
                dopplerIndexMatrix.dataSize = doaStaticCfg->numRangeBins * doaStaticCfg->azimuthFftSize * sizeof(uint8_t);
            }
            dopplerIndexMatrix.data = DPC_ObjDet_MemPoolAlloc(&gMmwMssMCB.L3RamObj,
                                                              dopplerIndexMatrix.dataSize,
                                                              sizeof(uint8_t));
            if (dopplerIndexMatrix.data == NULL)
            {
                retVal = DPC_OBJECTDETECTION_ENOMEM__L3_RAM_DET_MATRIX;
                goto exit;
            }
            dopplerIndexMatrix.datafmt = DPC_DPU_DPIF_DETMATRIX_FORMAT_2;
        }
        else
        {
            /* Non-coherent combining along Doppler dimension, Doppler output = 0 */
            dopplerIndexMatrix.dataSize = 0;
            dopplerIndexMatrix.data = NULL;
            dopplerIndexMatrix.datafmt = 0;
        }
        if (doaStaticCfg->angleDimension == 2)
        {
             /* L3 - Elevation Index Matrix */
            elevationIndexMatrix.dataSize = doaStaticCfg->numRangeBins * doaStaticCfg->azimuthFftSize * doaStaticCfg->numDopplerBins *sizeof(uint8_t);
            elevationIndexMatrix.data = DPC_ObjDet_MemPoolAlloc(&gMmwMssMCB.L3RamObj,
                                                                elevationIndexMatrix.dataSize,
                                                                sizeof(uint8_t));
            if (elevationIndexMatrix.data == NULL)
            {
                retVal = DPC_OBJECTDETECTION_ENOMEM__L3_RAM_DET_MATRIX;
                goto exit;
            }
            elevationIndexMatrix.datafmt = DPC_DPU_DPIF_DETMATRIX_FORMAT_2;
        }
        else
        {
            elevationIndexMatrix.dataSize = 0;
            elevationIndexMatrix.data = NULL;
            elevationIndexMatrix.datafmt = 0;
        }
    }

    /* hwRes - copy these structures */
    hwRes->radarCube = gMmwMssMCB.radarCube[0];
    hwRes->radarCubeMinMot = gMmwMssMCB.radarCube[1];

    hwRes->dopplerIndexMatrix = dopplerIndexMatrix;
    hwRes->elevationIndexMatrix = elevationIndexMatrix;

    gMmwMssMCB.dopplerIndexMatrix = dopplerIndexMatrix;
    gMmwMssMCB.elevationIndexMatrix = elevationIndexMatrix;

    /* hwRes - edmaCfg */
    edmaCfg->edmaHandle = gEdmaHandle[0];

    /* edmaIn - ping - minor motion*/
    edmaCfg->edmaIn.chunk[0].channel =            DPC_OBJDET_DPU_DOAPROC_EDMAIN_PING_CH;
    edmaCfg->edmaIn.chunk[0].channelShadow =      DPC_OBJDET_DPU_DOAPROC_EDMAIN_PING_SHADOW;
    edmaCfg->edmaIn.chunk[0].eventQueue =         DPC_OBJDET_DPU_DOAPROC_EDMAIN_PING_EVENT_QUE;

    /* edmaIn - pong - minor motion*/
    edmaCfg->edmaIn.chunk[1].channel =            DPC_OBJDET_DPU_DOAPROC_EDMAIN_PONG_CH;
    edmaCfg->edmaIn.chunk[1].channelShadow =      DPC_OBJDET_DPU_DOAPROC_EDMAIN_PONG_SHADOW;
    edmaCfg->edmaIn.chunk[1].eventQueue =         DPC_OBJDET_DPU_DOAPROC_EDMAIN_PONG_EVENT_QUE;

    /* edmaHotSig */
    edmaCfg->edmaHotSig.channel =             DPC_OBJDET_DPU_DOAPROC_EDMA_HOT_SIG_CH;
    edmaCfg->edmaHotSig.channelShadow =       DPC_OBJDET_DPU_DOAPROC_EDMA_HOT_SIG_SHADOW;
    edmaCfg->edmaHotSig.eventQueue =          DPC_OBJDET_DPU_DOAPROC_EDMA_HOT_SIG_EVENT_QUE;




    /* edmaOut - Detection Matrix */
    edmaCfg->edmaDetMatOut.channel =           DPC_OBJDET_DPU_DOAPROC_EDMAOUT_DET_MATRIX_CH;
    edmaCfg->edmaDetMatOut.channelShadow =     DPC_OBJDET_DPU_DOAPROC_EDMAOUT_DET_MATRIX_SHADOW;
    edmaCfg->edmaDetMatOut.eventQueue =        DPC_OBJDET_DPU_DOAPROC_EDMAOUT_DET_MATRIX_EVENT_QUE;

    /* edmaOut - Elevation Index Matrix */
    edmaCfg->elevIndMatOut.channel =       DPC_OBJDET_DPU_DOAPROC_EDMAOUT_ELEVIND_MATRIX_CH;
    edmaCfg->elevIndMatOut.channelShadow = DPC_OBJDET_DPU_DOAPROC_EDMAOUT_ELEVIND_MATRIX_SHADOW;
    edmaCfg->elevIndMatOut.eventQueue =    DPC_OBJDET_DPU_DOAPROC_EDMAOUT_ELEVIND_MATRIX_EVENT_QUE;

    /* edmaOut - Doppler Index Matrix */
    edmaCfg->dopIndMatOut.channel =        DPC_OBJDET_DPU_DOAPROC_EDMAOUT_DOPIND_MATRIX_CH;
    edmaCfg->dopIndMatOut.channelShadow =  DPC_OBJDET_DPU_DOAPROC_EDMAOUT_DOPIND_MATRIX_SHADOW;
    edmaCfg->dopIndMatOut.eventQueue =     DPC_OBJDET_DPU_DOAPROC_EDMAOUT_DOPIND_MATRIX_EVENT_QUE;

    edmaCfg->edmaInterLoopOut.channel =       DPC_OBJDET_DPU_DOAPROC_INTER_LOOP_EDMAOUT_DET_MATRIX_CH;
    edmaCfg->edmaInterLoopOut.channelShadow = DPC_OBJDET_DPU_DOAPROC_INTER_LOOP_EDMAOUT_DET_MATRIX_SHADOW;
    edmaCfg->edmaInterLoopOut.eventQueue =    DPC_OBJDET_DPU_DOAPROC_INTER_LOOP_EDMAOUT_DET_MATRIX_EVENT_QUE;

    edmaCfg->edmaInterLoopIn.channel =       DPC_OBJDET_DPU_DOAPROC_INTER_LOOP_EDMAIN_CH;
    edmaCfg->edmaInterLoopIn.channelShadow = DPC_OBJDET_DPU_DOAPROC_INTER_LOOP_EDMAIN_SHADOW;
    edmaCfg->edmaInterLoopIn.eventQueue =    DPC_OBJDET_DPU_DOAPROC_INTER_LOOP_EDMAIN_EVENT_QUE;

    edmaCfg->edmaInterLoopHotSig.channel =       DPC_OBJDET_DPU_DOAPROC_INTER_LOOP_EDMA_HOT_SIG_CH;
    edmaCfg->edmaInterLoopHotSig.channelShadow = DPC_OBJDET_DPU_DOAPROC_INTER_LOOP_EDMA_HOT_SIG_SHADOW;
    edmaCfg->edmaInterLoopHotSig.eventQueue =    DPC_OBJDET_DPU_DOAPROC_INTER_LOOP_EDMA_HOT_SIG_EVENT_QUE;

    edmaCfg->edmaInterLoopChainBack.channel =       DPC_OBJDET_DPU_DOAPROC_INTER_LOOP_EDMA_CHAIN_BACK_CH;
    edmaCfg->edmaInterLoopChainBack.channelShadow = DPC_OBJDET_DPU_DOAPROC_INTER_LOOP_EDMA_CHAIN_BACK_SHADOW;
    edmaCfg->edmaInterLoopChainBack.eventQueue =    DPC_OBJDET_DPU_DOAPROC_INTER_LOOP_EDMA_CHAIN_BACK_EVENT_QUE;
    edmaCfg->intrObj = &intrObj_doaProc;

    /* hwaCfg */
    hwaCfg->hwaMemInpAddr = CSL_APP_HWA_DMA0_RAM_BANK3_BASE;
    hwaCfg->numParamSets = 0;  //The number depends on the configuration, will be populated by DPU_DoaProc_config()
    hwaCfg->paramSetStartIdx = DPU_RANGEPROCHWA_NUM_HWA_PARAM_SETS;

    hwaCfg->dmaTrigSrcChan = DPC_ObjDet_HwaDmaTrigSrcChanPoolAlloc(&gMmwMssMCB.HwaDmaChanPoolObj);

    /* hwaCfg - window */
    //Share FFT window between azimuth and elevation FFT, window = [+1 -1 +1 -1 ... ]
    winGenLen = (doaStaticCfg->azimuthFftSize > doaStaticCfg->elevationFftSize) ? doaStaticCfg->azimuthFftSize : doaStaticCfg->elevationFftSize;
    hwaCfg->windowSize = winGenLen * sizeof(int32_t);

    hwaCfg->window = (int32_t *)DPC_ObjDet_MemPoolAlloc(&gMmwMssMCB.CoreLocalRamObj,
                                                        hwaCfg->windowSize,
                                                        sizeof(uint32_t));
    if (hwaCfg->window == NULL)
    {
        retVal = DPC_OBJECTDETECTION_ENOMEM__CORE_LOCAL_RAM_DOA_HWA_WINDOW;
        goto exit;
    }
    /*Alternate 1,-1,...*/
    for (i=0; i<winGenLen; i++)
    {
        hwaCfg->window[i] = (1 - 2 * (i & 0x1)) * ((1<<17) - 1);
    }

    hwaCfg->winRamOffset = doaStaticCfg->numRangeBins;
    hwaCfg->winSym = HWA_FFT_WINDOW_NONSYMMETRIC;

    /* Rx compensation coefficients */
    {
        int32_t rxInd, txInd;
        int32_t ind = 0;
        doaStaticCfg->compRxChanCfg.rangeBias = gMmwMssMCB.compRxChannelBiasCfg.rangeBias;
        for (txInd = 0; txInd < doaStaticCfg->numTxAntennas; txInd++)
        {
            for (rxInd = 0; rxInd < doaStaticCfg->numRxAntennas; rxInd++)
            {
                doaStaticCfg->compRxChanCfg.rxChPhaseComp[ind++] = gMmwMssMCB.compRxChannelBiasCfg.rxChPhaseComp[gMmwMssMCB.rxAntOrder[rxInd] + (txInd * SYS_COMMON_NUM_RX_CHANNEL)];
            }
        }
    }

    hwRes->interLoopDataBuffer = NULL;

exit:
return retVal;
}

/**
*  @b Description
*  @n
*    Based on the configuration, set up the aoasvc processing DPU configurations
*/
int32_t AoasvcProc_configParser()
{

    /* Aoasvcproc DPU */
    DPU_AoasvcProc_EdmaCfg *edmaCfg;
    DPU_AoasvcProc_HwaCfg *hwaCfg;
    int32_t retVal = 0;
    DPU_AoasvcProc_StaticConfig  *aoasvcStaticCfg;
    DPU_AoasvcProc_HW_Resources  *hwRes;
    float slope;
    memset((void *)&aoasvcProcDpuCfg, 0, sizeof(DPU_AoasvcProc_Config));

    hwRes = &aoasvcProcDpuCfg.hwRes;
    aoasvcStaticCfg = &aoasvcProcDpuCfg.staticCfg;
    edmaCfg = &hwRes->edmaCfg;
    hwaCfg = &hwRes->hwaCfg;

    aoasvcStaticCfg->numAntRow = gMmwMssMCB.numAntRow;
    aoasvcStaticCfg->numAntCol = gMmwMssMCB.numAntCol;

    aoasvcStaticCfg->enableMajorMotion = gMmwMssMCB.enableMajorMotion;
    aoasvcStaticCfg->enableMinorMotion = gMmwMssMCB.enableMinorMotion;

    aoasvcStaticCfg->numTxAntennas = (uint8_t) gMmwMssMCB.numTxAntennas;
    aoasvcStaticCfg->numRxAntennas = (uint8_t) gMmwMssMCB.numRxAntennas;
    aoasvcStaticCfg->numVirtualAntennas = (uint8_t) (gMmwMssMCB.numTxAntennas * gMmwMssMCB.numRxAntennas);
    aoasvcStaticCfg->numRangeBins = gMmwMssMCB.numRangeBins;

    aoasvcStaticCfg->enableSteeringVectorCorrection =  gMmwMssMCB.steeringVecCorrCfg.enableSteeringVectorCorrection;
    aoasvcStaticCfg->enableAngleInterpolation =  gMmwMssMCB.steeringVecCorrCfg.enableAngleInterpolation;
    aoasvcStaticCfg->enableAngleInterpolation = gMmwMssMCB.steeringVecCorrCfg.enableAngleInterpolation;

    /* Steering vectors parameters */
    aoasvcStaticCfg->azimStart = SteeringVecParams.azimuthStartDeg;
    aoasvcStaticCfg->azimStep = SteeringVecParams.azimuthStepDeg;
    aoasvcStaticCfg->elevStart = SteeringVecParams.elevationStartDeg;
    aoasvcStaticCfg->elevStep = SteeringVecParams.elevationStepDeg;
    aoasvcStaticCfg->numAzimVec =  round(2 * fabs(aoasvcStaticCfg->azimStart) / aoasvcStaticCfg->azimStep + 1);
    aoasvcStaticCfg->numElevVec =  round(2 * fabs(aoasvcStaticCfg->elevStart) / aoasvcStaticCfg->elevStep + 1);

    slope = (float)(gMmwMssMCB.chirpSlope * 1.e12);
    aoasvcStaticCfg->rangeStep = (MMWDEMO_RFPARSER_SPEED_OF_LIGHT_IN_METERS_PER_SEC * (gMmwMssMCB.adcSamplingRate * 1.e6)) /
                                  (2.f * slope * (2*aoasvcStaticCfg->numRangeBins));

    if (aoasvcStaticCfg->numTxAntennas > 1)
    {
        if (aoasvcStaticCfg->enableMajorMotion)
        {
            if ((gMmwMssMCB.frameCfg.h_NumOfBurstsInFrame > 1) && (gMmwMssMCB.frameCfg.h_NumOfChirpsInBurst == 2))
            {
                /* Burst mode: h_NumOfBurstsInFrame > 1, h_NumOfChirpsInBurst = 2 */
                aoasvcStaticCfg->numDopplerChirps   = gMmwMssMCB.frameCfg.h_NumOfBurstsInFrame;
            }
            else if (gMmwMssMCB.frameCfg.h_NumOfBurstsInFrame == 1)
            {
                /* Normal mode: h_NumOfBurstsInFrame = 1, h_NumOfChirpsInBurst >= aoasvcStaticCfg->numTxAntennas */
                aoasvcStaticCfg->numDopplerChirps   = gMmwMssMCB.frameCfg.h_NumOfChirpsInBurst / aoasvcStaticCfg->numTxAntennas;
            }
            else
            {
                retVal = DPU_AOASVCPROC_EINVAL;
                goto exit;
            }
        }
        else
        {
            /* Minor motion */
            aoasvcStaticCfg->numDopplerChirps   = gMmwMssMCB.sigProcChainCfg.numFrmPerMinorMotProc * gMmwMssMCB.sigProcChainCfg.numMinorMotionChirpsPerFrame;
        }
    }
    else
    {
        /* 1Tx antenna */
        if (gMmwMssMCB.frameCfg.h_NumOfBurstsInFrame != 1)
        {
            retVal = DPU_AOASVCPROC_EINVAL;
            goto exit;
        }

        if (aoasvcStaticCfg->enableMajorMotion)
        {
            aoasvcStaticCfg->numDopplerChirps = gMmwMssMCB.frameCfg.h_NumOfChirpsInBurst;
        }
        else
        {
            /* This is to support 1Tx/1chirpPerFrame/NchirpsAcrossFrames in minor mode */
            aoasvcStaticCfg->numDopplerChirps = gMmwMssMCB.sigProcChainCfg.numFrmPerMinorMotProc * gMmwMssMCB.sigProcChainCfg.numMinorMotionChirpsPerFrame; //These are now chirps, not chirp pairs
        }
    }


    aoasvcStaticCfg->numDopplerBins     = mathUtils_pow2roundup(aoasvcStaticCfg->numDopplerChirps);
    aoasvcStaticCfg->log2NumDopplerBins = mathUtils_ceilLog2(aoasvcStaticCfg->numDopplerBins);


    aoasvcStaticCfg->selectCoherentPeakInDopplerDim = gMmwMssMCB.sigProcChainCfg.coherentDoppler;
    aoasvcStaticCfg->angleDimension        = gMmwMssMCB.angleDimension;
    aoasvcStaticCfg->isDetMatrixLogScale   = false;
    aoasvcStaticCfg->azimuthFftSize        = gMmwMssMCB.sigProcChainCfg.azimuthFftSize;
    aoasvcStaticCfg->elevationFftSize      = gMmwMssMCB.sigProcChainCfg.elevationFftSize;
    aoasvcStaticCfg->isStaticClutterRemovalEnabled = gMmwMssMCB.staticClutterRemovalEnable;
    aoasvcStaticCfg->isRxChGainPhaseCompensationEnabled   = 1;
    aoasvcStaticCfg->aoasvcRangeLoopType = 0;

    /* hwRes - copy these structures */
    hwRes->radarCube[0] = gMmwMssMCB.radarCube[0];
    hwRes->radarCube[1] = gMmwMssMCB.radarCube[1];
    hwRes->virtAntElemList = gMmwMssMCB.virtAntElemList;

    /* hwRes - edmaCfg */
    edmaCfg->edmaHandle = gEdmaHandle[0];

    /* edmaIn - ping - minor motion*/
    edmaCfg->edmaIn[0].channel =            DPC_OBJDET_DPU_AOASVCPROC_EDMAIN_PING_CH;
    edmaCfg->edmaIn[0].channelShadow =      DPC_OBJDET_DPU_AOASVCPROC_EDMAIN_PING_SHADOW;
    edmaCfg->edmaIn[0].eventQueue =         DPC_OBJDET_DPU_AOASVCPROC_EDMAIN_PING_EVENT_QUE;

    /* edmaIn - pong - minor motion*/
    edmaCfg->edmaIn[1].channel =            DPC_OBJDET_DPU_AOASVCPROC_EDMAIN_PONG_CH;
    edmaCfg->edmaIn[1].channelShadow =      DPC_OBJDET_DPU_AOASVCPROC_EDMAIN_PONG_SHADOW;
    edmaCfg->edmaIn[1].eventQueue =         DPC_OBJDET_DPU_AOASVCPROC_EDMAIN_PONG_EVENT_QUE;

    /* edmaHotSig */
    edmaCfg->edmaHotSig.channel =             DPC_OBJDET_DPU_AOASVCPROC_EDMA_HOT_SIG_CH;
    edmaCfg->edmaHotSig.channelShadow =       DPC_OBJDET_DPU_AOASVCPROC_EDMA_HOT_SIG_SHADOW;
    edmaCfg->edmaHotSig.eventQueue =          DPC_OBJDET_DPU_AOASVCPROC_EDMA_HOT_SIG_EVENT_QUE;

    /* edmaOut - */
    edmaCfg->edmaOut[0].channel =           DPC_OBJDET_DPU_AOASVCPROC_EDMAOUT_PING_CH;
    edmaCfg->edmaOut[0].channelShadow =     DPC_OBJDET_DPU_AOASVCPROC_EDMAOUT_PING_SHADOW;
    edmaCfg->edmaOut[0].eventQueue =        DPC_OBJDET_DPU_AOASVCPROC_EDMAOUT_PING_EVENT_QUE;

    edmaCfg->edmaOut[1].channel =           DPC_OBJDET_DPU_AOASVCPROC_EDMAOUT_PONG_CH;
    edmaCfg->edmaOut[1].channelShadow =     DPC_OBJDET_DPU_AOASVCPROC_EDMAOUT_PONG_SHADOW;
    edmaCfg->edmaOut[1].eventQueue =        DPC_OBJDET_DPU_AOASVCPROC_EDMAOUT_PONG_EVENT_QUE;

    edmaCfg->edmaInSteerVec.channel =       DPC_OBJDET_DPU_AOASVCPROC_EDMAIN_STVEC_CH;
    edmaCfg->edmaInSteerVec.channelShadow = DPC_OBJDET_DPU_AOASVCPROC_EDMAIN_STVEC_SHADOW;
    edmaCfg->edmaInSteerVec.eventQueue =    DPC_OBJDET_DPU_AOASVCPROC_EDMAIN_STVEC_EVENT_QUE;

    /* hwaCfg */
    hwaCfg->paramSetStartIdx = DPU_RANGEPROCHWA_NUM_HWA_PARAM_SETS +
            DPU_DOAPROC_MAX_NUM_HWA_PARAMSET +
            DPU_CFARPROCHWA_MAX_NUM_HWA_PARAMSET;

    hwaCfg->dmaTrigSrcChan[0] = DPC_ObjDet_HwaDmaTrigSrcChanPoolAlloc(&gMmwMssMCB.HwaDmaChanPoolObj);
    hwaCfg->dmaTrigSrcChan[1] = DPC_ObjDet_HwaDmaTrigSrcChanPoolAlloc(&gMmwMssMCB.HwaDmaChanPoolObj);

exit:
return retVal;
}

/**
*  @b Description
*  @n
*    Based on the configuration, set up the micro Doppler processing DPU configurations
*/
int32_t uDopProc_configParser()
{
    /* Doaproc DPU */
    int32_t retVal = 0;
    DPU_uDopProc_EdmaCfg *edmaCfg;
    DPU_uDopProc_HwaCfg *hwaCfg;
    DPU_uDopProc_StaticConfig  *uDopStaticCfg;
    DPU_uDopProc_HW_Resources  *hwRes;
    float adcStart, startFreq, slope, bandwidth, centerFreq;

    memset((void *)&uDopProcDpuCfg, 0, sizeof(uDopProcDpuCfg));

    if (!gMmwMssMCB.oneTimeConfigDone)
    {
        uDopProcDpuCfg.isFirstTimeCfg = true;
    }
    else
    {
        uDopProcDpuCfg.isFirstTimeCfg = false;
    }

    hwRes = &uDopProcDpuCfg.hwRes;
    uDopStaticCfg = &uDopProcDpuCfg.staticCfg;
    edmaCfg = &hwRes->edmaCfg;
    hwaCfg = &hwRes->hwaCfg;

    if (!(gMmwMssMCB.trackerCfg.staticCfg.trackerEnabled & gMmwMssMCB.enableMajorMotion))
    {
        /*Requires tracker and major motion to be enabled.*/
        retVal = DPC_OBJECTDETECTION_EINVAL_CFG;
        goto exit;
    }
    uDopStaticCfg->numAntRow = gMmwMssMCB.numAntRow;
    uDopStaticCfg->numAntCol = gMmwMssMCB.numAntCol;
    uDopStaticCfg->angleDimension = gMmwMssMCB.angleDimension;

    uDopStaticCfg->numDopplerChirps   = gMmwMssMCB.frameCfg.h_NumOfBurstsInFrame * gMmwMssMCB.frameCfg.h_NumOfChirpsInBurst / gMmwMssMCB.numTxAntennas;
    uDopStaticCfg->numDopplerBins     = mathUtils_pow2roundup(uDopStaticCfg->numDopplerChirps);
    uDopStaticCfg->numRangeBins       = gMmwMssMCB.numRangeBins;
    uDopStaticCfg->numRxAntennas      = gMmwMssMCB.numRxAntennas;
    uDopStaticCfg->numVirtualAntennas = gMmwMssMCB.numRxAntennas * gMmwMssMCB.numTxAntennas;
    uDopStaticCfg->log2NumDopplerBins = mathUtils_ceilLog2(uDopStaticCfg->numDopplerBins);;
    uDopStaticCfg->numTxAntennas      = gMmwMssMCB.numTxAntennas;
    uDopStaticCfg->maxNumTracks       = gMmwMssMCB.trackerCfg.staticCfg.gtrackModuleConfig.maxNumTracks;

    adcStart                        =   (gMmwMssMCB.adcStartTime * 1.e-6);
    startFreq                       =   (float)(gMmwMssMCB.startFreq * 1.e9);
    slope                           =   (float)(gMmwMssMCB.chirpSlope * 1.e12);
    bandwidth                       =   (slope * gMmwMssMCB.profileComCfg.h_NumOfAdcSamples)/(gMmwMssMCB.adcSamplingRate * 1.e6);
    centerFreq                      =   startFreq + bandwidth * 0.5f + adcStart * slope;

    uDopStaticCfg->rangeStep          = (MMWDEMO_RFPARSER_SPEED_OF_LIGHT_IN_METERS_PER_SEC * (gMmwMssMCB.adcSamplingRate * 1.e6)) /
                                        (2.f * slope * (2*gMmwMssMCB.numRangeBins));

    if (gMmwMssMCB.frameCfg.h_NumOfBurstsInFrame > 1)
    {
        /* Burst mode: h_NumOfBurstsInFrame > 1, h_NumOfChirpsInBurst = 2 */
        uDopStaticCfg->dopplerStep          =   MMWDEMO_RFPARSER_SPEED_OF_LIGHT_IN_METERS_PER_SEC /
                                            (2.f * uDopStaticCfg->numDopplerBins *
                                            centerFreq * (gMmwMssMCB.burstPeriod * 1e-6));
    }
    else
    {
        /* Normal mode: h_NumOfBurstsInFrame = 1, h_NumOfChirpsInBurst >= 2 */
        uDopStaticCfg->dopplerStep          =   MMWDEMO_RFPARSER_SPEED_OF_LIGHT_IN_METERS_PER_SEC /
                                            (2.f * gMmwMssMCB.frameCfg.h_NumOfChirpsInBurst *
                                            centerFreq * ((gMmwMssMCB.profileTimeCfg.h_ChirpIdleTime + gMmwMssMCB.profileComCfg.h_ChirpRampEndTime) * 1e-1 * 1e-6));
    }

    uDopStaticCfg->angleDimension        = gMmwMssMCB.angleDimension;
    uDopStaticCfg->isDetMatrixLogScale   = false;
    uDopStaticCfg->azimuthFftSize        = gMmwMssMCB.sigProcChainCfg.azimuthFftSize;
    uDopStaticCfg->elevationFftSize        = gMmwMssMCB.sigProcChainCfg.elevationFftSize;
    uDopStaticCfg->isStaticClutterRemovalEnabled = gMmwMssMCB.staticClutterRemovalEnable;
    uDopStaticCfg->isRxChGainPhaseCompensationEnabled   = true;

    /* CLI configuration */
    uDopStaticCfg->cliCfg = gMmwMssMCB.microDopplerCliCfg;
    uDopStaticCfg->microDopplerClassifierCliCfg  = gMmwMssMCB.microDopplerClassifierCliCfg;

    uDopStaticCfg->maxNumAzimAccumBins = DPU_UDOPPROC_MAX_NUM_AZIMUTH_ACCUM_BINS;

    /* hwRes - copy these structures */
    hwRes->radarCube = gMmwMssMCB.radarCube[0];

    /* hwRes - micro Doppler output array */
    hwRes->uDopplerHwaOutput = DPC_ObjDet_MemPoolAlloc(&gMmwMssMCB.CoreLocalRamObj,
                                                       uDopStaticCfg->numDopplerBins * sizeof(uint32_t) * 2, //allocate 2 buffers (for ping/pong)
                                                       sizeof(uint32_t));
    if (hwRes->uDopplerHwaOutput == NULL)
    {
        retVal = DPC_OBJECTDETECTION_ENOMEM__MICRO_DOPPLER_BUFFER;
        goto exit;
    }

    gMmwMssMCB.uDopProcOutParams.uDopplerOutput = (float *) DPC_ObjDet_MemPoolAlloc(&gMmwMssMCB.CoreLocalRamObj,
                                                                                    uDopStaticCfg->numDopplerBins * sizeof(float) * TRACKER_MAX_NUM_TR,
                                                                                    sizeof(float));
    if (gMmwMssMCB.uDopProcOutParams.uDopplerOutput == NULL)
    {
        retVal = DPC_OBJECTDETECTION_ENOMEM__MICRO_DOPPLER_BUFFER;
        goto exit;
    }

    gMmwMssMCB.uDopProcOutParams.uDopplerFeatures = (FeatExtract_featOutput *) DPC_ObjDet_MemPoolAlloc(&gMmwMssMCB.CoreLocalRamObj,
                                                                                                       sizeof(FeatExtract_featOutput) * TRACKER_MAX_NUM_TR,
                                                                                                       sizeof(float));
    if (gMmwMssMCB.uDopProcOutParams.uDopplerFeatures == NULL)
    {
        retVal = DPC_OBJECTDETECTION_ENOMEM__MICRO_DOPPLER_FEATURES;
        goto exit;
    }


    /* hwRes - edmaCfg */
    edmaCfg->edmaHandle = gEdmaHandle[0];

    /* edma to reset accumulating micro doppler per range bain */
    edmaCfg->edmaResetIn.channel =           DPC_OBJDET_DPU_UDOP_PROC_EDMA_RESET_CH;
    edmaCfg->edmaResetIn.channelShadow =     DPC_OBJDET_DPU_UDOP_PROC_EDMA_RESET_SHADOW;
    edmaCfg->edmaResetIn.eventQueue =        DPC_OBJDET_DPU_UDOP_PROC_EDMARESET_EVENT_QUE;

    /* edmaIn - to transfer one range bin from radar cube matrix to HWA */
    edmaCfg->edmaIn.channel =           DPC_OBJDET_DPU_UDOP_PROC_EDMAIN_CH;
    edmaCfg->edmaIn.channelShadow =     DPC_OBJDET_DPU_UDOP_PROC_EDMAIN_SHADOW;
    edmaCfg->edmaIn.eventQueue =        DPC_OBJDET_DPU_UDOP_PROC_EDMAIN_EVENT_QUE;

    /* edmaHotSig */
    edmaCfg->edmaHotSig.channel =             DPC_OBJDET_DPU_UDOP_PROC_EDMAIN_SIG_CH;
    edmaCfg->edmaHotSig.channelShadow =       DPC_OBJDET_DPU_UDOP_PROC_EDMAIN_SIG_SHADOW;
    edmaCfg->edmaHotSig.eventQueue =          DPC_OBJDET_DPU_UDOP_PROC_EDMAIN_SIG_EVENT_QUE;

    /* edmaChainOut - loop back and in last iteration chain to output buffer */
    edmaCfg->edmaChainOut.channel =           DPC_OBJDET_DPU_UDOP_PROC_EDMAOUT_CHAIN_CH;
    edmaCfg->edmaChainOut.ShadowPramId[0] =  DPC_OBJDET_DPU_UDOP_PROC_EDMAOUT_CHAIN0_SHADOW;
    edmaCfg->edmaChainOut.ShadowPramId[1] =  DPC_OBJDET_DPU_UDOP_PROC_EDMAOUT_CHAIN1_SHADOW;
    edmaCfg->edmaChainOut.eventQueue =        DPC_OBJDET_DPU_UDOP_PROC_EDMAOUT_CHAIN_EVENT_QUE;

    /* edmaOut - transfer Micro Doppler from HWA to output ping/pong buffer */
    edmaCfg->edmaMicroDopOut.channel =       DPC_OBJDET_DPU_UDOP_PROC_EDMAOUT_UDOPPLER_CH;
    edmaCfg->edmaMicroDopOut.channelShadow = DPC_OBJDET_DPU_UDOP_PROC_EDMAOUT_UDOPPLER_SHADOW;
    edmaCfg->edmaMicroDopOut.eventQueue =    DPC_OBJDET_DPU_UDOP_PROC_EDMAOUT_UDOPPLER_EVENT_QUE;

    edmaCfg->intrObj = &intrObj_uDopProc;

    if (gMmwMssMCB.steeringVecCorrCfg.enableAntSymbGen)
    {
        hwaCfg->paramSetStartIdx = DPU_RANGEPROCHWA_NUM_HWA_PARAM_SETS +
                                    DPU_DOAPROC_MAX_NUM_HWA_PARAMSET +
                                    DPU_CFARPROCHWA_MAX_NUM_HWA_PARAMSET +
                                    DPU_AOASVCPROC_MAX_NUM_HWA_PARAMSET;
    }
    else
    {
        hwaCfg->paramSetStartIdx = DPU_RANGEPROCHWA_NUM_HWA_PARAM_SETS +
                                    DPU_DOAPROC_MAX_NUM_HWA_PARAMSET +
                                    DPU_CFARPROCHWA_MAX_NUM_HWA_PARAMSET;
    }
    hwaCfg->dmaTrigSrcChan = DPC_ObjDet_HwaDmaTrigSrcChanPoolAlloc(&gMmwMssMCB.HwaDmaChanPoolObj);

    /* Rx compensation coefficients */
    {
        int32_t rxInd, txInd;
        int32_t ind = 0;
        uDopStaticCfg->compRxChanCfg.rangeBias = gMmwMssMCB.compRxChannelBiasCfg.rangeBias;
        for (txInd = 0; txInd < uDopStaticCfg->numTxAntennas; txInd++)
        {
            for (rxInd = 0; rxInd < uDopStaticCfg->numRxAntennas; rxInd++)
            {
                uDopStaticCfg->compRxChanCfg.rxChPhaseComp[ind++] = gMmwMssMCB.compRxChannelBiasCfg.rxChPhaseComp[gMmwMssMCB.rxAntOrder[rxInd] + (txInd * SYS_COMMON_NUM_RX_CHANNEL)];
            }
        }
    }

    /* Calculate Doppler+mapping table */
    retVal = MmwDemo_cfgDopplerParamMapping(&hwRes->doaRngGateCfg, DOPPLER_OUTPUT_MAPPING_ROW_DOP_COL);
    if (retVal < 0)
    {
        goto exit;
    }

    hwRes->scratchBuf.sizeBytes = (uDopStaticCfg->numDopplerBins + 1) * sizeof(float);

    if (uDopStaticCfg->microDopplerClassifierCliCfg.enabled)
    {
        /* Classifier on the target*/
        uint32_t classifierScratchBuffSizeBytes = classifier_bytes_needed();
        if (classifierScratchBuffSizeBytes > hwRes->scratchBuf.sizeBytes)
        {
            hwRes->scratchBuf.sizeBytes = classifierScratchBuffSizeBytes;
        }
    }


    //Shared between feature extraction and classifier
    hwRes->scratchBuf.data = DPC_ObjDet_MemPoolAlloc(&gMmwMssMCB.CoreLocalRamObj,
                                                     hwRes->scratchBuf.sizeBytes,
                                                     sizeof(float));
    if (hwRes->scratchBuf.data == NULL)
    {
        retVal = DPC_OBJECTDETECTION_ENOMEM__MICRO_DOPPLER_FEATURES;
        goto exit;
    }

    if (uDopStaticCfg->microDopplerClassifierCliCfg.enabled)
    {
        /* Memory for feature delay line - retained acreoss frames */
        hwRes->featureObj = DPC_ObjDet_MemPoolAlloc(&gMmwMssMCB.CoreLocalRamObj,
                                                    sizeof(DPU_uDopProc_FeatureObj) * gMmwMssMCB.trackerCfg.staticCfg.gtrackModuleConfig.maxNumTracks,
                                                    sizeof(float));
        if (hwRes->featureObj == NULL)
        {
            retVal = DPC_OBJECTDETECTION_ENOMEM__MICRO_DOPPLER_FEATURES;
            goto exit;
        }

        /* Scratch buffer for linearized feature set, input to classifier */
        hwRes->scratchBuf2.sizeBytes = CLASSIFIER_NUM_FRAMES * CLASSIFIER_NUM_FEATURES * sizeof(float);
        hwRes->scratchBuf2.data = DPC_ObjDet_MemPoolAlloc(&gMmwMssMCB.CoreLocalRamObj,
                                                          hwRes->scratchBuf2.sizeBytes,
                                                          sizeof(float));
        if (hwRes->scratchBuf2.data == NULL)
        {
            retVal = DPC_OBJECTDETECTION_ENOMEM__MICRO_DOPPLER_FEATURES;
            goto exit;
        }
    }

exit:
return retVal;
}

/**
*  @b Description
*  @n
*    Based on the configuration, set up the CFAR processing DPU configurations
*/
int32_t CfarProc_configParser()
{
    int32_t retVal = 0;
    float adcStart, startFreq, slope, bandwidth, centerFreq;
    DPU_CFARProcHWA_HW_Resources *pHwConfig;
    DPU_CFARProcHWA_StaticConfig  *params;

    /* CFARproc DPU */
    pHwConfig = &cfarProcDpuCfg.res;
    params = &cfarProcDpuCfg.staticCfg;

    memset((void *)&cfarProcDpuCfg, 0, sizeof(DPU_CFARProcHWA_Config));

    /* HWA configurations, not related to per test, common to all test */
    pHwConfig->hwaCfg.paramSetStartIdx = DPU_DOAPROC_MAX_NUM_HWA_PARAMSET + DPU_RANGEPROCHWA_NUM_HWA_PARAM_SETS;
    pHwConfig->hwaCfg.numParamSet = DPU_CFARPROCHWA_MAX_NUM_HWA_PARAMSET;
    pHwConfig->hwaCfg.dmaTrigSrcChan = DPC_ObjDet_HwaDmaTrigSrcChanPoolAlloc(&gMmwMssMCB.HwaDmaChanPoolObj);

    /* edma configuration */
    pHwConfig->edmaHandle  = gEdmaHandle[0];

    /* Data Input EDMA */
    pHwConfig->edmaHwaIn.channel         = DPC_OBJDET_DPU_CFAR_PROC_EDMAIN_CH;
    pHwConfig->edmaHwaIn.channelShadow   = DPC_OBJDET_DPU_CFAR_PROC_EDMAIN_SHADOW;
    pHwConfig->edmaHwaIn.eventQueue      = DPC_OBJDET_DPU_CFAR_PROC_EDMAIN_EVENT_QUE;
    pHwConfig->edmaHwaInSignature.channel         = DPC_OBJDET_DPU_CFAR_PROC_EDMAIN_SIG_CH;
    pHwConfig->edmaHwaInSignature.channelShadow   = DPC_OBJDET_DPU_CFAR_PROC_EDMAIN_SIG_SHADOW;
    pHwConfig->edmaHwaInSignature.eventQueue      = DPC_OBJDET_DPU_CFAR_PROC_EDMAIN_SIG_EVENT_QUE;
    pHwConfig->intrObj = &intrObj_cfarProc;

    /* Data Output EDMA */
    pHwConfig->edmaHwaOut.channel         = DPC_OBJDET_DPU_CFAR_PROC_EDMAOUT_RNG_PROFILE_CH;
    pHwConfig->edmaHwaOut.channelShadow   = DPC_OBJDET_DPU_CFAR_PROC_EDMAOUT_RNG_PROFILE_SHADOW;
    pHwConfig->edmaHwaOut.eventQueue      = DPC_OBJDET_DPU_CFAR_PROC_EDMAOUT_RNG_PROFILE_EVENT_QUE;

    /* Give M0 and M1 memory banks for detection matrix scratch. */
    pHwConfig->hwaMemInp = (uint16_t *) CSL_APP_HWA_DMA0_RAM_BANK0_BASE;
    pHwConfig->hwaMemInpSize = (CSL_APP_HWA_BANK_SIZE * 2) / sizeof(uint16_t);

    /* M2 bank: for CFAR detection list */
    pHwConfig->hwaMemOutDetList = (DPU_CFARProcHWA_CfarDetOutput *) CSL_APP_HWA_DMA0_RAM_BANK2_BASE;
    pHwConfig->hwaMemOutDetListSize = CSL_APP_HWA_BANK_SIZE /
                                sizeof(DPU_CFARProcHWA_CfarDetOutput);

    /* M3 bank: for maximum azimuth values per range bin  (range profile) */
    pHwConfig->hwaMemOutRangeProfile = (DPU_CFARProcHWA_HwaMaxOutput *) CSL_APP_HWA_DMA0_RAM_BANK3_BASE;

    /* dynamic config */
    cfarProcDpuCfg.dynCfg.cfarCfg   = &gMmwMssMCB.cfarCfg;
    cfarProcDpuCfg.dynCfg.fovRange  = &gMmwMssMCB.rangeSelCfg;
    cfarProcDpuCfg.dynCfg.fovAoaCfg    = &gMmwMssMCB.fovCfg;

    /* DPU Static config */
    params->detectionHeatmapType = DPU_CFAR_RANGE_AZIMUTH_HEATMAP;
    params->numRangeBins = gMmwMssMCB.numRangeBins;
    params->numDopplerBins     = gMmwMssMCB.numDopplerBins;
    params->log2NumDopplerBins = mathUtils_ceilLog2(params->numDopplerBins);

    params->selectCoherentPeakInDopplerDim = gMmwMssMCB.sigProcChainCfg.coherentDoppler;
    params->angleDimension        = gMmwMssMCB.angleDimension;
    params->isDetMatrixLogScale   = false;
    params->azimuthFftSize        = gMmwMssMCB.sigProcChainCfg.azimuthFftSize;
    params->elevationFftSize      = gMmwMssMCB.sigProcChainCfg.elevationFftSize;
    params->isStaticClutterRemovalEnabled = gMmwMssMCB.staticClutterRemovalEnable;
    params->dopElevDimReductOrder = gMmwMssMCB.sigProcChainCfg.dopElevDimReductOrder;

    if (params->isDetMatrixLogScale)
    {
        gMmwMssMCB.cfarCfg.thresholdScale    = (uint32_t) lroundf(gMmwMssMCB.cfarCfg.threshold_dB * 2048.0 / (log10(2)*20));
    }
    else
    {
        gMmwMssMCB.cfarCfg.thresholdScale = (uint32_t) lroundf(pow(10, gMmwMssMCB.cfarCfg.threshold_dB/20.0) * 16.0);
    }
    
    adcStart                        =   (gMmwMssMCB.adcStartTime * 1.e-6);
    startFreq                       =   (float)(gMmwMssMCB.startFreq * 1.e9);
    slope                           =   (float)(gMmwMssMCB.chirpSlope * 1.e12);
    bandwidth                       =   (slope * gMmwMssMCB.profileComCfg.h_NumOfAdcSamples)/(gMmwMssMCB.adcSamplingRate * 1.e6);
    centerFreq                      =   startFreq + bandwidth * 0.5f + adcStart * slope;

    params->rangeStep            =   (MMWDEMO_RFPARSER_SPEED_OF_LIGHT_IN_METERS_PER_SEC * (gMmwMssMCB.adcSamplingRate * 1.e6)) /
                                        (2.f * slope * (2*params->numRangeBins));
    /*outParams->rangeResolution      =   (MMWDEMO_RFPARSER_SPEED_OF_LIGHT_IN_METERS_PER_SEC * gMmwMssMCB.adcSamplingRate * 1e6) /
                                        (2.f * slope * gMmwMssMCB.profileComCfg.h_NumOfAdcSamples);*/

    gMmwMssMCB.rangeStep           = params->rangeStep;
    
    if (gMmwMssMCB.frameCfg.h_NumOfBurstsInFrame > 1)
    {
        /* Burst mode: h_NumOfBurstsInFrame > 1, h_NumOfChirpsInBurst = 2 */
        params->dopplerStep          =   MMWDEMO_RFPARSER_SPEED_OF_LIGHT_IN_METERS_PER_SEC /
                                            (2.f * params->numDopplerBins *
                                            centerFreq * (gMmwMssMCB.burstPeriod * 1e-6));
    }
    else
    {
        /* Normal mode: h_NumOfBurstsInFrame = 1, h_NumOfChirpsInBurst >= 2 */
        params->dopplerStep          =   MMWDEMO_RFPARSER_SPEED_OF_LIGHT_IN_METERS_PER_SEC /
                                            (2.f * gMmwMssMCB.frameCfg.h_NumOfChirpsInBurst *
                                            centerFreq * ((gMmwMssMCB.profileTimeCfg.h_ChirpIdleTime + gMmwMssMCB.profileComCfg.h_ChirpRampEndTime) * 1e-1 * 1e-6));                         
        if(gMmwMssMCB.frameCfg.c_NumOfChirpsAccum != 0)
        {
            /* When numOfChirpsAccum is greater than zero, the chirping window will increase acccording to numOfChirpsAccum selected. */ 
            params->dopplerStep = params->dopplerStep/gMmwMssMCB.frameCfg.c_NumOfChirpsAccum;
        }
    }
    /*outParams->dopplerResolution    =   MMWDEMO_RFPARSER_SPEED_OF_LIGHT_IN_METERS_PER_SEC /
                                        (2.f * gMmwMssMCB.frameCfg.h_NumOfBurstsInFrame * centerFreq * (gMmwMssMCB.frameCfg.w_BurstPeriodicity));*/

    if (gMmwMssMCB.antennaGeometryCfg.antDistanceXdim == 0.)
    {
        params->lambdaOverDistX = 2.0;
    }
    else
    {
        params->lambdaOverDistX = 3e8 / (centerFreq * gMmwMssMCB.antennaGeometryCfg.antDistanceXdim);
    }

    if (gMmwMssMCB.antennaGeometryCfg.antDistanceZdim == 0.)
    {
        params->lambdaOverDistZ = 2.0;
    }
    else
    {
        params->lambdaOverDistZ = 3e8 / (centerFreq * gMmwMssMCB.antennaGeometryCfg.antDistanceZdim);
    }

    /* Range bias (m) */
    params->rangeBias = gMmwMssMCB.compRxChannelBiasCfg.rangeBias;

    #if (CLI_REMOVAL == 0)
    if(gMmwMssMCB.adcDataSourceCfg.source == 1)
    {
        //ADC data from file, populate point cloud list with target indices (range/azimuth/elevation/doppler)
        params->enableCfarPointCloudListWithIndices = true;
    }
    else
    {
        params->enableCfarPointCloudListWithIndices = false;
    }
    #else
    params->enableCfarPointCloudListWithIndices = false;
    #endif

    params->enableCfarPointCloudListWithIndices = true;

    /* hwres config - Copy these structures */
    pHwConfig->dopplerIndexMatrix = gMmwMssMCB.dopplerIndexMatrix;
    pHwConfig->elevationIndexMatrix = gMmwMssMCB.elevationIndexMatrix;

    /* For point cloud compression over UART: set the cloud point units and reciprocal values */
    gMmwMssMCB.pointCloudToUart.pointUint.xyzUnit = (params->rangeStep * params->numRangeBins) / 32768.0;
    gMmwMssMCB.pointCloudToUart.pointUint.dopplerUnit = (params->dopplerStep * params->numDopplerBins/2) / 32768.0;
    gMmwMssMCB.pointCloudToUart.pointUint.snrUint = 0.25;
    gMmwMssMCB.pointCloudToUart.pointUint.noiseUint = 1.0;
    gMmwMssMCB.pointCloudUintRecip.xyzUnit = 1. / gMmwMssMCB.pointCloudToUart.pointUint.xyzUnit;
    gMmwMssMCB.pointCloudUintRecip.dopplerUnit = 1. / gMmwMssMCB.pointCloudToUart.pointUint.dopplerUnit;
    gMmwMssMCB.pointCloudUintRecip.snrUint = 1. / gMmwMssMCB.pointCloudToUart.pointUint.snrUint;  //scale 0.1 since in the CFAR DPU structure it is in 0.1 dB
    gMmwMssMCB.pointCloudUintRecip.noiseUint = 1. / gMmwMssMCB.pointCloudToUart.pointUint.noiseUint;  //scale 0.1 since in the CFAR DPU structure it is in 0.1 dB


    if(gMmwMssMCB.enableMajorMotion)
    {
        /* In major motion CFAR post processing, the velocity inclusion threshold is set to maximum, so all points are includeed and their detected velocity is not modified */
        gMmwMssMCB.cfarRunTimeInputParams[0].forceVelocityToZero = false;
        gMmwMssMCB.cfarRunTimeInputParams[0].velocityInclusionThr = params->dopplerStep * params->numDopplerBins/2; //Maximum velocity, include all
    }
    if(gMmwMssMCB.enableMinorMotion)
    {
        /* In minor motion CFAR post processing, the detected points with radial velocity greater than minorMotionVelocityInclusionThr are excluded from the list, 
           and then velocity  of included points is forced to zero if the flag forceMinorMotionVelocityToZero is set to true. */
        gMmwMssMCB.cfarRunTimeInputParams[1].forceVelocityToZero = gMmwMssMCB.sigProcChainCfg.forceMinorMotionVelocityToZero;
        gMmwMssMCB.cfarRunTimeInputParams[1].velocityInclusionThr = gMmwMssMCB.sigProcChainCfg.minorMotionVelocityInclusionThr;
    }

    if (rangeCompCfg.enabled == 1 && gMmwMssMCB.stats.frameStartIntCounter == 0)
    {
        // If no range/SNR values are specified, then default to the maximum range index and detection SNR
        if (rangeCompCfg.detectionRangeIdx < 0)
        {
            // max range index allowed is the number of samples / 2 * RANGE_FFT_ANTI_ALIAS_SAFETY_FACTOR
            rangeCompCfg.detectionRangeIdx = gMmwMssMCB.profileComCfg.h_NumOfAdcSamples * 0.5 * RANGE_FFT_ANTI_ALIAS_SAFETY_FACTOR;
            // inverse operation to get the detection SNR specified in the configuration file
            rangeCompCfg.detectionSNR = log10(((((float)gMmwMssMCB.cfarCfg.thresholdScale) - 0.5) / 16.0)) * 20;
        }
        // If the range/SNR are specified, then convert the range from meters to index. detection SNR is already set from mmw_cli.c
        else
        {
            rangeCompCfg.detectionRangeIdx = roundf(rangeCompCfg.detectionRangeIdx / params->rangeStep);
            rangeCompCfg.minCompRange      = roundf(rangeCompCfg.minCompRange / params->rangeStep);
            rangeCompCfg.maxCompRange      = roundf(rangeCompCfg.maxCompRange / params->rangeStep);
        }
    }

    return retVal;
}

/**
*  @b Description
*  @n
*    Based on the configuration, set up the MPD processing DPU configurations
*/
int32_t MpdProc_configParser()
{
    int32_t retVal = 0;
    int16_t i = 0;
    DPU_MpdProc_HW_Resources *pHwConfig;
    DPU_MpdProc_StaticConfig *params;

    /* CFARproc DPU */
    pHwConfig = &mpdProcDpuCfg.res;
    params = &mpdProcDpuCfg.staticCfg;

    memset((void *)&mpdProcDpuCfg, 0, sizeof(DPU_MpdProc_Config));
    
    pHwConfig->zones = gMmwMssMCB.dpcZones;

    /* Initializing the zones structure based on CLI config. CAUTION: Accumulates history across frames */
    if (!gMmwMssMCB.oneTimeConfigDone)
    {
        memset((void *)pHwConfig->zones, 0, (gMmwMssMCB.sceneryParams.numBoundaryBoxes * sizeof(mpdProc_MotionTracker)));

        for(i = 0;i < gMmwMssMCB.sceneryParams.numBoundaryBoxes;i++)
        {
            pHwConfig->zones[i].pointHistBufferMajor.bufferSize = gMmwMssMCB.majorStateParamCfg.histBufferSize;
            pHwConfig->zones[i].pointHistBufferMajor.oldest = gMmwMssMCB.majorStateParamCfg.histBufferSize - 1;
            
            pHwConfig->zones[i].snrHistBufferMajor.bufferSize = gMmwMssMCB.majorStateParamCfg.histBufferSize;
            pHwConfig->zones[i].snrHistBufferMajor.oldest = gMmwMssMCB.majorStateParamCfg.histBufferSize - 1;
            
            pHwConfig->zones[i].pointHistBufferMinor.bufferSize = gMmwMssMCB.minorStateParamCfg.histBufferSize;
            pHwConfig->zones[i].pointHistBufferMinor.oldest = gMmwMssMCB.minorStateParamCfg.histBufferSize - 1;

            pHwConfig->zones[i].snrHistBufferMinor.bufferSize = gMmwMssMCB.minorStateParamCfg.histBufferSize;
            pHwConfig->zones[i].snrHistBufferMinor.oldest = gMmwMssMCB.minorStateParamCfg.histBufferSize - 1;
        }
    }

    pHwConfig->numDetMajor = (uint16_t *)&gMmwMssMCB.dpcResult.numObjOutMajor;
    pHwConfig->numDetMinor = (uint16_t *)&gMmwMssMCB.dpcResult.numObjOutMinor;
    pHwConfig->detObjMajor = (DPIF_PointCloudCartesianExt *)&gMmwMssMCB.cfarDetObjOut[0];
    pHwConfig->detObjMinor = (DPIF_PointCloudCartesianExt *)&gMmwMssMCB.cfarDetObjOut[0];
    
    params->motionMode = (uint8_t)gMmwMssMCB.sigProcChainCfg.motDetMode;
    
    memcpy(&params->sceneryParams, &gMmwMssMCB.sceneryParams, sizeof(mpdProc_SceneryParams));
    memcpy(&params->clusterParams, &gMmwMssMCB.clusterParamCfg, sizeof(mpdProc_ClusterParamCfg));
    memcpy(&params->majorStateParamCfg, &gMmwMssMCB.majorStateParamCfg, sizeof(mpdProc_MotionModeStateParamCfg));
    memcpy(&params->minorStateParamCfg, &gMmwMssMCB.minorStateParamCfg, sizeof(mpdProc_MotionModeStateParamCfg));

    return retVal;
}

/**
*  @b Description
*  @n
*        Function configuring range processing DPU
*/
void mmwDemo_rangeProcConfig()
{
    int32_t retVal = 0;

    retVal = RangeProc_configParser();
    if (retVal < 0)
    {
        CLI_write("Error in setting up range profile:%d \n", retVal);
        DebugP_assert(0);
    }

    retVal = DPU_RangeProcHWA_config(gMmwMssMCB.rangeProcDpuHandle, &rangeProcDpuCfg);
    if (retVal < 0)
    {
        CLI_write("Error: RANGE DPU config return error:%d \n", retVal);
        DebugP_assert(0);
    }
}

/**
*  @b Description
*  @n
*        Function configuring DOA DPU
*/
void mmwDemo_doaProcConfig()
{
    //uint32_t i;
    int32_t retVal = 0;
    uint32_t numSamples;
    DPU_DoaProc_HW_Resources  *hwRes;

    hwRes = &doaProcDpuCfg.hwRes;

    retVal = DoaProc_configParser();
    if (retVal < 0)
    {
        CLI_write("Error: Error in setting up doa profile:%d \n", retVal);
        DebugP_assert(0);
    }

    /* Note: chunk[1] is not used */
    /* Major motion - set input source and destination address */
    numSamples = doaProcDpuCfg.staticCfg.numVirtualAntennas * doaProcDpuCfg.staticCfg.numDopplerChirps;
    gMmwMssMCB.radarCubeSrc[MMW_DEMO_MAJOR_MODE].chunk[0].srcAddress = (uint32_t) gMmwMssMCB.radarCube[0].data;
    gMmwMssMCB.radarCubeSrc[MMW_DEMO_MAJOR_MODE].chunk[0].dstAddress = hwRes->hwaCfg.hwaMemInpAddr;
    gMmwMssMCB.radarCubeSrc[MMW_DEMO_MAJOR_MODE].chunk[0].Bcnt_Acnt = (numSamples << 16)   | sizeof(cmplx16ImRe_t);

    /* Minor Motion set input source and destination address */
    numSamples = doaProcDpuCfg.staticCfg.numVirtualAntennas * doaProcDpuCfg.staticCfg.numDopplerChirps;
    gMmwMssMCB.radarCubeSrc[MMW_DEMO_MINOR_MODE].chunk[0].srcAddress = (uint32_t) gMmwMssMCB.radarCube[1].data;
    gMmwMssMCB.radarCubeSrc[MMW_DEMO_MINOR_MODE].chunk[0].dstAddress = hwRes->hwaCfg.hwaMemInpAddr;
    gMmwMssMCB.radarCubeSrc[MMW_DEMO_MINOR_MODE].chunk[0].Bcnt_Acnt = (numSamples << 16)   | sizeof(cmplx16ImRe_t);

    retVal = DPU_DoaProc_config (gMmwMssMCB.doaProcDpuHandle, &doaProcDpuCfg);
    if (retVal < 0)
    {
        CLI_write("DOA DPU config return error:%d \n", retVal);
        DebugP_assert(0);
    }

}

/**
*  @b Description
*  @n
*        Function configuring AOASVC DPU
*/
void mmwDemo_aoasvcProcConfig()
{
    int32_t retVal = 0;

    retVal = AoasvcProc_configParser();
    if (retVal < 0)
    {
        CLI_write("Error: Error in setting up doa profile:%d \n", retVal);
        DebugP_assert(0);
    }

    retVal = DPU_AoasvcProc_config (gMmwMssMCB.aoasvcProcDpuHandle, &aoasvcProcDpuCfg);
    if (retVal < 0)
    {
        CLI_write("AOASVC DPU config return error:%d \n", retVal);
        DebugP_assert(0);
    }

}

/**
 *  @b Description
 *  @n
*        Function configuring Micro Doppler DPU
*/
void mmwDemo_uDopProcConfig()
{
    int32_t retVal = 0;
    retVal = uDopProc_configParser();
    if (retVal < 0)
    {
        CLI_write("Error in setting up micro Doppler profile:%d \n", retVal);
        DebugP_assert(0);
    }

    retVal = DPU_uDopProc_config(gMmwMssMCB.microDopDpuHandle, &uDopProcDpuCfg);
    if (retVal < 0)
    {
        CLI_write("Micro Doppler DPU config return error:%d \n", retVal);
        DebugP_assert(0);
    }
}

/**
*  @b Description
*  @n
*        Function configuring CFAR DPU
*/
void mmwDemo_cfarProcConfig()
{
    int32_t retVal = 0;

    retVal = CfarProc_configParser();
    if (retVal < 0)
    {
        CLI_write("Error in setting up CFAR profile:%d \n", retVal);
        DebugP_assert(0);
    }

    retVal = DPU_CFARProcHWA_config(gMmwMssMCB.cfarProcDpuHandle, &cfarProcDpuCfg);
    if (retVal < 0)
    {
        CLI_write("CFAR DPU config return error:%d \n", retVal);
        DebugP_assert(0);
    }
}

/**
 *  @b Description
 *  @n
*        Function configuring MPD DPU
*/
void mmwDemo_mpdProcConfig()
{
    int32_t retVal = 0;
    retVal = MpdProc_configParser();
    if (retVal < 0)
    {
        CLI_write("Error in setting up MPD profile:%d \n", retVal);
        DebugP_assert(0);
    }

    retVal = DPU_MpdProc_config(gMmwMssMCB.mpdProcDpuHandle, &mpdProcDpuCfg);
    if (retVal < 0)
    {
        CLI_write("MPD DPU config return error:%d \n", retVal);
        DebugP_assert(0);
    }
}

/**
*  @b Description
*  @n
 *      The function is used to configure the tracker DPU.
 *
 */
void mmwDemo_trackerConfig (void)
{
    int32_t    retVal=0;

    /* Fill sensor position */
    MmwDemo_FillTrackerSensorPositionCfg();

    retVal = DPU_TrackerProc_config(gMmwMssMCB.trackerProcDpuHandle, &gMmwMssMCB.trackerCfg);

    if (retVal < 0)
    {
        CLI_write("Tracker DPU config return error:%d \n", retVal);
        DebugP_assert(0);
    }
}

/*For debugging purposes*/
#if 0
volatile int16_t gTest1Tx[16][4];
volatile uint32_t gTest1TxCntr=0;
#endif

/**
*  @b Description
*  @n
*        Function initiliazing all indvidual DPUs
*/
void DPC_Init()
{
    /* hwa, edma, and DPU initialization*/

    /* Register Frame Start Interrupt */
    if(mmwDemo_registerFrameStartInterrupt() != 0){
        CLI_write("Error: Failed to register frame start interrupts\n");
        DebugP_assert(0);
    }
/*For debugging purposes*/
#if 0
    if(mmwDemo_registerChirpAvailableInterrupts() != 0){
        CLI_write("Failed to register chirp available interrupts\n");
        DebugP_assert(0);
    }
    mmwDemo_registerChirpInterrupt();
    mmwDemo_registerBurstInterrupt();
#endif
    int32_t status = SystemP_SUCCESS;

    /* Shared memory pool */
    gMmwMssMCB.L3RamObj.cfg.addr = (void *)&gMmwL3[0];
    gMmwMssMCB.L3RamObj.cfg.size = sizeof(gMmwL3);

    /* Local memory pool */
    gMmwMssMCB.CoreLocalRamObj.cfg.addr = (void *)&gMmwCoreLocMem[0];
    gMmwMssMCB.CoreLocalRamObj.cfg.size = sizeof(gMmwCoreLocMem);

    /* Memory pool for the tracker */
    HeapP_construct(&gMmwMssMCB.CoreLocalTrackerHeapObj, (void *) gMmwCoreLocMem2, MMWDEMO_OBJDET_CORE_LOCAL_MEM2_SIZE);

    /* Memory pool for the feature extraction */
    featExtract_heapConstruct();

    hwaHandle = HWA_open(0, NULL, &status);
    if (hwaHandle == NULL)
    {
        CLI_write("Error: Unable to open the HWA Instance err:%d\n", status);
        DebugP_assert(0);
    }

    rangeProc_dpuInit();
    doaProc_dpuInit();
    aoasvcProc_dpuInit();
    cfarProc_dpuInit();

    #if (CLI_REMOVAL == 0 || MPD_ENABLE == 1)
    {
        mpdProc_dpuInit();
    }
    #endif

    #if (CLI_REMOVAL == 0 || TRACKER_CLASSIFIER_ENABLE == 1)
    {
        if (!gMmwMssMCB.oneTimeConfigDone)
        {
            udopProc_dpuInit();
            trackerProc_dpuInit();
        }
    }
    #endif
}

/**
*  @b Description
*  @n

*        Function configuring all DPUs
*/
void DPC_Config()
{

    int32_t retVal;

    /*TODO Cleanup: MMWLPSDK-237*/
    
    DPC_ObjDet_MemPoolReset(&gMmwMssMCB.L3RamObj);
    DPC_ObjDet_MemPoolReset(&gMmwMssMCB.CoreLocalRamObj);
    DPC_ObjDet_HwaDmaTrigSrcChanPoolReset(&gMmwMssMCB.HwaDmaChanPoolObj);


    /*Allocate memory for point cloud */
    gMmwMssMCB.cfarDetObjOut = (DPIF_PointCloudCartesianExt *) DPC_ObjDet_MemPoolAlloc(&gMmwMssMCB.L3RamObj,
                                                                                       MAX_NUM_DETECTIONS * sizeof(DPIF_PointCloudCartesianExt),
                                                                                       sizeof(uint32_t));
    if (gMmwMssMCB.cfarDetObjOut == NULL)
    {
        CLI_write("DPC configuration: memory allocation failed\n");
        DebugP_assert(0);
    }

    gMmwMssMCB.dpcObjOut = (DPIF_PointCloudCartesian *) DPC_ObjDet_MemPoolAlloc(&gMmwMssMCB.L3RamObj,
                                                                                       MAX_NUM_DETECTIONS * sizeof(DPIF_PointCloudCartesian),
                                                                                       sizeof(uint32_t));
    if (gMmwMssMCB.dpcObjOut == NULL)
    {
        CLI_write("DPC configuration: memory allocation failed\n");
        DebugP_assert(0);
    }

    gMmwMssMCB.dpcObjSideInfo = (DPIF_PointCloudSideInfo *) DPC_ObjDet_MemPoolAlloc(&gMmwMssMCB.L3RamObj,
                                                                                       MAX_NUM_DETECTIONS * sizeof(DPIF_PointCloudSideInfo),
                                                                                       sizeof(uint32_t));
    if (gMmwMssMCB.dpcObjSideInfo == NULL)
    {
        CLI_write("DPC configuration: memory allocation failed\n");
        DebugP_assert(0);
    }

        gMmwMssMCB.dpcObjIndOut = (DPIF_PointCloudRngAzimElevDopInd *) DPC_ObjDet_MemPoolAlloc(&gMmwMssMCB.L3RamObj,
                                                                                           MAX_NUM_DETECTIONS * sizeof(DPIF_PointCloudRngAzimElevDopInd),
                                                                                           sizeof(uint32_t));
        if (gMmwMssMCB.dpcObjIndOut == NULL)
        {
            CLI_write("DPC configuration: memory allocation failed\n");
            DebugP_assert(0);
        }

    if (gMmwMssMCB.steeringVecCorrCfg.enableAntSymbGen)
    {
        gMmwMssMCB.virtAntElemList = (DPU_AoasvcProc_VirtualAntennaElements *) DPC_ObjDet_MemPoolAlloc(&gMmwMssMCB.L3RamObj,
                                                                                                       MAX_NUM_DETECTIONS * sizeof(DPU_AoasvcProc_VirtualAntennaElements),
                                                                                                       sizeof(uint32_t));
        if (gMmwMssMCB.virtAntElemList == NULL)
        {
            CLI_write("DPC configuration: memory allocation failed\n");
            DebugP_assert(0);
        }
    }

    if(gMmwMssMCB.isMotionPresenceDpuEnabled)
    {
        /*Allocate memory for zone state array based on no of zones configured*/
        gMmwMssMCB.dpcZones = (mpdProc_MotionTracker *) DPC_ObjDet_MemPoolAlloc(&gMmwMssMCB.L3RamObj,
                                                                                    gMmwMssMCB.sceneryParams.numBoundaryBoxes * sizeof(mpdProc_MotionTracker),
                                                                                    sizeof(uint8_t));

        gMmwMssMCB.dpcZoneState = (uint8_t *) DPC_ObjDet_MemPoolAlloc(&gMmwMssMCB.L3RamObj,
                                                                        ((uint8_t)ceil(gMmwMssMCB.sceneryParams.numBoundaryBoxes/4.0) + 1) * sizeof(uint8_t),
                                                                        sizeof(uint8_t));
    }

    /* Configure DPUs */
    mmwDemo_rangeProcConfig();
    mmwDemo_doaProcConfig();

    /*
    if (gMmwMssMCB.steeringVecCorrCfg.enableAntSymbGen)
    {
        mmwDemo_aoasvcProcConfig();
    }
    */

    /*
    #if (CLI_REMOVAL == 0 || TRACKER_CLASSIFIER_ENABLE == 1)
    if(gMmwMssMCB.microDopplerCliCfg.enabled)
    {
        if (!gMmwMssMCB.trackerCfg.staticCfg.trackerEnabled)
        {
            CLI_write("DPC configuration: Micro Doppler DPU requires group tracker DPU to be enabled \n");
            DebugP_assert(0);
        }

    	mmwDemo_uDopProcConfig();
    }
    #endif
    */

    mmwDemo_cfarProcConfig();

    #if (CLI_REMOVAL == 0 || MPD_ENABLE == 1)
    {
        if(gMmwMssMCB.isMotionPresenceDpuEnabled)
        {
            mmwDemo_mpdProcConfig();
        }
    }
    #endif

    #if (CLI_REMOVAL == 0 || TRACKER_CLASSIFIER_ENABLE == 1)
    {
        if(gMmwMssMCB.trackerCfg.staticCfg.trackerEnabled)
        {
            if (!gMmwMssMCB.oneTimeConfigDone)
            {
                mmwDemo_trackerConfig();
            }
        }
    }
    #endif

    if(gMmwMssMCB.measureRxChannelBiasCliCfg.enabled)
    {
        retVal = mmwDemo_rangeBiasRxChPhaseMeasureConfig();
        if (retVal != 0)
        {
            CLI_write("DPC configuration: Invalid Rx channel compensation procedure configuration \n");
            DebugP_assert(0);
        }
    }

    if (!gMmwMssMCB.oneTimeConfigDone)
    {

        /* Report RAM usage */
        gMmwMssMCB.memUsage.CoreLocalRamUsage = DPC_ObjDet_MemPoolGetMaxUsage(&gMmwMssMCB.CoreLocalRamObj);
        gMmwMssMCB.memUsage.L3RamUsage = DPC_ObjDet_MemPoolGetMaxUsage(&gMmwMssMCB.L3RamObj);
        HeapP_getHeapStats(&gMmwMssMCB.CoreLocalTrackerHeapObj, &gMmwMssMCB.memUsage.trackerHeapStats);

        gMmwMssMCB.memUsage.L3RamTotal = gMmwMssMCB.L3RamObj.cfg.size;
        gMmwMssMCB.memUsage.CoreLocalRamTotal = gMmwMssMCB.CoreLocalRamObj.cfg.size;
    
        if(gMmwMssMCB.lowPowerMode == LOW_PWR_MODE_DISABLE)
        {
            DebugP_log(" ========== Memory Stats ==========\n");
            DebugP_log("%20s %12s %12s %12s\n", " ", "Size", "Used", "Free");

            DebugP_log("%20s %12d %12d %12d\n", "L3",
                      sizeof(gMmwL3),
                      gMmwMssMCB.memUsage.L3RamUsage,
                      sizeof(gMmwL3) - gMmwMssMCB.memUsage.L3RamUsage);

            DebugP_log("%20s %12d %12d %12d\n", "Local",
                      sizeof(gMmwCoreLocMem),
                      gMmwMssMCB.memUsage.CoreLocalRamUsage,
                      sizeof(gMmwCoreLocMem) - gMmwMssMCB.memUsage.CoreLocalRamUsage);
            DebugP_log("%20s %12d %12d %12d\n", "Tracker",
                      sizeof(gMmwCoreLocMem2),
                      sizeof(gMmwCoreLocMem2) - gMmwMssMCB.memUsage.trackerHeapStats.availableHeapSpaceInBytes,
                      gMmwMssMCB.memUsage.trackerHeapStats.availableHeapSpaceInBytes);
            DebugP_log("%20s %12d %12d %12d\n", "FeatExt",
                      sizeof(gMmwCoreLocMem3),
                      featExtract_memUsage(),
                      sizeof(gMmwCoreLocMem3) - featExtract_memUsage());
        }
    }

#if (CLI_REMOVAL==1)
    /* Trigger CLI init completion for one time config done */
    SemaphoreP_post(&gMmwMssMCB.dpcCfgDoneSemHandle);
#endif

}

/**
 *  @b Description
 *  @n  DPC processing chain execute function.
 *
*/
void DPC_Execute(){
    int32_t retVal;
    int32_t errCode = 0;
    int32_t i;
    DPU_RangeProcHWA_OutParams outParms;
    DPU_DoaProc_OutParams outParmsDoaproc;
    DPU_AoasvcProc_OutParams outParmsAoasvcproc;
    DPU_CFARProcHWA_OutParams outParmsCfar;
    DPU_MpdProc_OutParams outParmsMpdproc;
    uint8_t enableMajorMotion;
    uint8_t enableMinorMotion;
    int16_t skipFrmCntr = 0;//gMmwMssMCB.sigProcChainCfg.numFrmPerMinorMotProc - 1; //We start minor motion from the first frame although the radar cube is not fully filled 
    DPC_ObjectDetection_ExecuteResult *result = &gMmwMssMCB.dpcResult;
    uint32_t numDetectedPoints[2]; //numDetectedPoints[0] - Number of points in major motion detection, numDetectedPoints[0] - Number of points in minor motion detection,
    #if (SPI_ADC_DATA_STREAMING==1)
    MCSPI_Transaction   spiTransaction;
    int32_t             transferOK;
    uint32_t totalSizeToTfr,tempSize;
    uint8_t count;
    #endif
    
    #if (CLI_REMOVAL == 0 || TRACKER_CLASSIFIER_ENABLE == 1)
    DPU_uDopProc_TrackerData trackerData;
    #endif

    /* give initial trigger for the first frame */
    errCode = DPU_RangeProcHWA_control(gMmwMssMCB.rangeProcDpuHandle,
                 DPU_RangeProcHWA_Cmd_triggerProc, NULL, 0);
    if(errCode < 0)
    {
        CLI_write("Error: Range control execution failed [Error code %d]\n", errCode);
    }

    if (gMmwMssMCB.sigProcChainCfg.motDetMode == 1)
    {
        enableMajorMotion = 1;
        enableMinorMotion = 0;
    }
    else if (gMmwMssMCB.sigProcChainCfg.motDetMode == 3)
    {
        enableMajorMotion = 1;
        enableMinorMotion = 1;
    }
    else
    {
        enableMajorMotion = 0;
        enableMinorMotion = 1;
    }
    if (enableMajorMotion)
    {
        result->rngAzHeatMap[MMW_DEMO_MAJOR_MODE] = (uint32_t *) gMmwMssMCB.detMatrix[MMW_DEMO_MAJOR_MODE].data;
    }
    else
    {
        result->rngAzHeatMap[MMW_DEMO_MAJOR_MODE] = NULL;
    }
    if (enableMinorMotion)
    {
        result->rngAzHeatMap[MMW_DEMO_MINOR_MODE] = (uint32_t *) gMmwMssMCB.detMatrix[MMW_DEMO_MINOR_MODE].data;
    }
    else
    {
        result->rngAzHeatMap[MMW_DEMO_MINOR_MODE] = NULL;
    }

    result->objOut = gMmwMssMCB.dpcObjOut;
    result->objOutSideInfo = gMmwMssMCB.dpcObjSideInfo;

    while(true){

        memset((void *)&outParms, 0, sizeof(DPU_RangeProcHWA_OutParams));
        retVal = DPU_RangeProcHWA_process(gMmwMssMCB.rangeProcDpuHandle, &outParms);
        if(retVal != 0){
            CLI_write("DPU_RangeProcHWA_process failed with error code %d", retVal);
            DebugP_assert(0);
        }

        /* Procedure for range bias measurement and Rx channels gain/phase offset measurement */
        if(gMmwMssMCB.measureRxChannelBiasCliCfg.enabled)
        {
            mmwDemo_rangeBiasRxChPhaseMeasure();
        }
        /***************************ADC Streaming Via SPI***********************************************************/

        #if (SPI_ADC_DATA_STREAMING==1)

            if( gMmwMssMCB.spiADCStream == 1)
            {
                uint32_t* adc_data = (uint32_t*)adcbuffer;
                totalSizeToTfr = adcDataPerFrame;
                tempSize = adcDataPerFrame;
                count = 0;
                while(totalSizeToTfr > 0)
                {
                    if(totalSizeToTfr > MAXSPISIZEFTDI)
                    {
                        tempSize=MAXSPISIZEFTDI;
                    }
                    else
                    {
                        tempSize = totalSizeToTfr;
                    }
                    
                    MCSPI_Transaction_init(&spiTransaction);
                    spiTransaction.channel  = gConfigMcspi0ChCfg[0].chNum;
                    spiTransaction.dataSize = 32;
                    spiTransaction.csDisable = TRUE;
                    spiTransaction.count    = tempSize/4;
                    spiTransaction.txBuf    = (void *)(&adc_data[(MAXSPISIZEFTDI/4)*count]);
                    spiTransaction.rxBuf    = NULL;
                    spiTransaction.args     = NULL;

                    GPIO_pinWriteLow(gpioBaseAddrLed, pinNumLed);
                
                    transferOK = MCSPI_transfer(gMcspiHandle[CONFIG_MCSPI0], &spiTransaction);
                    
                    if(transferOK != 0)
                    {
                        CLI_write("SPI Raw Data Transfer Failed\r\n");
                    }
                    GPIO_pinWriteHigh(gpioBaseAddrLed, pinNumLed);
                    totalSizeToTfr  =   totalSizeToTfr  -   tempSize;
                    count++;
                }

            }
        #endif
    /********************************************************************/
        #if (ENABLE_GPADC==1U)
       // Read the GPADC Data

        MMWave_readGPADC(&gMmwMssMCB.GPADCVal[0],&gMmwMssMCB.GPADCVal[1]);
       //CLI_write("\r\n GPADC 1 Reading: %f\r\n",gMmwMssMCB.GPADCVal[0]);
       //CLI_write("\r\n GPADC 2 Reading: %f\r\n",gMmwMssMCB.GPADCVal[1]);
        #endif

       // Read the temperature
        MMWave_getTemperatureReport(&tempStats);
        
        #if (ENABLE_MONITORS==1)
        // If atleast one monitor is enabled.
        if(gMmwMssMCB.rfMonEnbl != 0)
        {
            // Enable Monitors configured (They have to be enabled only during frame idle time)
            MMWave_enableMonitors(gMmwMssMCB.ctrlHandle);
        }
        /* If Synth Frequency Monitor is enabled read the value*/
        if((gMmwMssMCB.sensorStart.frameLivMonEn & 0x1) == 0x1)
        {
            gMmwMssMCB.rfMonRes.synthFreqres = MMWaveMon_getSynthFreqMonres();
            #if (PRINT_MON_RES == 1)
            CLI_write("Synth Frequency monitor: %x \r\n",gMmwMssMCB.rfMonRes.synthFreqres.status);
            #endif
        }

        /* If Rx Sat Live Monitor is enabled read the value*/
        if((gMmwMssMCB.sensorStart.frameLivMonEn & 0x2) == 0x2)
        {
            /*Base Address of Rx Saturation Live Monitor Results */
            uint32_t *baseAddrRxSatLive; 
            gMmwMssMCB.rfMonRes.rxSatLiveres.rxSatLivePtr = MMWaveMon_getRxSatLiveMonres(); 
            /*Each chirp will have 1 byte data for each RX, max 4 RX is supported (4th Rx is reserved)*/
            for (int i=0; i<(gMmwMssMCB.frameCfg.h_NumOfChirpsInBurst * gMmwMssMCB.frameCfg.h_NumOfBurstsInFrame);i+=1U)
            {
                /* Incrementing Address by 32 bits */
                baseAddrRxSatLive = gMmwMssMCB.rfMonRes.rxSatLiveres.rxSatLivePtr + i;
                /*Checking Rx Saturation Live Monitor status*/
                if(*baseAddrRxSatLive==0)
                {
                    gMmwMssMCB.rfMonRes.status_rxSatLive=0x1; // No Saturation
                }
                else
                {
                    gMmwMssMCB.rfMonRes.status_rxSatLive=0x0;
                    break;
                }
            }
            #if (PRINT_MON_RES == 1)
            if(gMmwMssMCB.rfMonRes.status_rxSatLive == 0)
            {
                CLI_write("Rx Saturation Live monitor: Saturation is occurring \r\n");
            }
            else
            {
                CLI_write("Rx Saturation Live monitor: No Saturation  \r\n");
            }
            #endif
        }
        #endif
        
        /* Chirping finished start interframe processing */
        gMmwMssMCB.stats.interFrameStartTimeStamp = Cycleprofiler_getTimeStamp();
        gMmwMssMCB.stats.chirpingTime_us = (gMmwMssMCB.stats.interFrameStartTimeStamp - gMmwMssMCB.stats.frameStartTimeStamp)/FRAME_REF_TIMER_CLOCK_MHZ;

        if((gMmwMssMCB.lowPowerMode == LOW_PWR_MODE_ENABLE) || (gMmwMssMCB.lowPowerMode == LOW_PWR_TEST_MODE))
        {
            //Shutdown the FECSS after chirping
            // Retain FECSS Code Memory
            int32_t err;

            if(pgVersion==1)
            {
                PRCMSetSRAMRetention((PRCM_FEC_PD_SRAM_CLUSTER_2 | PRCM_FEC_PD_SRAM_CLUSTER_3), PRCM_SRAM_LPDS_RET);
            }
            else
            {
                PRCMSetSRAMRetention((PRCM_FEC_PD_SRAM_CLUSTER_1), PRCM_SRAM_LPDS_RET);
            }

            //Reset The FrameTimer for next frame
            HW_WR_REG32(CSL_APP_RCM_U_BASE + CSL_APP_RCM_BLOCKRESET2, 0x1c0);
            for(int i =0;i<10;i++)
            {
                test = PRCMSlowClkCtrGet();
            }
            HW_WR_REG32(CSL_APP_RCM_U_BASE + CSL_APP_RCM_BLOCKRESET2, 0x0);

            /* Delay to account for RFS Processing time before shutting it down */
            ClockP_usleep(RFS_PROC_END_TIME);
            
            #if (ENABLE_MONITORS==1)
            // If atleast one monitor is enabled ,wait till monitors are complete
            if(gMmwMssMCB.rfMonEnbl != 0)
            {
                SemaphoreP_pend(&gMmwMssMCB.rfmonSemHandle, SystemP_WAIT_FOREVER);
            }
            #endif
            // MMW Closure in preparation for Low power state
            MMWave_stop(gMmwMssMCB.ctrlHandle,&err);
            MMWave_close(gMmwMssMCB.ctrlHandle,&err);
            MMWave_deinit(gMmwMssMCB.ctrlHandle,&err);
            /* As the Frame timer is reset, Capture the Inter Frame Start Time again */
            gMmwMssMCB.stats.interFrameStartTimeStamp = Cycleprofiler_getTimeStamp();
        }

        /* Trigger doaproc, CFAR and mpdproc DPUs subsequently */
        memset((void *)&outParmsDoaproc, 0, sizeof(DPU_DoaProc_OutParams));
        memset((void *)&outParmsCfar, 0, sizeof(DPU_CFARProcHWA_OutParams));
        memset((void *)&outParmsMpdproc, 0, sizeof(DPU_MpdProc_OutParams));

        numDetectedPoints[MMW_DEMO_MAJOR_MODE] = 0;
        if (enableMajorMotion)
        {
            memset((void *)&outParmsDoaproc, 0, sizeof(DPU_DoaProc_OutParams));
            retVal = DPU_DoaProc_process(gMmwMssMCB.doaProcDpuHandle, &gMmwMssMCB.radarCubeSrc[MMW_DEMO_MAJOR_MODE], &gMmwMssMCB.detMatrix[MMW_DEMO_MAJOR_MODE], &outParmsDoaproc);
            if(retVal != 0){
                CLI_write("DPU_DoaProc_process failed with error code %d", retVal);
                DebugP_assert(0);
            }

            memset((void *)&outParmsCfar, 0, sizeof(DPU_CFARProcHWA_OutParams));
            outParmsCfar.rangeProfile = gMmwMssMCB.rangeProfile[MMW_DEMO_MAJOR_MODE];
            outParmsCfar.detObjOut = &gMmwMssMCB.cfarDetObjOut[0];
            outParmsCfar.detObjIndOut = &gMmwMssMCB.dpcObjIndOut[0];
            outParmsCfar.detObjOutMaxSize = MAX_NUM_DETECTIONS;
            retVal = DPU_CFARProcHWA_process(gMmwMssMCB.cfarProcDpuHandle,
                                             &gMmwMssMCB.detMatrix[MMW_DEMO_MAJOR_MODE],
                                             &gMmwMssMCB.cfarRunTimeInputParams[MMW_DEMO_MAJOR_MODE],
                                             &outParmsCfar);
            numDetectedPoints[MMW_DEMO_MAJOR_MODE] = outParmsCfar.numCfarDetectedPoints;


            if(retVal != 0){
                CLI_write("DPU_CFARProcHWA_process failed with error code %d", retVal);
                DebugP_assert(0);
            }
        }

        numDetectedPoints[MMW_DEMO_MINOR_MODE] = 0;
        if (enableMinorMotion)
        {
            if (skipFrmCntr > 0)
            {
                skipFrmCntr--;
            }
            else
            {
                memset((void *)&outParmsDoaproc, 0, sizeof(DPU_DoaProc_OutParams));

                retVal = DPU_DoaProc_process(gMmwMssMCB.doaProcDpuHandle, &gMmwMssMCB.radarCubeSrc[MMW_DEMO_MINOR_MODE], &gMmwMssMCB.detMatrix[MMW_DEMO_MINOR_MODE], &outParmsDoaproc);
                if(retVal != 0){
                    CLI_write("DPU_DoaProc_process failed with error code %d", retVal);
                    DebugP_assert(0);
                }

                memset((void *)&outParmsCfar, 0, sizeof(DPU_CFARProcHWA_OutParams));
                outParmsCfar.rangeProfile = gMmwMssMCB.rangeProfile[MMW_DEMO_MINOR_MODE];
                outParmsCfar.detObjOut = &gMmwMssMCB.cfarDetObjOut[numDetectedPoints[MMW_DEMO_MAJOR_MODE]];
                outParmsCfar.detObjIndOut = &gMmwMssMCB.dpcObjIndOut[numDetectedPoints[MMW_DEMO_MAJOR_MODE]];
                outParmsCfar.detObjOutMaxSize = MAX_NUM_DETECTIONS - numDetectedPoints[MMW_DEMO_MAJOR_MODE];
                retVal = DPU_CFARProcHWA_process(gMmwMssMCB.cfarProcDpuHandle,
                                                 &gMmwMssMCB.detMatrix[MMW_DEMO_MINOR_MODE],
                                                 &gMmwMssMCB.cfarRunTimeInputParams[MMW_DEMO_MINOR_MODE],
                                                 &outParmsCfar);
                numDetectedPoints[MMW_DEMO_MINOR_MODE] = outParmsCfar.numCfarDetectedPoints;

                if(retVal != 0){
                    CLI_write("DPU_CFARProcHWA_process failed with error code %d", retVal);
                    DebugP_assert(0);
                }
            }
        }

        /* For AOA estimation improvement using steering vectors */
        /*
        if (gMmwMssMCB.steeringVecCorrCfg.enableAntSymbGen)
        {
            outParmsAoasvcproc.virtAntElemList = gMmwMssMCB.virtAntElemList;
            retVal = DPU_AoasvcProc_process(gMmwMssMCB.aoasvcProcDpuHandle,
                                            numDetectedPoints[MMW_DEMO_MAJOR_MODE],
                                            numDetectedPoints[MMW_DEMO_MINOR_MODE],
                                            gMmwMssMCB.cfarDetObjOut,
                                            gMmwMssMCB.dpcObjIndOut,
                                            &outParmsAoasvcproc);
            if(retVal != 0){
                CLI_write("DPU_AoasvcProc_process failed with error code %d", retVal);
                DebugP_assert(0);
            }
        }
       */

/*For debugging purposes*/
#if 0
        {
            cmplx16ImRe_t *radCub = (cmplx16ImRe_t *) gMmwMssMCB.radarCube[1].data;
            int i;
            uint32_t numSampPerFrame = doaProcDpuCfg.staticCfg.numVirtualAntennas * doaProcDpuCfg.staticCfg.numRangeBins * doaProcDpuCfg.staticCfg.numMinorMotionChirpsPerFrame;
            for(i=0; i<doaProcDpuCfg.staticCfg.numFrmPerMinorMotProc; i++)
            {
                gTest1Tx[gTest1TxCntr][i] = radCub[5 + i*numSampPerFrame].imag;
            }
            gTest1TxCntr = (gTest1TxCntr+1) & 15;
        }
#endif

        result->numObjOutMajor = numDetectedPoints[MMW_DEMO_MAJOR_MODE];
        result->numObjOutMinor = numDetectedPoints[MMW_DEMO_MINOR_MODE];
        result->numObjOut = numDetectedPoints[MMW_DEMO_MAJOR_MODE] + numDetectedPoints[MMW_DEMO_MINOR_MODE];

        #if (CLI_REMOVAL == 0 || MPD_ENABLE == 1)
        {
            if (gMmwMssMCB.isMotionPresenceDpuEnabled)
            {
                outParmsMpdproc.mpdZoneState = gMmwMssMCB.dpcZoneState;

                retVal = DPU_MpdProc_process(gMmwMssMCB.mpdProcDpuHandle, &outParmsMpdproc);
                if(retVal != 0){
                    CLI_write("DPU_MpdProc_process failed with error code %d", retVal);
                    DebugP_assert(0);
                }

                //outParmsMpdproc.minMpdCentroid gives the range of nearest cluster from Radar position
                gTestMinMpdCentroid = outParmsMpdproc.minMpdCentroid;
            }
        }
        #endif
        if(gMmwMssMCB.guiMonSel.pointCloud == 1)
        {
            for(i=0; i < result->numObjOut; i++)
            {
                result->objOut[i].x = gMmwMssMCB.cfarDetObjOut[i].x;
                result->objOut[i].y = gMmwMssMCB.cfarDetObjOut[i].y;
                result->objOut[i].z = gMmwMssMCB.cfarDetObjOut[i].z;
                result->objOut[i].velocity = gMmwMssMCB.cfarDetObjOut[i].velocity;
                result->objOutSideInfo[i].snr = (int16_t) (10. * gMmwMssMCB.cfarDetObjOut[i].snr); //steps of 0.1dB
                result->objOutSideInfo[i].noise = (int16_t) (10. * gMmwMssMCB.cfarDetObjOut[i].noise); //steps of 0.1dB
            }
        }

        if(gMmwMssMCB.guiMonSel.pointCloud == 2)
        {
            /* Compress point cloud list (data converted from floating point to fix point) */
            MmwDemo_compressPointCloudList(&gMmwMssMCB.pointCloudToUart,
                                           &gMmwMssMCB.pointCloudUintRecip,
                                           gMmwMssMCB.cfarDetObjOut,
                                           result->numObjOut);
            gMmwMssMCB.pointCloudToUart.pointUint.numDetectedPoints[0] = numDetectedPoints[0]; //Number of points in major motion mode
            gMmwMssMCB.pointCloudToUart.pointUint.numDetectedPoints[1] = numDetectedPoints[1]; //Number of points in minor motion mode
        }

        #if (CLI_REMOVAL == 0 || TRACKER_CLASSIFIER_ENABLE == 1)
        {
            if(gMmwMssMCB.trackerCfg.staticCfg.trackerEnabled)
            {
                uint32_t trackerStartTime = Cycleprofiler_getTimeStamp();

                /* Group tracker DPU */
                retVal = DPU_TrackerProc_process(gMmwMssMCB.trackerProcDpuHandle,
                                                (uint16_t) (numDetectedPoints[0] + numDetectedPoints[1]),
                                                gMmwMssMCB.cfarDetObjOut,
                                                &result->trackerOutParams);
                if (retVal != 0)
                {
                    CLI_write("Error: DPU_TrackerProc_process failed with error code %d", retVal);
                    DebugP_assert(0);
                }
                gMmwMssMCB.stats.trackerTime_us = (Cycleprofiler_getTimeStamp() - trackerStartTime)/FRAME_REF_TIMER_CLOCK_MHZ;
            }

            if (result->trackerOutParams.numTargets > 0)
            {
                if (vsDataCount == 0)
                {
                    xDistForVS       = (float)result->trackerOutParams.tList[0].posX;
                    yDistForVS       = (float)result->trackerOutParams.tList[0].posY;
                    radialDistance = sqrtf((xDistForVS * xDistForVS) + (yDistForVS * yDistForVS));
                    vsRangeBin        = (uint16_t)(radialDistance / 0.093);
                    vsBaseAddr         = (uint32_t)gMmwMssMCB.radarCube[0].data;
                    vsBaseAddr         = vsBaseAddr + (vsRangeBin * 4) - 8;
                }
                indicateNoTarget = 0;
            }
            else
            {
                indicateNoTarget = 1;
                vsLoop           = 0;
            }
    
            vitalSignsAntenna.vsActiveAntennaGeometryCfg[0].row = gMmwMssMCB.activeAntennaGeometryCfg.ant[0].row;
            vitalSignsAntenna.vsActiveAntennaGeometryCfg[0].col = gMmwMssMCB.activeAntennaGeometryCfg.ant[0].col;
            vitalSignsAntenna.vsActiveAntennaGeometryCfg[1].row = gMmwMssMCB.activeAntennaGeometryCfg.ant[1].row;
            vitalSignsAntenna.vsActiveAntennaGeometryCfg[1].col = gMmwMssMCB.activeAntennaGeometryCfg.ant[1].col;
            vitalSignsAntenna.vsActiveAntennaGeometryCfg[2].row = gMmwMssMCB.activeAntennaGeometryCfg.ant[2].row;
            vitalSignsAntenna.vsActiveAntennaGeometryCfg[2].col = gMmwMssMCB.activeAntennaGeometryCfg.ant[2].col;
            vitalSignsAntenna.vsActiveAntennaGeometryCfg[3].row = gMmwMssMCB.activeAntennaGeometryCfg.ant[3].row;
            vitalSignsAntenna.vsActiveAntennaGeometryCfg[3].col = gMmwMssMCB.activeAntennaGeometryCfg.ant[3].col;
            vitalSignsAntenna.vsActiveAntennaGeometryCfg[4].row = gMmwMssMCB.activeAntennaGeometryCfg.ant[4].row;
            vitalSignsAntenna.vsActiveAntennaGeometryCfg[4].col = gMmwMssMCB.activeAntennaGeometryCfg.ant[4].col;
            vitalSignsAntenna.vsActiveAntennaGeometryCfg[5].row = gMmwMssMCB.activeAntennaGeometryCfg.ant[5].row;
            vitalSignsAntenna.vsActiveAntennaGeometryCfg[5].col = gMmwMssMCB.activeAntennaGeometryCfg.ant[5].col;
            vitalSignsAntenna.numAntCol = gMmwMssMCB.numAntCol;
            vitalSignsAntenna.numAntRow = gMmwMssMCB.numAntRow;
            vitalSignsAntenna.numRxAntennas = gMmwMssMCB.numRxAntennas;
            vitalSignsAntenna.numTxAntennas = gMmwMssMCB.numTxAntennas;
            vitalSignsAntenna.numRangeBins = gMmwMssMCB.numRangeBins;
    
            vsLoop = MmwDemo_runVitalSigns(vsBaseAddr, indicateNoTarget, vsLoop, vitalSignsAntenna);

/*
            if (gMmwMssMCB.microDopplerCliCfg.enabled)
            {
                uint32_t microDopplerStartTime = Cycleprofiler_getTimeStamp();
                trackerData.numTargets = result->trackerOutParams.numTargets;
                trackerData.numIndices = result->trackerOutParams.numIndices;
                trackerData.numIndicesMajorMotion = numDetectedPoints[0];
                trackerData.numIndicesMinorMotion = numDetectedPoints[1];
                trackerData.tIndex = result->trackerOutParams.targetIndex;

                for (i = 0; i < trackerData.numTargets; i++)
                {
                    trackerData.tList[i].tid  = result->trackerOutParams.tList[i].tid;
                    trackerData.tList[i].posX = result->trackerOutParams.tList[i].posX;
                    trackerData.tList[i].posY = result->trackerOutParams.tList[i].posY;
                    trackerData.tList[i].posZ = result->trackerOutParams.tList[i].posZ;
                    if (gMmwMssMCB.microDopplerCliCfg.circShiftAroundCentroid)
                    {
                        trackerData.tList[i].velX = result->trackerOutParams.tList[i].velX;
                        trackerData.tList[i].velY = result->trackerOutParams.tList[i].velY;
                        trackerData.tList[i].velZ = result->trackerOutParams.tList[i].velZ;
                    }
                }

                result->microDopplerOutParams.uDopplerOutput = gMmwMssMCB.uDopProcOutParams.uDopplerOutput;
                result->microDopplerOutParams.uDopplerFeatures = gMmwMssMCB.uDopProcOutParams.uDopplerFeatures;

                retVal = DPU_uDopProc_process(gMmwMssMCB.microDopDpuHandle,
                                            &gMmwMssMCB.radarCube[0], //Major motion radar cube
                                            &gMmwMssMCB.detMatrix[0], //Major motion detection matrix
                                            &trackerData,
                                            &result->microDopplerOutParams);
                if (retVal != 0)
                {
                    CLI_write("Error: DPU_uDopProc_process failed with error code %d", retVal);
                    DebugP_assert(0);
                }
                gMmwMssMCB.stats.microDopplerDpuTime_us = (Cycleprofiler_getTimeStamp() - microDopplerStartTime)/FRAME_REF_TIMER_CLOCK_MHZ;
                gMmwMssMCB.stats.featureExtractionTime_us = result->microDopplerOutParams.stats.featureExtractTime_cpuCycles/FRAME_REF_TIMER_CLOCK_MHZ;
                gMmwMssMCB.stats.classifierTime_us = result->microDopplerOutParams.stats.classifierTime_cpuCycles/FRAME_REF_TIMER_CLOCK_MHZ;

            }
*/
        }
        #endif
        
        #if (CLI_REMOVAL == 0)
        {
            if (gMmwMssMCB.adcDataSourceCfg.source == 2)
            {
                ClockP_sleep(1);
            }
        }
        #endif
        
        /* Give initial trigger for the next frame */
        retVal = DPU_RangeProcHWA_control(gMmwMssMCB.rangeProcDpuHandle,
                    DPU_RangeProcHWA_Cmd_triggerProc, NULL, 0);
        if(retVal < 0)
        {
            CLI_write("Error: DPU_RangeProcHWA_control failed with error code %d", retVal);
            DebugP_assert(0);
        }


        /* Interframe processing finished */
        gMmwMssMCB.stats.interFrameEndTimeStamp = Cycleprofiler_getTimeStamp();
        gMmwMssMCB.outStats.interFrameProcessingTime = (gMmwMssMCB.stats.interFrameEndTimeStamp - gMmwMssMCB.stats.interFrameStartTimeStamp)/FRAME_REF_TIMER_CLOCK_MHZ;

        #if (CLI_REMOVAL == 0 && DYNAMIC_RECONFIG == 1) 
        if(gMmwMssMCB.profileSwitchCfg.switchCfgEnable)
        {
            mmwDemo_ProfileSwitchStateMachine();
        }
        #endif

        /* Trigger UART task to send TLVs to host */
        SemaphoreP_post(&gMmwMssMCB.tlvSemHandle);
    }
}
