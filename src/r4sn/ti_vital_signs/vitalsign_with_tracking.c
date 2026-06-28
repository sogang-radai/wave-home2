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
#include <datapath/dpedma/v0/dpedmahwa.h>
#include <datapath/dpedma/v0/dpedma.h>
#include <datapath/dpu/rangeproc/v0/rangeprochwa.h>
#include <datapath/dpu/rangeproc/v0/rangeprochwa_internal.h>
#include <datapath/dpu/mpdproc/v0/mpdproc.h>
#include <datapath/dpu/trackerproc/v0/trackerproc.h>

#include <control/mmwave/mmwave.h>
#include <drivers/edma.h>
#include <drivers/uart.h>
#include <drivers/i2c.h>
#include <drivers/hw_include/cslr_adcbuf.h>
#include <drivers/mcspi.h>

#include <board/ina.h>

#include <utils/mathutils/mathutils.h>
#include "mmw_cli.h"

#include "mmw_res.h"
#include "motion_detect.h"
#include "profile_switch.h"
#include "calibrations/mmw_flash_cal.h"


#include "ti_drivers_config.h"
#include "ti_drivers_open_close.h"
#include "ti_board_open_close.h"
#include "ti_board_config.h"

#include <FreeRTOS.h>
#include <task.h>
#include <semphr.h>
#include "drivers/power.h"
#include <mmwavelink/include/rl_device.h>
#include <mmwavelink/include/rl_sensor.h>
#include <drivers/prcm.h>
#include <drivers/hw_include/cslr_soc.h>

#include "dpc/dpc.h"
#include "calibrations/range_phase_bias_measurement.h"
#include "utils/mmw_demo_utils.h"
#include "calibrations/factory_cal.h"
#include "mmwave_control/monitors.h"
#include "power_management/power_management.h"

#include "vitalsign.h"
extern vsFeature vitalSignsOutput;

/* Enable Continuous wave mode */
#define CONTINUOUS_WAVE_MODE_ENABLE   0

#define EDMA_TEST_EVT_QUEUE_NO      (0U)
#define MAX_NUM_RX_ANTENNA          (3U)
#define MAX_NUM_ADCSAMPLE_PERCHIRP  (128U)
#define MAX_AZ_FFT_SIZE             (64U)
#define DPU_DOAPROC_6432_1432_BOARD 0
#define DPU_DOAPROC_ISK_BOARD 1
#define DPC_OBJDET_DPIF_RADARCUBE_FORMAT_6 6

#define FRAME_REF_TIMER_CLOCK_MHZ  40

/* Max Frame Size for FTDI chip is 64KB */
#define MAXSPISIZEFTDI               (65536U)

// Radar Power Management Framework
// Priority of Power task
#define POWER_TASK_PRI  (2u)
// Stack for Power task
#define POWER_TASK_SIZE (512u)
#define LOW_PWR_MODE_DISABLE (0)
#define LOW_PWR_MODE_ENABLE (1)
#define LOW_PWR_TEST_MODE (2)

// Time to transfer single Byte of data obtained by measuring time for various data size.
#define TIME_TO_SEND_1BYTE_DATA_WITH_BAUDRATE_115200_us  95  
#define TIME_TO_SEND_1BYTE_DATA_WITH_BAUDRATE_1250000_us  10

#define MMWINITTASK_PRI  (5u)
#define MMWINIT_TASK_SIZE (512u)

uint8_t                 pgVersion;
uint8_t                 rxData[16];
uint8_t                 txData[16];
I2C_Transaction         i2cTransaction;

MmwDemo_MSS_MCB gMmwMssMCB = {0};

float gTestMinMpdCentroid = 0;
float gTestRangeLED = 0;

/* Default antenna geometry - XWR6432 EVM */
MmwDemo_antennaGeometryCfg gDefaultAntGeometry = {.ant = {{0,0}, {1,1}, {0,2}, {0,1}, {1,2}, {0,3}}};

//Interrupt object for Sensor Stop
HwiP_Object gHwiSensorStopHwiObject;

#if (CLI_REMOVAL == 0 && DYNAMIC_RECONFIG == 1)
MmwDemo_PresenceSwitch_Config prDetectParams;
MmwDemo_TrackerSwitch_Config trDetectParams;
volatile uint8_t  gFlagPresenceDetect         = 0;
/* This is enabled when the presence has been detected for threshold number of frames but config not switched to tracking */
volatile uint16_t  gPresenceToTrackSwitchCntr = 1;
volatile uint16_t gTrackToPresenceSwitchCntr  = 1;
uint8_t gUserCfgPresenceTrack = 0;
volatile uint8_t gOneTimeSwitch = 1;

char* trackCmdString[MAX_TRACKER_CMD] =
{
#if 1
"channelCfg 7 3 0 \r\n",
"chirpComnCfg 15 0 0 128 4 28 0 \r\n",
#if defined (SOC_XWRL64XX)
"chirpTimingCfg 6 32 0 40 60.5 \r\n",
#else
"chirpTimingCfg 6 32 0 40 77.5 \r\n",
#endif
"frameCfg 2 0 200 64 100 0 \r\n",
"guiMonitor 2 3 0 0 0 1 0 0 1 1 1 \r\n",
"sigProcChainCfg 32 2 3 2 8 8 1 0.3 \r\n",
"cfarCfg 2 8 4 3 0 12.0 0 0.5 0 1 1 1 \r\n",
"aoaFovCfg -70 70 -40 40 \r\n",
"rangeSelCfg 0.1 10.0 \r\n",
"clutterRemoval 1 \r\n",
"boundaryBox -3.5 3.5 0 9 -0.5 3 \r\n",
"staticBoundaryBox -3 3 0.5 7.5 0 3 \r\n",
"gatingParam 3 2 2 2 4 \r\n",
"stateParam 3 3 12 50 5 200 \r\n",
"allocationParam 6 10 0.1 4 0.5 20 \r\n",
"maxAcceleration 0.4 0.4 0.1 \r\n",
"trackingCfg 1 2 100 3 \r\n",
"presenceBoundaryBox -3 3 0.5 7.5 0 3 \r\n",
"microDopplerCfg 1 0 0.5 0 1 1 12.5 87.5 1 \r\n",
"classifierCfg 1 3 4 \r\n"
#endif
};

char* presenceCmdString[MAX_PRESENCE_CMD] =
{
#if 1
"channelCfg 7 3 0 \r\n",
"chirpComnCfg 20 0 0 128 4 30 0 \r\n",
#if defined (SOC_XWRL64XX)
"chirpTimingCfg 6 28 0 90 59.75 \r\n",
#else
"chirpTimingCfg 6 28 0 90 77 \r\n",
#endif
"frameCfg 8 0 403 1 250 0 \r\n",
"guiMonitor 2 2 0 0 0 1 1 0 0 0 0 \r\n",
"sigProcChainCfg 64 8 2 0 4 4 0 .5 \r\n",
"cfarCfg 2 4 3 2 0 12.0 0 0.5 0 1 1 1 \r\n",
"aoaFovCfg -60 60 -40 40 \r\n",
"rangeSelCfg 0.1 4.0 \r\n",
"clutterRemoval 1 \r\n",
"mpdBoundaryBox 1 0 1.48 0 1.95 0 3 \r\n",
"mpdBoundaryBox 2 0 1.48 1.95 3.9 0 3 \r\n",
"mpdBoundaryBox 3 -1.48 0 0 1.95 0 3 \r\n",
"mpdBoundaryBox 4 -1.48 0 1.95 3.9 0 3 \r\n",
"minorStateCfg 5 4 40 8 4 30 8 8 \r\n",
"clusterCfg 1 0.5 2 \r\n"
#endif
};

char* trackCmdStringAOP[MAX_TRACKER_CMD] =
{
#if 1
"channelCfg 7 3 0 \r\n",
"chirpComnCfg 8 0 0 256 1 23 2 \r\n",
"chirpTimingCfg 6 24 0 75 60.5 \r\n",
"frameCfg 2 8 600 16 170 0 \r\n",
"guiMonitor 2 3 0 0 0 1 0 0 1 0 0 \r\n",
"sigProcChainCfg 64 4 3 2 8 4 0 0.3 0 \r\n",
"cfarCfg 2 8 4 3 0 15 0 0.9 0 1 1 1 \r\n",
"aoaFovCfg -70 70 -60 60 \r\n",
"rangeSelCfg 0.25 7.5 \r\n",
"clutterRemoval 1 \r\n",
"boundaryBox -7 7 0 7.5 -0.5 3 \r\n",
"staticBoundaryBox -6.5 6.5 0.5 7 0 3 \r\n",
"gatingParam 3 2.5 2.5 4 8 \r\n",
"stateParam 3 3 25 50 5 50 \r\n",
"allocationParam 15 15 0.0001 5 3 2 \r\n",
"maxAcceleration 1.2 1.2 0.5 \r\n",
"trackingCfg 1 2 100 3 \r\n",
"presenceBoundaryBox -6.5 6.5 0.5 7 0 3 \r\n",
#endif
};

char* presenceCmdStringAOP[MAX_PRESENCE_CMD] =
{
#if 1
"channelCfg 7 3 0 \r\n",
"chirpComnCfg 8 0 0 256 1 23 2 \r\n",
"chirpTimingCfg 6 24 0 75 60.5 \r\n",
"frameCfg 2 4 600 4 250 0 \r\n",
"guiMonitor 2 2 0 0 0 1 1 0 0 0 0 \r\n",
"sigProcChainCfg 64 4 2 2 4 4 0 0.5 0 \r\n",
"cfarCfg 2 8 4 3 0 15 0 0.9 0 1 1 1 \r\n",
"aoaFovCfg -70 70 -60 60 \r\n",
"rangeSelCfg 0.25 7.5 \r\n",
"clutterRemoval 1 \r\n",
"mpdBoundaryBox 1 -3.5 0 0 3.5 0 3 \r\n",
"mpdBoundaryBox 2 -3.5 0 3.51 7 0 3 \r\n",
"mpdBoundaryBox 3 0.01 3.5 0 3.5 0 3 \r\n",
"mpdBoundaryBox 4 0.01 3.5 3.51 7 0 3 \r\n",
"minorStateCfg 4 3 12 8 5 20 4 20 \r\n",
"clusterCfg 1 1 2 \r\n"
#endif
};

#endif

uint8_t gMmwL3[L3_MEM_SIZE]  __attribute((section(".l3")));

/*! Local RAM buffer for object detection DPC */
uint8_t gMmwCoreLocMem[MMWDEMO_OBJDET_CORE_LOCAL_MEM_SIZE];

