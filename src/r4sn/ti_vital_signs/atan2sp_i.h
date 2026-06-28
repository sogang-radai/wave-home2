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

/* =========================================================================== */
/* atan2sp_i.h - single precision floating point arctangent two arguement      */
/*               optimized inlined C implementation (w/ intrinsics)            */
/* =========================================================================== */

#ifndef ATAN2SP_I_H_
#define ATAN2SP_I_H_ 1

float tan_input;
float tan_output;

static inline float atan2f_sr1i_atan2spi(float g1, float pih, int s, int bn, int an)
{
    float coef;
    float g2;
    float g4;
    float g6;
    float g8;
    float g10;
    float g12;
    float pol;
    float tmp1;
    float tmp2;
    float c1 = 0.00230158202f;
    float c2 = -0.01394551000f;
    float c3 = 0.03937087815f;
    float c4 = -0.07235669163f;
    float c5 = 0.10521499322f;
    float c6 = -0.14175076797f;
    float c7 = 0.19989300877f;
    float c8 = -0.33332930041f;
    int   ns_nbn;

    /* get coef based on the flags */
    coef = pih;
    if (!s)
    {
        coef = 3.1415927f;
    }

    ns_nbn = s | bn;

    if (!ns_nbn)
    {
        coef = 0;
    }
    if (an)
    {
        coef = -coef;
    }

    /* calculate polynomial */
    g2  = g1 * g1;
    g4  = g2 * g2;
    g6  = g2 * g4;
    g8  = g4 * g4;
    g10 = g6 * g4;
    g12 = g8 * g4;

    tmp1 = ((c5 * g8) + (c6 * g6)) + ((c7 * g4) + (c8 * g2));
    tmp2 = (((c1 * g4 + c2 * g2) + c3) * g12) + (c4 * g10);

    pol = tmp1 + tmp2;
    pol = pol * g1 + g1;

    return (s ? (coef - pol) : (coef + pol));
}


static inline float atan2sp_i(float a, float b)
{
    float g, x, y;
    float res;
    float temp;
    float pih = 1.570796327f;
    float pi  = 3.141592741f;
    float Max = 3.402823466E+38F;
    int   an;
    int   bn;
    int   s = 0;
    float a_tmp, b_tmp;

    x  = a;
    y  = b;
    an = (a < 0); /* flag for a negative */
    bn = (b < 0); /* flag for b negative */

    /* swap a and b before calling division sub routine if a > b */
    if (a < 0)
    {
        a_tmp = -a;
    }
    else
    {
        a_tmp = a;
    }

    if (b < 0)
    {
        b_tmp = -b;
    }
    else
    {
        b_tmp = b;
    }


    if (a_tmp > b_tmp)
    {
        temp = b;
        b    = a;
        a    = temp;
        s    = 1; /* swap flag */
    }

    g         = a / b;
    tan_input = g;
    /* do polynomial estimation */
    res = atan2f_sr1i_atan2spi(g, pih, s, bn, an);

    tan_output = res;
    // switch the returns so that the answer is equivalent
    if (x == 0.0f)
    {
        res = (y >= 0.0f ? 0 : pi);
    }
    if (g > Max)
    {
        res = pih;
    }
    if (g < -Max)
    {
        res = -pih;
    }

    return (res);
}

#endif /* ATAN2SP_I_H_ */

/* ======================================================================== */
/*  End of file: atan2sp_i.h                          	                 	  */
/* ======================================================================== */