/*! Local RAM buffer for tracker */
uint8_t gMmwCoreLocMem2[MMWDEMO_OBJDET_CORE_LOCAL_MEM2_SIZE];

extern float gSocClk;

MMWave_temperatureStats  tempStats;

extern void Mmwave_populateDefaultCalibrationCfg (MMWave_CalibrationCfg* ptrCalibrationCfg);

extern void MMWave_getTemperatureReport (MMWave_temperatureStats* ptrTempStats);

extern int32_t CLI_ByPassApi(CLI_Cfg* ptrCLICfg, uint8_t profileSwitch);

int32_t mmwDemo_mmWaveInit(bool iswarmstrt);
#if (ENABLE_MONITORS==1)
/*API to get Results of RF Monitors*/
extern void mmwDemo_GetMonRes(void);
#endif
// Default Config task
// Priority of this task
#define DEFAULT_CFG_TASK_PRI  (2u)
// Stack for Power task
#define DEFAULT_CFG_TASK_SIZE (256u)
StackType_t gDefCfgTaskStack[DEFAULT_CFG_TASK_SIZE] __attribute__((aligned(32)));
// Power task objects
StaticTask_t gDefCfgTaskObj;
TaskHandle_t gDefCfgTask;
#if (CLI_REMOVAL == 0 && QUICK_START == 1)
// Task function
void CLI_defaultcfg_task(void* args);
#endif

StaticTask_t gmmwinitTaskObj;
TaskHandle_t gmmwinitTask;
extern void mmwreinitTask(void *args);
StackType_t gmmwinitTaskStack[MMWINIT_TASK_SIZE] __attribute__((aligned(32)));
StaticSemaphore_t gmmwinitObj;
SemaphoreHandle_t gmmwinit;

uint8_t gIsDefCfgUsed = 0;

StackType_t gPowerTaskStack[POWER_TASK_SIZE] __attribute__((aligned(32)));
// Power task objects
StaticTask_t gPowerTaskObj;
TaskHandle_t gPowerTask;
//Power task function
void powerManagementTask(void *args);
// Power task semaphore objects
StaticSemaphore_t gPowerSemObj;
SemaphoreHandle_t gPowerSem;

//For Sensor Stop
uint32_t sensorStop = 0;
extern int8_t isSensorStarted;

// For freeing the channels after Sensor Stop
void mmwDemo_freeDmaChannels(EDMA_Handle edmaHandle);

// LED config
uint32_t gpioBaseAddrLed, pinNumLed;
extern volatile unsigned long long demoStartTime;
volatile unsigned long long demoEndTime, slpTimeus,lpdsLatency;
double demoTimeus,frmPrdus;
extern TaskHandle_t gDpcTask;
extern CLI_MCB     gCLI;
// char bootRst[6][15] = {"PORZ", "Warm Reset", "Deep Sleep", "Soft Reset", "STC_WARM", "STC_PORZ"};

/**
*  @b Description
*  @n
*        Function configuring and executing DPC
*/
void mmwDemo_dpcTask()
{
    
    DPC_Config();

    DPC_Execute();

    /* Never return for this task. */
    SemaphoreP_pend(&gMmwMssMCB.TestSemHandle, SystemP_WAIT_FOREVER);
}

#if (CLI_REMOVAL == 0 && DYNAMIC_RECONFIG == 1)
/**
 *  @b Description
 *  @n
 *    Function to calculate the chirping parameters based on the switched configuration
 */
void mmwDemo_ChirpConfigSwitch_Calc()
{
    uint8_t i;
    float scale, rfBandwidth, rampDownTime;
    /* Calculation of channel configuration: 1 1 0 */
    if((gMmwMssMCB.channelCfg.h_RxChCtrlBitMask & 2) == 2)
    {
        gMmwMssMCB.angleDimension = 2;
    }
    else
    {
        gMmwMssMCB.angleDimension = 1;
    }

    gMmwMssMCB.numRxAntennas = 0;
    gMmwMssMCB.numTxAntennas = 0;
    for (i=0; i<16; i++)
    {
        if((gMmwMssMCB.channelCfg.h_TxChCtrlBitMask >> i) & 0x1)
        {
            gMmwMssMCB.numTxAntennas++;
        }
        if((gMmwMssMCB.channelCfg.h_RxChCtrlBitMask >> i) & 0x1)
        {
            gMmwMssMCB.rxAntOrder[gMmwMssMCB.numRxAntennas] = (uint8_t) i;
            gMmwMssMCB.numRxAntennas++;
        }
    }

    /* Calculation of Chirp Common configuration: 21 0 0 128 1 34 0 */
    gMmwMssMCB.adcSamplingRate = 100.0/gMmwMssMCB.profileComCfg.c_DigOutputSampRate; //Range 1MHz to 12.5MHz
    gMmwMssMCB.numRangeBins = mathUtils_pow2roundup(gMmwMssMCB.profileComCfg.h_NumOfAdcSamples)/2; //Real only sampling

    if (gMmwMssMCB.profileComCfg.c_ChirpTxMimoPatSel == 1 || gMmwMssMCB.profileComCfg.c_ChirpTxMimoPatSel == 0)
    {
        /* TDM-MIMO*/
        gMmwMssMCB.isBpmEnabled = 0;
    }
    else if (gMmwMssMCB.profileComCfg.c_ChirpTxMimoPatSel == 4)
    {
        /* BPM-MIMO*/
        gMmwMssMCB.isBpmEnabled = 1;
    }

    /* Calculate the Chirp Timing configuration: 8 30 0 102.98 59 */
#ifdef SOC_XWRL64XX
    gMmwMssMCB.profileTimeCfg.xh_ChirpRfFreqSlope  = (gMmwMssMCB.chirpSlope * 1048576.0)/(3* 100 * 100);
    gMmwMssMCB.profileTimeCfg.w_ChirpRfFreqStart   = (gMmwMssMCB.startFreq * 1000.0 * 256.0)/(300);
#else
    gMmwMssMCB.profileTimeCfg.xh_ChirpRfFreqSlope  = (gMmwMssMCB.chirpSlope * 1048576.0)/(4* 100 * 100);
    gMmwMssMCB.profileTimeCfg.w_ChirpRfFreqStart   = (gMmwMssMCB.startFreq * 1000.0 * 256.0)/(400);
#endif

    /* Calculate the frame configuration: 2 0 200 128 70 0 */
    gMmwMssMCB.frameCfg.w_BurstPeriodicity        = 10.0 * gMmwMssMCB.burstPeriod;

    /* Miscellaneous calculation */
#ifdef SOC_XWRL64XX
    scale = 65536./(3*100*100);
#else
    scale = 65536./(4*100*100);
#endif
    rfBandwidth = (gMmwMssMCB.profileComCfg.h_ChirpRampEndTime*0.1) * gMmwMssMCB.chirpSlope; //In MHz/usec
    rampDownTime = MIN((gMmwMssMCB.profileTimeCfg.h_ChirpIdleTime*0.1-1.0), 6.0); //In usec
    gMmwMssMCB.profileComCfg.h_CrdNSlopeMag = (uint16_t)  fabs((scale * rfBandwidth / rampDownTime + 0.5));
    gMmwMssMCB.profileTimeCfg.h_ChirpTxEnSel       = gMmwMssMCB.channelCfg.h_TxChCtrlBitMask;
    gMmwMssMCB.profileTimeCfg.h_ChirpTxBpmEnSel    = 0U;

    gMmwMssMCB.channelCfg.c_MiscCtrl = 1U << M_RL_RF_MISC_CTRL_RDIF_CLK;
}

/**
 *  @b Description
 *  @n
 *    Function to store the initial user provided CLI configurations
 */
void mmwDemo_UserConfigStore()
{
    if(gUserCfgPresenceTrack)
    {
        prDetectParams.channelCfg                          = gMmwMssMCB.channelCfg;
        prDetectParams.frameCfg                            = gMmwMssMCB.frameCfg;
        prDetectParams.profileComCfg                       = gMmwMssMCB.profileComCfg;
        prDetectParams.profileTimeCfg                      = gMmwMssMCB.profileTimeCfg;
        prDetectParams.sigProcChainCfg                     = gMmwMssMCB.sigProcChainCfg;
        prDetectParams.cfarCfg                             = gMmwMssMCB.cfarCfg;
        prDetectParams.fovCfg                              = gMmwMssMCB.fovCfg;
        prDetectParams.rangeSelCfg                         = gMmwMssMCB.rangeSelCfg;
        prDetectParams.steeringVecCorrCfg                  = gMmwMssMCB.steeringVecCorrCfg;
        prDetectParams.staticClutterRemovalEnable          = gMmwMssMCB.staticClutterRemovalEnable;
        prDetectParams.guiMonSel                           = gMmwMssMCB.guiMonSel;
        prDetectParams.majorStateParamCfg                  = gMmwMssMCB.majorStateParamCfg;
        prDetectParams.minorStateParamCfg                  = gMmwMssMCB.minorStateParamCfg;
        prDetectParams.clusterParamCfg                     = gMmwMssMCB.clusterParamCfg;
        prDetectParams.sceneryParams                       = gMmwMssMCB.sceneryParams;
        prDetectParams.enableMajorMotion                   = gMmwMssMCB.enableMajorMotion;
        prDetectParams.startFreq                           = gMmwMssMCB.startFreq;
        prDetectParams.chirpSlope                          = gMmwMssMCB.chirpSlope;
        prDetectParams.burstPeriod                         = gMmwMssMCB.burstPeriod;
    }
    else
    {
        trDetectParams.channelCfg                          = gMmwMssMCB.channelCfg;
        trDetectParams.frameCfg                            = gMmwMssMCB.frameCfg;
        trDetectParams.profileComCfg                       = gMmwMssMCB.profileComCfg;
        trDetectParams.profileTimeCfg                      = gMmwMssMCB.profileTimeCfg;
        trDetectParams.sigProcChainCfg                     = gMmwMssMCB.sigProcChainCfg;
        trDetectParams.trackerCfg                          = gMmwMssMCB.trackerCfg;
        trDetectParams.microDopplerCliCfg                  = gMmwMssMCB.microDopplerCliCfg;
        trDetectParams.microDopplerClassifierCliCfg        = gMmwMssMCB.microDopplerClassifierCliCfg;
        trDetectParams.guiMonSel                           = gMmwMssMCB.guiMonSel;
        trDetectParams.cfarCfg                             = gMmwMssMCB.cfarCfg;
        trDetectParams.fovCfg                              = gMmwMssMCB.fovCfg;
        trDetectParams.rangeSelCfg                         = gMmwMssMCB.rangeSelCfg;
        trDetectParams.steeringVecCorrCfg                  = gMmwMssMCB.steeringVecCorrCfg;
        trDetectParams.staticClutterRemovalEnable          = gMmwMssMCB.staticClutterRemovalEnable;
        trDetectParams.enableMajorMotion                   = gMmwMssMCB.enableMajorMotion;
        trDetectParams.startFreq                           = gMmwMssMCB.startFreq;
        trDetectParams.chirpSlope                          = gMmwMssMCB.chirpSlope;
        trDetectParams.burstPeriod                         = gMmwMssMCB.burstPeriod;
        gMmwMssMCB.isMotionPresenceDpuEnabled = 0;
    }
}

/**
 *  @b Description
 *  @n
 *    Function to reconfigure the demo chain to tracker specific configurations,
 *    to enable profile switching from Presence detection to Tracker
 */
void mmwDemo_PresenceToTrackSwitch()
{
    if(gOneTimeSwitch)
    {
        CLI_ByPassApi(&gCLI.cfg, 1);
        trDetectParams.channelCfg                          = gMmwMssMCB.channelCfg;
        trDetectParams.frameCfg                            = gMmwMssMCB.frameCfg;
        trDetectParams.profileComCfg                       = gMmwMssMCB.profileComCfg;
        trDetectParams.profileTimeCfg                      = gMmwMssMCB.profileTimeCfg;
        trDetectParams.sigProcChainCfg                     = gMmwMssMCB.sigProcChainCfg;
        trDetectParams.trackerCfg                          = gMmwMssMCB.trackerCfg;
        trDetectParams.microDopplerCliCfg                  = gMmwMssMCB.microDopplerCliCfg;
        trDetectParams.microDopplerClassifierCliCfg        = gMmwMssMCB.microDopplerClassifierCliCfg;
        trDetectParams.guiMonSel                           = gMmwMssMCB.guiMonSel;
        trDetectParams.cfarCfg                             = gMmwMssMCB.cfarCfg;
        trDetectParams.fovCfg                              = gMmwMssMCB.fovCfg;
        trDetectParams.rangeSelCfg                         = gMmwMssMCB.rangeSelCfg;
        trDetectParams.steeringVecCorrCfg                  = gMmwMssMCB.steeringVecCorrCfg;
        trDetectParams.staticClutterRemovalEnable          = gMmwMssMCB.staticClutterRemovalEnable;
        trDetectParams.enableMajorMotion                   = gMmwMssMCB.enableMajorMotion;
        trDetectParams.startFreq                           = gMmwMssMCB.startFreq;
        trDetectParams.chirpSlope                          = gMmwMssMCB.chirpSlope;
        trDetectParams.burstPeriod                         = gMmwMssMCB.burstPeriod;
        gOneTimeSwitch = 0;
    }
    else
    {
       gMmwMssMCB.channelCfg                          = trDetectParams.channelCfg;
       gMmwMssMCB.frameCfg                            = trDetectParams.frameCfg;
       gMmwMssMCB.profileComCfg                       = trDetectParams.profileComCfg;
       gMmwMssMCB.profileTimeCfg                      = trDetectParams.profileTimeCfg;
       gMmwMssMCB.sigProcChainCfg                     = trDetectParams.sigProcChainCfg;
       gMmwMssMCB.trackerCfg                          = trDetectParams.trackerCfg;
       gMmwMssMCB.microDopplerCliCfg                  = trDetectParams.microDopplerCliCfg;
       gMmwMssMCB.microDopplerClassifierCliCfg        = trDetectParams.microDopplerClassifierCliCfg;
       gMmwMssMCB.guiMonSel                           = trDetectParams.guiMonSel;
       gMmwMssMCB.cfarCfg                             = trDetectParams.cfarCfg;
       gMmwMssMCB.fovCfg                              = trDetectParams.fovCfg;
       gMmwMssMCB.rangeSelCfg                         = trDetectParams.rangeSelCfg;
       gMmwMssMCB.steeringVecCorrCfg                  = trDetectParams.steeringVecCorrCfg;
       gMmwMssMCB.staticClutterRemovalEnable          = trDetectParams.staticClutterRemovalEnable;
       gMmwMssMCB.enableMajorMotion                   = trDetectParams.enableMajorMotion;
       gMmwMssMCB.startFreq                           = trDetectParams.startFreq;
       gMmwMssMCB.chirpSlope                          = trDetectParams.chirpSlope;
       gMmwMssMCB.burstPeriod                         = trDetectParams.burstPeriod;
    }

    gMmwMssMCB.isMotionPresenceDpuEnabled = 0;

    mmwDemo_ChirpConfigSwitch_Calc();
}

/**
 *  @b Description
 *  @n
 *    Function to reconfigure the demo chain to presence detect specific configurations, 
 *    to enable profile switching from Tracker to Presence detection
 */
void mmwDemo_TrackToPresenceSwitch()
{
    if(gOneTimeSwitch)
    {
        CLI_ByPassApi(&gCLI.cfg, 2);
        prDetectParams.channelCfg                          = gMmwMssMCB.channelCfg;
        prDetectParams.frameCfg                            = gMmwMssMCB.frameCfg;
        prDetectParams.profileComCfg                       = gMmwMssMCB.profileComCfg;
        prDetectParams.profileTimeCfg                      = gMmwMssMCB.profileTimeCfg;
        prDetectParams.sigProcChainCfg                     = gMmwMssMCB.sigProcChainCfg;
        prDetectParams.cfarCfg                             = gMmwMssMCB.cfarCfg;
        prDetectParams.fovCfg                              = gMmwMssMCB.fovCfg;
        prDetectParams.rangeSelCfg                         = gMmwMssMCB.rangeSelCfg;
        prDetectParams.steeringVecCorrCfg                  = gMmwMssMCB.steeringVecCorrCfg;
        prDetectParams.staticClutterRemovalEnable          = gMmwMssMCB.staticClutterRemovalEnable;
        prDetectParams.guiMonSel                           = gMmwMssMCB.guiMonSel;
        prDetectParams.majorStateParamCfg                  = gMmwMssMCB.majorStateParamCfg;
        prDetectParams.minorStateParamCfg                  = gMmwMssMCB.minorStateParamCfg;
        prDetectParams.clusterParamCfg                     = gMmwMssMCB.clusterParamCfg;
        prDetectParams.sceneryParams                       = gMmwMssMCB.sceneryParams;
        prDetectParams.enableMajorMotion                   = gMmwMssMCB.enableMajorMotion;
        prDetectParams.startFreq                           = gMmwMssMCB.startFreq;
        prDetectParams.chirpSlope                          = gMmwMssMCB.chirpSlope;
        prDetectParams.burstPeriod                         = gMmwMssMCB.burstPeriod;
        gOneTimeSwitch = 0;
    }
    else
    {
        gMmwMssMCB.channelCfg                          = prDetectParams.channelCfg;
        gMmwMssMCB.frameCfg                            = prDetectParams.frameCfg;
        gMmwMssMCB.profileComCfg                       = prDetectParams.profileComCfg;
        gMmwMssMCB.profileTimeCfg                      = prDetectParams.profileTimeCfg;
        gMmwMssMCB.sigProcChainCfg                     = prDetectParams.sigProcChainCfg;
        gMmwMssMCB.cfarCfg                             = prDetectParams.cfarCfg;
        gMmwMssMCB.fovCfg                              = prDetectParams.fovCfg;
        gMmwMssMCB.rangeSelCfg                         = prDetectParams.rangeSelCfg;
        gMmwMssMCB.steeringVecCorrCfg                  = prDetectParams.steeringVecCorrCfg;
        gMmwMssMCB.staticClutterRemovalEnable          = prDetectParams.staticClutterRemovalEnable;
        gMmwMssMCB.guiMonSel                           = prDetectParams.guiMonSel;
        gMmwMssMCB.majorStateParamCfg                  = prDetectParams.majorStateParamCfg;
        gMmwMssMCB.minorStateParamCfg                  = prDetectParams.minorStateParamCfg;
        gMmwMssMCB.clusterParamCfg                     = prDetectParams.clusterParamCfg;
        gMmwMssMCB.sceneryParams                       = prDetectParams.sceneryParams;
        gMmwMssMCB.enableMajorMotion                   = prDetectParams.enableMajorMotion;
        gMmwMssMCB.startFreq                           = prDetectParams.startFreq;
        gMmwMssMCB.chirpSlope                          = prDetectParams.chirpSlope;
        gMmwMssMCB.burstPeriod                         = prDetectParams.burstPeriod;
    }
    gMmwMssMCB.isMotionPresenceDpuEnabled           = 1;
    gMmwMssMCB.trackerCfg.staticCfg.trackerEnabled  = 0;
    gMmwMssMCB.microDopplerCliCfg.enabled           = 0;
    gMmwMssMCB.microDopplerClassifierCliCfg.enabled = 0;
    
    mmwDemo_ChirpConfigSwitch_Calc();
}

/**
 *  @b Description
 *  @n
 *    Function to implement the state machine to monitor the presence detect 
 *    or tracking states and enable profile switching based on the user programmed thresholds
 */
void mmwDemo_ProfileSwitchStateMachine()
{
    int16_t i = 0;
    int16_t state = 0;
    for(i = 0; i < (gMmwMssMCB.sceneryParams.numBoundaryBoxes/4.0); i++)
    {
        if(*(gMmwMssMCB.dpcZoneState + ((i+1)*sizeof(uint8_t))))
        {
            state = 1;
            break;
        }
    }
    if(state && gMmwMssMCB.isMotionPresenceDpuEnabled && gPresenceToTrackSwitchCntr >= gMmwMssMCB.profileSwitchCfg.frmPretoTrack)
    {
        gFlagPresenceDetect = 1;
        gPresenceToTrackSwitchCntr = 1;
    }
    else if(state && gMmwMssMCB.isMotionPresenceDpuEnabled && gPresenceToTrackSwitchCntr < gMmwMssMCB.profileSwitchCfg.frmPretoTrack)
    {
        gFlagPresenceDetect = 0;
        gPresenceToTrackSwitchCntr += 1;
    }
    else
    {
        gFlagPresenceDetect = 0;
    }

    if(gFlagPresenceDetect)
    {
        gTrackToPresenceSwitchCntr = 1;
    }
    else if(gMmwMssMCB.dpcResult.trackerOutParams.numTargets == 0 && gMmwMssMCB.trackerCfg.staticCfg.trackerEnabled)
    {
        gTrackToPresenceSwitchCntr += 1;
    }
    else if(gMmwMssMCB.dpcResult.trackerOutParams.numTargets > 0 && gMmwMssMCB.trackerCfg.staticCfg.trackerEnabled)
    {
        gTrackToPresenceSwitchCntr = 1;
    }
    else
    {
        /* Do Nothing */
    }
}
#endif


void *classifier_malloc(uint32_t sizeInBytes)
{
    return HeapP_alloc(&gMmwMssMCB.CoreLocalFeatExtractHeapObj, sizeInBytes);
}
void classifier_free(void *pFree, uint32_t sizeInBytes)
{
    HeapP_free(&gMmwMssMCB.CoreLocalFeatExtractHeapObj, pFree);
}

/**
 *  @b Description
 *  @n
 *     UART write wrapper function
 *
 * @param[in]   handle          UART handle
 * @param[in]   payload         Pointer to payload data
 * @param[in]   payloadLength   Payload length in bytes
 *
 *  @retval
 *      Not Applicable.
 */
void mmw_UartWrite (UART_Handle handle,
                    uint8_t *payload,
                    uint32_t payloadLength)
{
    UART_Transaction trans;

    UART_Transaction_init(&trans);

    trans.buf   = payload;
    trans.count = payloadLength;

    UART_write(handle, &trans);
}


void mmwDemo_INAMeasNull(I2C_Handle i2cHandle, uint16_t *ptrPwrMeasured)
{
    ptrPwrMeasured[0] = (uint16_t)0xFFFF;
    ptrPwrMeasured[1] = (uint16_t)0xFFFF;
    ptrPwrMeasured[2] = (uint16_t)0xFFFF;
    ptrPwrMeasured[3] = (uint16_t)0xFFFF;
}




/** @brief Transmits detection data over UART
*
*    The following data is transmitted:
*    1. Header (size = 40bytes), including "Magic word", (size = 8 bytes)
*       and including the number of TLV items
*    TLV Items:
*    2. If pointCloud flag is 1 or 2, DPIF_PointCloudCartesian structure containing
*       X,Y,Z location and velocity for detected objects,
*       size = sizeof(DPIF_PointCloudCartesian) * number of detected objects
*    3. If pointCloud flag is 1, DPIF_PointCloudSideInfo structure containing SNR
*       and noise for detected objects,
*       size = sizeof(DPIF_PointCloudCartesian) * number of detected objects
*    4. If rangeProfile flag is set,  rangeProfile,
*       size = number of range bins * sizeof(uint32_t)
*    5. noiseProfile flag is set is not used.
*    6. If rangeAzimuthHeatMap flag is set, sends range/azimuth heatmap, size = number of range bins *
*       number of azimuth bins * sizeof(uint32_t)
*    7. rangeDopplerHeatMap flag is not used
*    8. If statsInfo flag is set, the stats information, timing, temperature and power
*/
void mmwDemo_TransmitProcessedOutputTask()
{
    UART_Handle uartHandle = gUartHandle[0];
    I2C_Handle  i2cHandle = gI2cHandle[CONFIG_I2C0];
    DPC_ObjectDetection_ExecuteResult *result = &gMmwMssMCB.dpcResult;
    //MmwDemo_output_message_stats      *timingInfo
    MmwDemo_output_message_header header;
    CLI_GuiMonSel   *pGuiMonSel;
    uint32_t tlvIdx = 0;
    //uint32_t i;
    uint32_t numPaddingBytes;
    uint32_t packetLen;
    uint8_t padding[MMWDEMO_OUTPUT_MSG_SEGMENT_LEN];
    MmwDemo_output_message_tl   tl[MMWDEMO_OUTPUT_ALL_MSG_MAX];
    uint8_t trackerEnabled;
    uint8_t microDopplerEnabled;
    uint8_t classifierEnabled;
    uint8_t mpdEnabled;
    uint32_t numTargets, numIndices, numDopplerBins;
    uint32_t numClassifiedTargets;
    uint8_t     *tList;
    uint8_t     *tIndex;

    uint8_t     *uDopplerData;
    uint8_t     *uDopplerFeatures;
    uint8_t     *uClassifierOutput;
    //uint8_t    index;
    TimerP_Params idleTimerParams;
    static uint32_t rangemmrndold = 0;
    uint32_t rangemmrnd = 0;
    uint32_t *rngProfile;
    uint32_t rngProfileMax = 0;
    uint16_t binIdx;
    uint32_t txUartus = 0;
    uint32_t uartTxStartTime;
    uint32_t timeElapsedus;
    uint32_t framePeriodicityus; 
    uint32_t timeRemainUart;

    /* Get Gui Monitor configuration */
    pGuiMonSel = &gMmwMssMCB.guiMonSel;

    trackerEnabled = gMmwMssMCB.trackerCfg.staticCfg.trackerEnabled;
    microDopplerEnabled = gMmwMssMCB.microDopplerCliCfg.enabled;
    classifierEnabled = gMmwMssMCB.microDopplerClassifierCliCfg.enabled;
    mpdEnabled = gMmwMssMCB.isMotionPresenceDpuEnabled;

    while(true)
    {
        SemaphoreP_pend(&gMmwMssMCB.tlvSemHandle, SystemP_WAIT_FOREVER);
        /*****************************************************************************************************/
        /* If Default Config: LED is blinked at frequency proportional to range of nearest detection */
        if(gIsDefCfgUsed == 1)
        {
            rngProfile = gMmwMssMCB.rangeProfile[1];
            if (gTestMinMpdCentroid != -1)
            {   
                for(binIdx=1; binIdx < round(gMmwMssMCB.rangeSelCfg.max/gMmwMssMCB.rangeStep); binIdx++)
                {
                    if(binIdx == 1)
                    {
                        rngProfileMax = rngProfile[binIdx];
                        rangemmrnd = binIdx;
                    }
                    else if(rngProfile[binIdx] > rngProfileMax)
                    {
                        rngProfileMax = rngProfile[binIdx];
                        rangemmrnd = binIdx;
                    }
                }

                gTestRangeLED = rangemmrnd * gMmwMssMCB.rangeStep;

                if(fabsf(gTestMinMpdCentroid - gTestRangeLED) <= gMmwMssMCB.clusterParamCfg.maxDistance)
                {
                    rangemmrnd = round(rangemmrnd * gMmwMssMCB.rangeStep * 10) * 100;
                }
                else
                {
                    rangemmrnd = round(gTestMinMpdCentroid * 10) * 100;
                }
                
                if(rangemmrndold != rangemmrnd)
                {
                    if(rangemmrnd > 100)
                    {
                        // Configure RTI Counter 0
                        TimerP_Params_init(&idleTimerParams);
                        // Selecting Osc clock as source for RTI with prescale 1.
                        idleTimerParams.inputPreScaler = 1;
                        idleTimerParams.inputClkHz     = 40000000u;
                        idleTimerParams.periodInUsec   = rangemmrnd * 500;
                        idleTimerParams.oneshotMode    = 0;
                        idleTimerParams.enableOverflowInt = 1;
                        TimerP_setup(CSL_APP_RTIA_U_BASE, &idleTimerParams);
                        TimerP_start(CSL_APP_RTIA_U_BASE);
                    }
                    else
                    {
                        TimerP_stop(CSL_APP_RTIA_U_BASE);
                        GPIO_pinWriteHigh(gpioBaseAddrLed, pinNumLed);
                    }   
                    rangemmrndold = rangemmrnd;
                }
            }
            else
            {
                TimerP_stop(CSL_APP_RTIA_U_BASE);
                rangemmrndold = 0;
                GPIO_pinWriteLow(gpioBaseAddrLed, pinNumLed);
            }
        }
        /*************************************************************************************************/
            
        tlvIdx = 0;

        /* Clear message header */
        memset((void *)&header, 0, sizeof(MmwDemo_output_message_header));
        /* Header: */
        header.platform =  0xA6432;
        header.magicWord[0] = 0x0102;
        header.magicWord[1] = 0x0304;
        header.magicWord[2] = 0x0506;
        header.magicWord[3] = 0x0708;
        header.numDetectedObj = result->numObjOut;
        header.version =    MMWAVE_SDK_VERSION_BUILD |   //DEBUG_VERSION
                            (MMWAVE_SDK_VERSION_BUGFIX << 8) |
                            (MMWAVE_SDK_VERSION_MINOR << 16) |
                            (MMWAVE_SDK_VERSION_MAJOR << 24);

        /* Tracker information */
        numTargets = result->trackerOutParams.numTargets;
        numIndices = result->trackerOutParams.numIndices;
        tList      = (uint8_t*) result->trackerOutParams.tList;
        tIndex     = (uint8_t*) result->trackerOutParams.targetIndex;
        numClassifiedTargets = result->microDopplerOutParams.numClassifiedTargets;

        uDopplerData = (uint8_t*) result->microDopplerOutParams.uDopplerOutput;
        uDopplerFeatures = (uint8_t*) result->microDopplerOutParams.uDopplerFeatures;
        uClassifierOutput = (uint8_t*) &result->microDopplerOutParams.classifierOutput;

        numDopplerBins = result->microDopplerOutParams.numDopplerBins;

        packetLen = sizeof(MmwDemo_output_message_header);
        if ((pGuiMonSel->pointCloud == 1) && (result->numObjOut > 0))
        {
            tl[tlvIdx].type = MMWDEMO_OUTPUT_MSG_DETECTED_POINTS;
            tl[tlvIdx].length = sizeof(DPIF_PointCloudCartesian) * result->numObjOut;
            packetLen += sizeof(MmwDemo_output_message_tl) + tl[tlvIdx].length;
            tlvIdx++;
        }

        /* Side info */
        if ((pGuiMonSel->pointCloud == 1) && result->numObjOut > 0)
        {
            tl[tlvIdx].type = MMWDEMO_OUTPUT_MSG_DETECTED_POINTS_SIDE_INFO;
            tl[tlvIdx].length = sizeof(DPIF_PointCloudSideInfo) * result->numObjOut;
            packetLen += sizeof(MmwDemo_output_message_tl) + tl[tlvIdx].length;
            tlvIdx++;
        }

        /* Point Cloud Compressed format */
        if ((pGuiMonSel->pointCloud == 2) && (result->numObjOut > 0))
        {
            tl[tlvIdx].type = MMWDEMO_OUTPUT_EXT_MSG_DETECTED_POINTS;
            tl[tlvIdx].length = sizeof(MmwDemo_output_message_point_unit) +
                                sizeof(MmwDemo_output_message_UARTpoint) * result->numObjOut;
            packetLen += sizeof(MmwDemo_output_message_tl) + tl[tlvIdx].length;
            tlvIdx++;
        }

        /* List of virtual antenna elements of detected points */
        if (gMmwMssMCB.steeringVecCorrCfg.enableAntSymbGen)
        {
            if ((pGuiMonSel->pointCloudAntennaSymbols == 1) && (result->numObjOut > 0))
            {
                tl[tlvIdx].type = MMWDEMO_OUTPUT_EXT_MSG_POINT_CLOUD_ANTENNA_SYMBOLS;
                tl[tlvIdx].length = sizeof(DPU_AoasvcProc_VirtualAntennaElements) * result->numObjOut;
                packetLen += sizeof(MmwDemo_output_message_tl) + tl[tlvIdx].length;
                tlvIdx++;
            }
        }

        /* Range Profile (Major motion) */
        if ((pGuiMonSel->rangeProfile & 0x1) && (gMmwMssMCB.rangeProfile[0] != NULL))
        {
            tl[tlvIdx].type = MMWDEMO_OUTPUT_EXT_MSG_RANGE_PROFILE_MAJOR;
            tl[tlvIdx].length = sizeof(uint32_t) * gMmwMssMCB.numRangeBins;
            packetLen += sizeof(MmwDemo_output_message_tl) + tl[tlvIdx].length;
            tlvIdx++;
        }
        /* Range Profile (Minor motion) */
        if ((pGuiMonSel->rangeProfile & 0x2) && (gMmwMssMCB.rangeProfile[1] != NULL))
        {
            tl[tlvIdx].type = MMWDEMO_OUTPUT_EXT_MSG_RANGE_PROFILE_MINOR;
            tl[tlvIdx].length = sizeof(uint32_t) * gMmwMssMCB.numRangeBins;
            packetLen += sizeof(MmwDemo_output_message_tl) + tl[tlvIdx].length;
            tlvIdx++;
        }

        /* Range-Azimuth Heatmap (Major motion) */
        if ((pGuiMonSel->rangeAzimuthHeatMap & 0x1) && (result->rngAzHeatMap[0] != NULL))
        {
            tl[tlvIdx].type = MMWDEMO_OUTPUT_EXT_MSG_RANGE_AZIMUT_HEAT_MAP_MAJOR;
            tl[tlvIdx].length = gMmwMssMCB.numRangeBins * gMmwMssMCB.sigProcChainCfg.azimuthFftSize * sizeof(uint32_t);
            packetLen += sizeof(MmwDemo_output_message_tl) +  tl[tlvIdx].length;
            tlvIdx++;
        }
        /* Range-Azimuth Heatmap (Minor motion) */
        if ((pGuiMonSel->rangeAzimuthHeatMap & 0x2) && (result->rngAzHeatMap[1] != NULL))
        {
            tl[tlvIdx].type = MMWDEMO_OUTPUT_EXT_MSG_RANGE_AZIMUT_HEAT_MAP_MINOR;
            tl[tlvIdx].length = gMmwMssMCB.numRangeBins * gMmwMssMCB.sigProcChainCfg.azimuthFftSize * sizeof(uint32_t);
            packetLen += sizeof(MmwDemo_output_message_tl) +  tl[tlvIdx].length;
            tlvIdx++;
        }

        if (pGuiMonSel->statsInfo)
        {
            memcpy((void*)gMmwMssMCB.outStats.tempReading, &tempStats.tempValue, (4 * sizeof(int16_t)));
            
            #ifdef INA228
            if(gMmwMssMCB.spiADCStream != 1)
            {
                mmwDemo_PowerMeasurement(i2cHandle, &gMmwMssMCB.outStats.powerMeasured[0]);
            }
            else
            {
                mmwDemo_INAMeasNull(i2cHandle, &gMmwMssMCB.outStats.powerMeasured[0]);
            }
            #else
            mmwDemo_INAMeasNull(i2cHandle, &gMmwMssMCB.outStats.powerMeasured[0]);
            #endif
            tl[tlvIdx].type = MMWDEMO_OUTPUT_EXT_MSG_STATS;
            tl[tlvIdx].length = sizeof(MmwDemo_output_message_stats);
            packetLen += sizeof(MmwDemo_output_message_tl) +  tl[tlvIdx].length;
            tlvIdx++;
        }

        if (pGuiMonSel->presenceInfo && mpdEnabled)
        {
            tl[tlvIdx].type = MMWDEMO_OUTPUT_EXT_MSG_ENHANCED_PRESENCE_INDICATION;
            tl[tlvIdx].length = ((uint8_t)ceil(gMmwMssMCB.sceneryParams.numBoundaryBoxes/4.0) + 1) * sizeof(uint8_t);
            packetLen += sizeof(MmwDemo_output_message_tl) + tl[tlvIdx].length;
            tlvIdx++;
        }

        if (pGuiMonSel->adcSamples)
        {
            tl[tlvIdx].type = MMWDEMO_OUTPUT_EXT_MSG_ADC_SAMPLES;
            tl[tlvIdx].length = gMmwMssMCB.numRxAntennas * gMmwMssMCB.numTxAntennas * gMmwMssMCB.profileComCfg.h_NumOfAdcSamples * sizeof(int16_t);
            packetLen += sizeof(MmwDemo_output_message_tl) +  tl[tlvIdx].length;
            tlvIdx++;
        }

        if (trackerEnabled && pGuiMonSel->trackerInfo)
        {
            if (numTargets > 0)
            {
                tl[tlvIdx].type = MMWDEMO_OUTPUT_EXT_MSG_TARGET_LIST;
                tl[tlvIdx].length = numTargets * sizeof(trackerProc_Target);
                packetLen += sizeof(MmwDemo_output_message_tl) + tl[tlvIdx].length;
                tlvIdx++;
            }
            if ((numIndices > 0) && (numTargets > 0))
            {
                tl[tlvIdx].type = MMWDEMO_OUTPUT_EXT_MSG_TARGET_INDEX;
                tl[tlvIdx].length = numIndices*sizeof(trackerProc_TargetIndex);
                packetLen += sizeof(MmwDemo_output_message_tl) + tl[tlvIdx].length;
                tlvIdx++;
            }
        }

        if (trackerEnabled && microDopplerEnabled)
        {
            if (pGuiMonSel->trackerInfo && pGuiMonSel->microDopplerInfo)
            {
                if (numTargets > 0)
                {
                    tl[tlvIdx].type = MMWDEMO_OUTPUT_EXT_MSG_MICRO_DOPPLER_RAW_DATA;
                    tl[tlvIdx].length = numTargets * numDopplerBins * sizeof(float);
                    packetLen += sizeof(MmwDemo_output_message_tl) + tl[tlvIdx].length;
                    tlvIdx++;

                    tl[tlvIdx].type = MMWDEMO_OUTPUT_EXT_MSG_MICRO_DOPPLER_FEATURES;
                    tl[tlvIdx].length = numTargets * sizeof(FeatExtract_featOutput);
                    packetLen += sizeof(MmwDemo_output_message_tl) + tl[tlvIdx].length;
                    tlvIdx++;
                }
            }
        }

        if (trackerEnabled && microDopplerEnabled && classifierEnabled)
        {
            if (pGuiMonSel->classifierInfo)
            {
                if (numClassifiedTargets > 0)
                {
                    tl[tlvIdx].type = MMWDEMO_OUTPUT_EXT_MSG_CLASSIFIER_INFO;
                    tl[tlvIdx].length = numClassifiedTargets * sizeof(DPU_uDopProc_classifierPrediction);
                    packetLen += sizeof(MmwDemo_output_message_tl) + tl[tlvIdx].length;
                    tlvIdx++;
                }
            }
        }

        if (pGuiMonSel->quickEvalInfo)
        {
            /* The quick eval plots the presence info and is not useful without it being enabled */
            if (pGuiMonSel->presenceInfo && mpdEnabled)
            {
                tl[tlvIdx].type = MMWDEMO_OUTPUT_EXT_MSG_QUICK_EVAL_INFO;
                tl[tlvIdx].length = sizeof(gMmwMssMCB.sceneryParams) + sizeof(gIsDefCfgUsed);
                packetLen += sizeof(MmwDemo_output_message_tl) + tl[tlvIdx].length;
                tlvIdx++;
            }
            else
            {
                tl[tlvIdx].type = MMWDEMO_OUTPUT_EXT_MSG_QUICK_EVAL_INFO;
                tl[tlvIdx].length = sizeof(gIsDefCfgUsed);
                packetLen += sizeof(MmwDemo_output_message_tl) + tl[tlvIdx].length;
                tlvIdx++;
            }
        }

        if (gMmwMssMCB.measureRxChannelBiasCliCfg.enabled)
        {
            tl[tlvIdx].type = MMWDEMO_OUTPUT_EXT_MSG_RX_CHAN_COMPENSATION_INFO;
            tl[tlvIdx].length = sizeof(DPU_DoaProc_compRxChannelBiasFloatCfg);
            packetLen += sizeof(MmwDemo_output_message_tl) + tl[tlvIdx].length;
            tlvIdx++;
        }

                // VS Output
        if (gMmwMssMCB.guiMonSel.pointCloud == 2)
        {
            tl[tlvIdx].type   = MMWDEMO_OUTPUT_MSG_VS;
            tl[tlvIdx].length = sizeof(vitalSignsOutput);
            packetLen += sizeof(MmwDemo_output_message_tl) + tl[tlvIdx].length;
            tlvIdx++;
        }

        /* Fill header */
        header.numTLVs = tlvIdx;
        /* Round up packet length to multiple of MMWDEMO_OUTPUT_MSG_SEGMENT_LEN */
        header.totalPacketLen = MMWDEMO_OUTPUT_MSG_SEGMENT_LEN *
                ((packetLen + (MMWDEMO_OUTPUT_MSG_SEGMENT_LEN-1))/MMWDEMO_OUTPUT_MSG_SEGMENT_LEN);
        header.timeCpuCycles =  0; //TODO: Populate with actual time
        header.frameNumber = gMmwMssMCB.stats.frameStartIntCounter;
        header.subFrameNumber = -1;

            
        /* Check if there is enough time for all TLVs*/
        uartTxStartTime = Cycleprofiler_getTimeStamp();
        timeElapsedus = (gMmwMssMCB.outStats.interFrameProcessingTime + gMmwMssMCB.stats.chirpingTime_us + ((uartTxStartTime - gMmwMssMCB.stats.interFrameEndTimeStamp)/FRAME_REF_TIMER_CLOCK_MHZ));
        framePeriodicityus = (gMmwMssMCB.frameCfg.w_FramePeriodicity/gSocClk)*1000000;
        if(gUartParams[0].baudRate == 115200)
        {
            txUartus = header.totalPacketLen * TIME_TO_SEND_1BYTE_DATA_WITH_BAUDRATE_115200_us;
        }
#ifdef  ENABLE_UART_HIGH_BAUD_RATE_DYNAMIC_CFG
        else if(gUartParams[0].baudRate == 1250000)
        {
            txUartus = header.totalPacketLen * TIME_TO_SEND_1BYTE_DATA_WITH_BAUDRATE_1250000_us;
        }
#endif
        timeRemainUart = framePeriodicityus - timeElapsedus;
        if(gMmwMssMCB.lowPowerMode == LOW_PWR_MODE_ENABLE)
        {   
            // Idle power mode has least threshold of all power modes hence considering
            uint32_t idleThreshold = Power_getThresholds(POWER_IDLE);
            timeRemainUart = timeRemainUart - idleThreshold;
        }
              
        if(txUartus >= timeRemainUart)
        {
             CLI_write ("\r\n Warning: Frame Time is not enough to transfer all the configured TLVs!!! \r\n");
        }
        else
        {
            if(tlvIdx != 0)
            {
                mmw_UartWrite (uartHandle, (uint8_t*)&header, sizeof(MmwDemo_output_message_header));
                tlvIdx = 0;
            }

            /* Send detected Objects */
            if ((pGuiMonSel->pointCloud == 1) && (result->numObjOut > 0))
            {
                mmw_UartWrite (uartHandle,
                                (uint8_t*)&tl[tlvIdx],
                                sizeof(MmwDemo_output_message_tl));

                /*Send array of objects */
                mmw_UartWrite (uartHandle, (uint8_t*)result->objOut,
                                sizeof(DPIF_PointCloudCartesian) * result->numObjOut);
                tlvIdx++;
            }

            /* Send detected Objects Side Info */
            if ((pGuiMonSel->pointCloud == 1) && (result->numObjOut > 0))
            {
                mmw_UartWrite (uartHandle,
                                (uint8_t*)&tl[tlvIdx],
                                sizeof(MmwDemo_output_message_tl));

                /*Send array of objects */
                mmw_UartWrite (uartHandle, (uint8_t*)result->objOutSideInfo,
                                sizeof(DPIF_PointCloudSideInfo) * result->numObjOut);
                tlvIdx++;
            }

            /* Send Point Cloud Compressed format */
            if ((pGuiMonSel->pointCloud == 2) && (result->numObjOut > 0))
            {
                /*Send point cloud */
                gMmwMssMCB.pointCloudToUart.header = tl[tlvIdx];
                mmw_UartWrite (uartHandle, (uint8_t*)&gMmwMssMCB.pointCloudToUart,
                            sizeof(MmwDemo_output_message_tl) + tl[tlvIdx].length);
                tlvIdx++;
            }

            /* Send List of virtual antenna elements of detected points */
            if (gMmwMssMCB.steeringVecCorrCfg.enableAntSymbGen)
            {
                if ((pGuiMonSel->pointCloudAntennaSymbols == 1) && (result->numObjOut > 0))
                {
                    mmw_UartWrite (uartHandle,
                                    (uint8_t*)&tl[tlvIdx],
                                    sizeof(MmwDemo_output_message_tl));
                    mmw_UartWrite (uartHandle, (uint8_t*)gMmwMssMCB.virtAntElemList,
                                    sizeof(DPU_AoasvcProc_VirtualAntennaElements) * result->numObjOut);
                    tlvIdx++;
                }
            }

            /* Send Range profile (Major mode) */
            if ((pGuiMonSel->rangeProfile & 0x1) && (gMmwMssMCB.rangeProfile[0] != NULL))
            {
                mmw_UartWrite (uartHandle,
                                (uint8_t*)&tl[tlvIdx],
                                sizeof(MmwDemo_output_message_tl));

                mmw_UartWrite (uartHandle,
                                (uint8_t*)gMmwMssMCB.rangeProfile[0],
                                (sizeof(uint32_t)*(mathUtils_pow2roundup(gMmwMssMCB.profileComCfg.h_NumOfAdcSamples)/2)));
                tlvIdx++;
            }
            /* Send Range profile (Minor mode) */
            if ((pGuiMonSel->rangeProfile & 0x2) && (gMmwMssMCB.rangeProfile[1] != NULL))
            {
                mmw_UartWrite (uartHandle,
                                (uint8_t*)&tl[tlvIdx],
                                sizeof(MmwDemo_output_message_tl));

                mmw_UartWrite (uartHandle,
                                (uint8_t*)gMmwMssMCB.rangeProfile[1],
                                (sizeof(uint32_t)*(mathUtils_pow2roundup(gMmwMssMCB.profileComCfg.h_NumOfAdcSamples)/2)));
                tlvIdx++;
            }

            /* Send Range-Azimuth Heatmap (Major motion) */
            if ((pGuiMonSel->rangeAzimuthHeatMap & 0x1) && (result->rngAzHeatMap[0] != NULL))
            {
                mmw_UartWrite (uartHandle,
                                (uint8_t*)&tl[tlvIdx],
                                sizeof(MmwDemo_output_message_tl));

                mmw_UartWrite (uartHandle,
                        (uint8_t *) result->rngAzHeatMap[0],
                        (gMmwMssMCB.numRangeBins * gMmwMssMCB.sigProcChainCfg.azimuthFftSize * sizeof(uint32_t)));

                tlvIdx++;
            }
            /* Send Range-Azimuth Heatmap (Minor motion) */
            if ((pGuiMonSel->rangeAzimuthHeatMap & 0x2) && (result->rngAzHeatMap[1] != NULL))
            {
                mmw_UartWrite (uartHandle,
                                (uint8_t*)&tl[tlvIdx],
                                sizeof(MmwDemo_output_message_tl));

                mmw_UartWrite (uartHandle,
                        (uint8_t *) result->rngAzHeatMap[1],
                        (gMmwMssMCB.numRangeBins * gMmwMssMCB.sigProcChainCfg.azimuthFftSize * sizeof(uint32_t)));

                tlvIdx++;
            }

            /* Send stats information (interframe processing time and uart transfer time) */
            if (pGuiMonSel->statsInfo)
            {
                mmw_UartWrite (uartHandle,
                                (uint8_t*)&tl[tlvIdx],
                                sizeof(MmwDemo_output_message_tl));

                mmw_UartWrite (uartHandle,
                            (uint8_t*) &gMmwMssMCB.outStats,
                            tl[tlvIdx].length);
                tlvIdx++;
            }

            /* Send motion, presence detected for each zone (0th index - No. of zones processed, 1st index onwards - 2bits state per zone from LSB) */
            if (pGuiMonSel->presenceInfo && mpdEnabled)
            {
                mmw_UartWrite (uartHandle,
                                (uint8_t*)&tl[tlvIdx],
                                sizeof(MmwDemo_output_message_tl));

                mmw_UartWrite (uartHandle,
                            (uint8_t*) gMmwMssMCB.dpcZoneState,
                            tl[tlvIdx].length);
                tlvIdx++;
            }
            /* Send ADC samples of last chirp pair in the frame */
            if (pGuiMonSel->adcSamples)
            {
                CSL_app_hwa_adcbuf_ctrlRegs *ptrAdcBufCtrlRegs = (CSL_app_hwa_adcbuf_ctrlRegs *)CSL_APP_HWA_ADCBUF_CTRL_U_BASE;

                mmw_UartWrite (uartHandle,
                                (uint8_t*)&tl[tlvIdx],
                                sizeof(MmwDemo_output_message_tl));

                if (gMmwMssMCB.numTxAntennas == 2)
                {
                    /* Set view to ADC ping buffer */
                    CSL_FINS(ptrAdcBufCtrlRegs->ADCBUFCFG1, APP_HWA_ADCBUF_CTRL_ADCBUFCFG1_ADCBUFCFG1_ADCBUFPIPOOVRCNT, 1);

                    mmw_UartWrite (uartHandle,
                                (uint8_t*) CSL_APP_HWA_ADCBUF_RD_U_BASE,
                                tl[tlvIdx].length/2);

                    /* Set view to ADC pong buffer */
                    CSL_FINS(ptrAdcBufCtrlRegs->ADCBUFCFG1, APP_HWA_ADCBUF_CTRL_ADCBUFCFG1_ADCBUFCFG1_ADCBUFPIPOOVRCNT, 0);

                    mmw_UartWrite (uartHandle,
                                (uint8_t*) CSL_APP_HWA_ADCBUF_RD_U_BASE,
                                tl[tlvIdx].length/2);
                }
                else
                {
                    /* Only one Tx antenna from ping buffer */
                    /* Set view to ADC ping buffer */
                    CSL_FINS(ptrAdcBufCtrlRegs->ADCBUFCFG1, APP_HWA_ADCBUF_CTRL_ADCBUFCFG1_ADCBUFCFG1_ADCBUFPIPOOVRCNT, 1);

                    mmw_UartWrite (uartHandle,
                                (uint8_t*) CSL_APP_HWA_ADCBUF_RD_U_BASE,
                                tl[tlvIdx].length);

                    /* Set view to ADC pong buffer */
                    CSL_FINS(ptrAdcBufCtrlRegs->ADCBUFCFG1, APP_HWA_ADCBUF_CTRL_ADCBUFCFG1_ADCBUFCFG1_ADCBUFPIPOOVRCNT, 0);

                }
                tlvIdx++;

            }

            /* Send Group Tracker output */
            if (trackerEnabled && pGuiMonSel->trackerInfo)
            {
                if (numTargets > 0)
                {
                    mmw_UartWrite (uartHandle,
                                    (uint8_t*)&tl[tlvIdx],
                                    sizeof(MmwDemo_output_message_tl));
                    mmw_UartWrite (uartHandle,
                                    (uint8_t*)tList,
                                    tl[tlvIdx].length);
                    tlvIdx++;
                }
                if ((numIndices > 0) && (numTargets > 0))
                {
                    mmw_UartWrite (uartHandle,
                                    (uint8_t*)&tl[tlvIdx],
                                    sizeof(MmwDemo_output_message_tl));
                    mmw_UartWrite (uartHandle,
                                    (uint8_t*)tIndex,
                                    tl[tlvIdx].length);
                    tlvIdx++;
                }
            }

            /* Send microDoppler output */
            if (trackerEnabled && microDopplerEnabled)
            {
                if (pGuiMonSel->trackerInfo && pGuiMonSel->microDopplerInfo)
                {
                    if (numTargets > 0)
                    {
                        /* Micro Doppler raw data */
                        mmw_UartWrite (uartHandle,
                                        (uint8_t*)&tl[tlvIdx],
                                        sizeof(MmwDemo_output_message_tl));
                        mmw_UartWrite (uartHandle,
                                        (uint8_t*)uDopplerData,
                                        tl[tlvIdx].length);
                        tlvIdx++;

                        /* Micro Doppler features */
                        mmw_UartWrite (uartHandle,
                                        (uint8_t*)&tl[tlvIdx],
                                        sizeof(MmwDemo_output_message_tl));
                        mmw_UartWrite (uartHandle,
                                        (uint8_t*)uDopplerFeatures,
                                        tl[tlvIdx].length);
                        tlvIdx++;
                    }
                }
            }

            /* Send Classifier output - predictions */
            if (trackerEnabled && microDopplerEnabled && classifierEnabled)
            {
                if (pGuiMonSel->classifierInfo)
                {
                    if (numClassifiedTargets > 0)
                    {
                        /* Classifier output - predictions */
                        mmw_UartWrite (uartHandle,
                                        (uint8_t*)&tl[tlvIdx],
                                        sizeof(MmwDemo_output_message_tl));
                        mmw_UartWrite (uartHandle,
                                        (uint8_t*)uClassifierOutput,
                                        tl[tlvIdx].length);
                        tlvIdx++;
                    }
                }
            }

            if (pGuiMonSel->quickEvalInfo)
            {
                /* The quick eval plots the presence info and is not useful without it being enabled */
                if (pGuiMonSel->presenceInfo && mpdEnabled)
                {
                    mmw_UartWrite (uartHandle,
                                        (uint8_t*)&tl[tlvIdx],
                                        sizeof(MmwDemo_output_message_tl));

                    mmw_UartWrite (uartHandle,
                                        (uint8_t*)&gMmwMssMCB.sceneryParams,
                                        sizeof(gMmwMssMCB.sceneryParams));

                    mmw_UartWrite (uartHandle,
                                        (uint8_t*)&gIsDefCfgUsed,
                                        sizeof(gIsDefCfgUsed));

                    tlvIdx++;
                }
                else
                {
                    mmw_UartWrite (uartHandle,
                                        (uint8_t*)&tl[tlvIdx],
                                        sizeof(MmwDemo_output_message_tl));

                    mmw_UartWrite (uartHandle,
                                        (uint8_t*)&gIsDefCfgUsed,
                                        sizeof(gIsDefCfgUsed));
                    tlvIdx++;
                }
            }
            
            /* Send Rx Channel compensation coefficients */
            if (gMmwMssMCB.measureRxChannelBiasCliCfg.enabled)
            {
                mmw_UartWrite (uartHandle,
                                (uint8_t*)&tl[tlvIdx],
                                sizeof(MmwDemo_output_message_tl));
                mmw_UartWrite (uartHandle,
                                (uint8_t*)&gMmwMssMCB.compRxChannelBiasCfgMeasureOut,
                                tl[tlvIdx].length);
                tlvIdx++;
            }

            if (gMmwMssMCB.guiMonSel.pointCloud == 2)
            {
                mmw_UartWrite(uartHandle,
                          (uint8_t *)&tl[tlvIdx],
                          sizeof(MmwDemo_output_message_tl));

                /*Send array of objects */
                mmw_UartWrite(uartHandle, (uint8_t *)&vitalSignsOutput, sizeof(vitalSignsOutput));
                tlvIdx++;
            }

            if(tlvIdx != 0)
            {
                /* Send padding bytes */
                numPaddingBytes = MMWDEMO_OUTPUT_MSG_SEGMENT_LEN - (packetLen & (MMWDEMO_OUTPUT_MSG_SEGMENT_LEN-1));
                if (numPaddingBytes < MMWDEMO_OUTPUT_MSG_SEGMENT_LEN)
                {
                    mmw_UartWrite (uartHandle, (uint8_t*)padding, numPaddingBytes);
                }
            }    
        }
        /* Flush UART buffer here for each frame. */
        UART_flushTxFifo(uartHandle);

        /* End of UART data transmission */
        gMmwMssMCB.stats.uartTransferEndTimeStamp = Cycleprofiler_getTimeStamp();
        gMmwMssMCB.outStats.transmitOutputTime = (gMmwMssMCB.stats.uartTransferEndTimeStamp - gMmwMssMCB.stats.interFrameEndTimeStamp)/FRAME_REF_TIMER_CLOCK_MHZ;
        gMmwMssMCB.stats.totalActiveTime_us = (gMmwMssMCB.stats.uartTransferEndTimeStamp - gMmwMssMCB.stats.frameStartTimeStamp)/FRAME_REF_TIMER_CLOCK_MHZ;

        //Interframe processing and UART data transmission completed
        gMmwMssMCB.interSubFrameProcToken--;

        // Capture the time elaspsed till here
        demoEndTime = PRCMSlowClkCtrGet();
        // Demo Run time for one frame (From Sensor Start till Completion of UART transmit)
        demoTimeus = (demoEndTime - demoStartTime)*(30.5);

        #if (CLI_REMOVAL == 0)
        if (gMmwMssMCB.adcDataSourceCfg.source == 1 || gMmwMssMCB.adcDataSourceCfg.source == 2)
        {
            demoTimeus = 0;
        }
        #endif

        if((gMmwMssMCB.lowPowerMode == LOW_PWR_MODE_ENABLE) || (gMmwMssMCB.lowPowerMode == LOW_PWR_TEST_MODE))
        {
            xSemaphoreGive(gPowerSem);
            /* Enable Power Management Policy if Low Power Mode is enabled */
            if(gMmwMssMCB.lowPowerMode == LOW_PWR_MODE_ENABLE)
            {
                Power_enablePolicy();
            }
        }
        if (gMmwMssMCB.lowPowerMode == LOW_PWR_MODE_DISABLE)
        {
            #if (CLI_REMOVAL == 0)
            if(gMmwMssMCB.adcDataSourceCfg.source == 1 || gMmwMssMCB.adcDataSourceCfg.source == 2)
            {
                /* In test mode trigger next frame processing */
                SemaphoreP_post(&gMmwMssMCB.adcFileTaskSemHandle);
            }
            #endif
            // Important Note: Sensor Stop command is honoured only when Low Power Cfg is disabled
            if(sensorStop == 1)
            {
                MmwDemo_stopSensor();
            }
        }
    }
}

int32_t mmwDemo_mmWaveInit(bool iswarmstrt)
{
    int32_t             errCode;
    int32_t             retVal = SystemP_SUCCESS;
    MMWave_InitCfg      initCfg;
    MMWave_ErrorLevel   errorLevel;
    int16_t             mmWaveErrorCode;
    int16_t             subsysErrorCode;

    /* Initialize the mmWave control init configuration */
    memset ((void*)&initCfg, 0, sizeof(MMWave_InitCfg));

    /* Is Warm Start? */
    initCfg.iswarmstart = iswarmstrt;

    /* Initialize and setup the mmWave Control module */
    gMmwMssMCB.ctrlHandle = MMWave_init (&initCfg, &errCode);
    if (gMmwMssMCB.ctrlHandle == NULL)
    {
        /* Error: Unable to initialize the mmWave control module */
        MMWave_decodeError (errCode, &errorLevel, &mmWaveErrorCode, &subsysErrorCode);

        /* Error: Unable to initialize the mmWave control module */
        CLI_write ("Error: mmWave Control Initialization failed [Error code %d] [errorLevel %d] [mmWaveErrorCode %d] [subsysErrorCode %d]\n", errCode, errorLevel, mmWaveErrorCode, subsysErrorCode);
        retVal = SystemP_FAILURE;
    }
    /* FECSS RF Power ON*/
    if(initCfg.iswarmstart)
    {
        errCode = rl_fecssRfPwrOnOff(M_DFP_DEVICE_INDEX_0, &gMmwMssMCB.channelCfg);
        if(errCode != M_DFP_RET_CODE_OK)
        {
            CLI_write ("Error: FECSS RF PowerON failed [Error code %d] \r\n", errCode);
            retVal = SystemP_FAILURE;
        }   
    }

    return retVal;
}

/**
 *  @b Description
 *  @n
 *      mmw demo helper Function to start sensor.
 *
 *  @retval
 *      Success     - 0
 *  @retval
 *      Error       - <0
 */
int32_t MmwDemo_startSensor(void)
{
#if !(CONTINUOUS_WAVE_MODE_ENABLE) /* suppress warning */
    int32_t     errCode;
#endif
    MMWave_CalibrationCfg   calibrationCfg;


    /*****************************************************************************
     * RF :: now start the RF and the real time ticking
     *****************************************************************************/
    /* Initialize the calibration configuration: */
    memset ((void *)&calibrationCfg, 0, sizeof(MMWave_CalibrationCfg));
    /* Populate the calibration configuration: */
    Mmwave_populateDefaultCalibrationCfg (&calibrationCfg);

    DebugP_logInfo("App: MMWave_start Issued\n");

#if !(CONTINUOUS_WAVE_MODE_ENABLE) /* disable mmWave_start for continousMode CW */
    /* Start the mmWave module: The configuration has been applied successfully. */
    if (MMWave_start(gMmwMssMCB.ctrlHandle, &calibrationCfg, &gMmwMssMCB.sensorStart, &errCode) < 0)
    {
        MMWave_ErrorLevel   errorLevel;
        int16_t             mmWaveErrorCode;
        int16_t             subsysErrorCode;

        /* Error/Warning: Unable to start the mmWave module */
        MMWave_decodeError (errCode, &errorLevel, &mmWaveErrorCode, &subsysErrorCode);
        CLI_write ("Error: mmWave Start failed [mmWave Error: %d Subsys: %d]\n", mmWaveErrorCode, subsysErrorCode);
        /* datapath has already been moved to start state; so either we initiate a cleanup of start sequence or
           assert here and re-start from the beginning. For now, choosing the latter path */
        MmwDemo_debugAssert(0);
        return -1;
    }
#endif
    return 0;
}

/**
 *  @b Description
 *  @n
 *      MMW Demo helper Function to Stop the Sensor. Sensor Stop in honoured only when Low Power Mode is disabled.
 *
 *  @retval
 *      None
 */
void MmwDemo_stopSensor(void)
{
    int32_t err;
    // Stop and Close the front end
    MMWave_stop(gMmwMssMCB.ctrlHandle,&err);
    MMWave_close(gMmwMssMCB.ctrlHandle,&err);
    // Delete the exisitng profile as we receive a new configuration
    MMWave_delProfile(gMmwMssMCB.ctrlHandle,gMmwMssMCB.mmwCtrlCfg.frameCfg[0].profileHandle[0],&err);
    // Free up all the edma channels and close the EDMA interface 
    mmwDemo_freeDmaChannels(gEdmaHandle[0]);
    Drivers_edmaClose();
    EDMA_deinit();
    // Demo Stopped
    rangeProcHWAObj* temp = gMmwMssMCB.rangeProcDpuHandle;
    temp->inProgress = false;
    gMmwMssMCB.oneTimeConfigDone = 0;
    // Re-init the EDMA interface for next configuration
    EDMA_init();
    Drivers_edmaOpen();
    gMmwMssMCB.stats.frameStartIntCounter = 0;
    sensorStop = 0;
    isSensorStarted = 0;
    // If Stopping Default Config.
    if( gIsDefCfgUsed == 1)
    {
        gIsDefCfgUsed = 0;
    }

    // Delete the DPC, TLV as we will create them again in next configuration when we start
    vTaskDelete(gDpcTask);
    vTaskDelete(NULL);
}

/**
 *  @b Description
 *  @n
 *      mmw demo helper Function to do one time sensor initialization.
 *      User need to fill gMmwMssMCB.mmwOpenCfg before calling this function
 *
 *  @retval
 *      Success     - 0
 *  @retval
 *      Error       - <0
 */
int32_t MmwDemo_openSensor(void)
{
    int32_t             errCode;
    MMWave_ErrorLevel   errorLevel;
    int16_t             mmWaveErrorCode;
    int16_t             subsysErrorCode;

    /**********************************************************
     **********************************************************/

    /* Open mmWave module, this is only done once */

    /* Open the mmWave module: */
    if (MMWave_open (gMmwMssMCB.ctrlHandle, &gMmwMssMCB.mmwOpenCfg, &errCode) < 0)
    {
        /* Error: decode and Report the error */
        MMWave_decodeError (errCode, &errorLevel, &mmWaveErrorCode, &subsysErrorCode);
        CLI_write ("Error: mmWave Open failed [Error code: %d Subsystem: %d]\n",
                        mmWaveErrorCode, subsysErrorCode);
        return -1;
    }

    return 0;
}

/**
 *  @b Description
 *  @n
 *      MMW demo helper Function to configure sensor. User need to fill gMmwMssMCB.mmwCtrlCfg and
 *      add profiles/chirp to mmWave before calling this function
 *
 *  @retval
 *      Success     - 0
 *  @retval
 *      Error       - <0
 */
int32_t MmwDemo_configSensor(void)
{
    int32_t     errCode = 0;

    /* Configure the mmWave module: */
    if (MMWave_config (gMmwMssMCB.ctrlHandle, &gMmwMssMCB.mmwCtrlCfg, &errCode) < 0)
    {
        MMWave_ErrorLevel   errorLevel;
        int16_t             mmWaveErrorCode;
        int16_t             subsysErrorCode;

        /* Error: Report the error */
        MMWave_decodeError (errCode, &errorLevel, &mmWaveErrorCode, &subsysErrorCode);
        CLI_write ("Error: mmWave Config failed [Error code: %d Subsystem: %d]\n",
                        mmWaveErrorCode, subsysErrorCode);
        goto exit;
    }

exit:
    return errCode;
}





void vitalsign_with_tracking(void* args)
{
    int32_t errorCode = SystemP_SUCCESS;
    int32_t retVal = -1;

    /* Peripheral Driver Initialization */
    Drivers_open();
    Board_driversOpen();
    
    // Get the Version of Device.
    pgVersion = SOC_getEfusePgVersion();

    // Configure the LED GPIO
    gpioBaseAddrLed = (uint32_t) AddrTranslateP_getLocalAddr(GPIO_LED_BASE_ADDR);
    pinNumLed       = GPIO_LED_PIN;
    GPIO_setDirMode(gpioBaseAddrLed, pinNumLed, GPIO_LED_DIR);

    /*HWASS_SHRD_RAM, TPCCA and TPCCB memory have to be init before use. */
    /*APPSS SHRAM0 and APPSS SHRAM1 memory have to be init before use. However, for awrL varients these are initialized by RBL */
    /*FECSS SHRAM (96KB) has to be initialized before use as RBL does not perform initialization.*/
    SOC_memoryInit(SOC_RCM_MEMINIT_HWA_SHRAM_INIT|SOC_RCM_MEMINIT_TPCCA_INIT|SOC_RCM_MEMINIT_TPCCB_INIT|SOC_RCM_MEMINIT_FECSS_SHRAM_INIT|SOC_RCM_MEMINIT_APPSS_SHRAM0_INIT|SOC_RCM_MEMINIT_APPSS_SHRAM1_INIT);
    
    /* Is the device 6432 AOP */
    #ifdef SOC_XWRL64XX
    gMmwMssMCB.isDevAOP = SOC_isDeviceAOP();
    #endif

    gMmwMssMCB.commandUartHandle = gUartHandle[0];

    /* mmWave initialization*/
    mmwDemo_mmWaveInit(0);

    /* Initialize default antenna geometry */
    memcpy((void *) &gMmwMssMCB.antennaGeometryCfg, (void *) &gDefaultAntGeometry, sizeof(MmwDemo_antennaGeometryCfg));

    gmmwinitTask = xTaskCreateStatic( mmwreinitTask,      /* Pointer to the function that implements the task. */
                                  "mmwinit",          /* Text name for the task.  This is to facilitate debugging only. */
                                  MMWINIT_TASK_SIZE,  /* Stack depth in units of StackType_t typically uint32_t on 32b CPUs */
                                  NULL,            /* We are not using the task parameter. */
                                  MMWINITTASK_PRI,   /* task priority, 0 is lowest priority, configMAX_PRIORITIES-1 is highest */
                                  gmmwinitTaskStack,  /* pointer to stack base */
                                  &gmmwinitTaskObj ); /* pointer to statically allocated task object memory */
    gmmwinit = xSemaphoreCreateBinaryStatic(&gmmwinitObj);

    // Radar Power Management Framework: Create a Task for Power Management Framework
    gPowerTask = xTaskCreateStatic( powerManagementTask,      /* Pointer to the function that implements the task. */
                                  "power",          /* Text name for the task.  This is to facilitate debugging only. */
                                  POWER_TASK_SIZE,  /* Stack depth in units of StackType_t typically uint32_t on 32b CPUs */
                                  NULL,            /* We are not using the task parameter. */
                                  POWER_TASK_PRI,   /* task priority, 0 is lowest priority, configMAX_PRIORITIES-1 is highest */
                                  gPowerTaskStack,  /* pointer to stack base */
                                  &gPowerTaskObj ); /* pointer to statically allocated task object memory */
                                  
    // Radar Power Management Framework: Create Semaphore for to pend Power Task
    gPowerSem = xSemaphoreCreateBinaryStatic(&gPowerSemObj);

    /* Create binary semaphore to pend Main task, */
    SemaphoreP_constructBinary(&gMmwMssMCB.demoInitTaskCompleteSemHandle, 0);

    errorCode = SemaphoreP_constructBinary(&gMmwMssMCB.cliInitTaskCompleteSemHandle, 0);
    DebugP_assert(SystemP_SUCCESS == errorCode);

    errorCode = SemaphoreP_constructBinary(&gMmwMssMCB.TestSemHandle, 0);
    DebugP_assert(SystemP_SUCCESS == errorCode);

    errorCode = SemaphoreP_constructBinary(&gMmwMssMCB.tlvSemHandle, 0);
    DebugP_assert(SystemP_SUCCESS == errorCode);

    errorCode = SemaphoreP_constructBinary(&gMmwMssMCB.adcFileTaskSemHandle, 0);
    DebugP_assert(SystemP_SUCCESS == errorCode);

#if (CLI_REMOVAL==1)
    errorCode = SemaphoreP_constructBinary(&gMmwMssMCB.dpcCfgDoneSemHandle, 0);
    DebugP_assert(SystemP_SUCCESS == errorCode);
#endif

#if (ENABLE_MONITORS==1)
    /*Creating Semaphore for Monitors*/
    errorCode = SemaphoreP_constructBinary(&gMmwMssMCB.rfmonSemHandle, 0);
    DebugP_assert(SystemP_SUCCESS == errorCode);
#endif
   /* Initialize Flash interface. */
    retVal = mmwDemo_flashInit();
    if (retVal < 0)
    {
        CLI_write("Error: Flash Initialization Failed!\r\n");
        MmwDemo_debugAssert (0);
    }
    /* Check if the device is RF-Trimmed */
    /* Checking one Trim is enough */
    if(SOC_rcmReadSynthTrimValid() == RF_SYNTH_TRIM_VALID)
    {
        gMmwMssMCB.factoryCalCfg.atecalibinEfuse = true;
    }
    else
    {
        gMmwMssMCB.factoryCalCfg.atecalibinEfuse = false;
        CLI_write("Error: Device is not RF-Trimmed!\r\n");
        MmwDemo_debugAssert (0);
    }

    /* DPC initialization*/
    DPC_Init();

    CLI_init(CLI_TASK_PRIORITY);

    #if (CLI_REMOVAL == 0 && QUICK_START == 1)
    // Create a Task for running default configuration
    gDefCfgTask = xTaskCreateStatic( CLI_defaultcfg_task,      /* Pointer to the function that implements the task. */
                                  "Run_Defaultcfg",          /* Text name for the task.  This is to facilitate debugging only. */
                                  DEFAULT_CFG_TASK_SIZE,  /* Stack depth in units of StackType_t typically uint32_t on 32b CPUs */
                                  NULL,            /* We are not using the task parameter. */
                                  DEFAULT_CFG_TASK_PRI,   /* task priority, 0 is lowest priority, configMAX_PRIORITIES-1 is highest */
                                  gDefCfgTaskStack,  /* pointer to stack base */
                                  &gDefCfgTaskObj ); /* pointer to statically allocated task object memory */
    #endif
    
    /* Never return for this task. */
    SemaphoreP_pend(&gMmwMssMCB.demoInitTaskCompleteSemHandle, SystemP_WAIT_FOREVER);

    Board_driversClose();
    Drivers_close();
}





// Free all the allocated EDMA channels
void mmwDemo_freeDmaChannels(EDMA_Handle edmaHandle)
{
    uint32_t   index;
    uint32_t  dmaCh, tcc, pram, shadow;
    for(index = 0; index < 64; index++)
    {
        dmaCh = index;
        tcc = index;
        pram = index;
        shadow = index;
        DPEDMA_freeEDMAChannel(edmaHandle, &dmaCh, &tcc, &pram, &shadow);
    }
    for(index = 0; index < 128; index++)
    {
        shadow = index;
        DebugP_assert(EDMA_freeParam(edmaHandle, &shadow) == SystemP_SUCCESS);
    }
    return;
}
