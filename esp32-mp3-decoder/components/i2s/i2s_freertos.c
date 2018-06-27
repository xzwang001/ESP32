
// Portions of this file come from the esp31-smsemu project
// Here is the copyright for these parts:

/******************************************************************************
 * Copyright 2013-2015 Espressif Systems
 *
 * FileName: i2s_freertos.c
 *
 * Description: I2S output routines for a FreeRTOS system. Uses DMA and a queue
 * to abstract away the nitty-gritty details.
 *
 * Modification history:
 *     2015/06/01, v1.0 File created.
 *     2015/12/17, Adapted for ESP31
*******************************************************************************/

// Rest of the functions are licensed under Apache license as found below:

// Copyright 2015-2016 Pewit Ltd.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

/*
How does this work? Basically, to get sound, you need to:
- Connect an I2S codec to the I2S pins on the ESP.
- Start up a thread that's going to do the sound output
- Call I2sInit()
- Call I2sSetRate() with the sample rate you want.
- Generate sound and call i2sPushSample() with 32-bit samples.
The 32bit samples basically are 2 16-bit signed values (the analog values for
the left and right channel) concatenated as (Rout<<16)+Lout

I2sPushSample will block when you're sending data too quickly, so you can just
generate and push data as fast as you can and I2sPushSample will regulate the
speed.
*/

#include "soc/i2s_reg.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "driver/gpio.h"
#include "soc/gpio_sig_map.h"
#include "rom/lldesc.h"
#include "esp_intr.h"
#include "driver/periph_ctrl.h"
#include "soc/gpio_sig_map.h"
#include "soc/gpio_reg.h"
#include "rom/gpio.h"
#include "i2s_freertos.h"

#include <stdio.h>

//Pointer to the I2S DMA buffer data
static uint8_t *i2sBuf[I2SDMABUFCNT];
//I2S DMA buffer descriptors
static lldesc_t i2sBufDesc[I2SDMABUFCNT];
//Queue which contains empty DMA buffers
static xQueueHandle dmaQueue;
//DMA underrun counter
static long underrunCnt;

static intr_handle_t ih;

//This routine is called as soon as the DMA routine has something to tell us. All we
//handle here is the RX_EOF_INT status, which indicate the DMA has sent a buffer whose
//descriptor has the 'EOF' field set to 1.
static void IRAM_ATTR i2s_isr(void* arg) {
	portBASE_TYPE HPTaskAwoken=0;

    lldesc_t *finishedDesc;
	uint32_t slc_intr_status;
	int dummy;

	slc_intr_status = READ_PERI_REG(I2S_INT_ST_REG(0));
	if (slc_intr_status == 0) {
		//No interested interrupts pending
		return;
	}
	//clear all intrs
	WRITE_PERI_REG(I2S_INT_CLR_REG(0), 0xffffffff);
	if (slc_intr_status & I2S_OUT_EOF_INT_ST) {
		//The DMA subsystem is done with this block: Push it on the queue so it can be re-used.
		finishedDesc=(lldesc_t*)READ_PERI_REG(I2S_OUT_EOF_DES_ADDR_REG(0));
		if (xQueueIsQueueFullFromISR(dmaQueue)) {
			//All buffers are empty. This means we have an underflow on our hands.
			underrunCnt++;
			//Pop the top off the queue; it's invalid now anyway.
			xQueueReceiveFromISR(dmaQueue, &dummy, &HPTaskAwoken);
		}
		//Dump the buffer on the queue so the rest of the software can fill it.
		xQueueSendFromISR(dmaQueue, (void*)(&finishedDesc->buf), &HPTaskAwoken);
	}
	//We're done.
	if(HPTaskAwoken == pdTRUE) {
		portYIELD_FROM_ISR();
	}
}


//Initialize I2S subsystem for DMA circular buffer use
void i2sInit() {
	int x, y;
	
	underrunCnt=0;
	
	
	//Take care of the DMA buffers.
	for (y=0; y<I2SDMABUFCNT; y++) {
		//Allocate memory for this DMA sample buffer.
		//Clear sample buffer. We don't want noise.
		i2sBuf[y]=calloc(I2SDMABUFLEN, 4);
		//Clear sample buffer. We don't want noise.
		for (x=0; x<I2SDMABUFLEN; x++) {
			i2sBuf[y][x]=0;
		}
	}

	periph_module_enable(PERIPH_I2S0_MODULE);

	//Reset DMA
	SET_PERI_REG_MASK(I2S_LC_CONF_REG(0), I2S_IN_RST | I2S_OUT_RST | I2S_AHBM_RST | I2S_AHBM_FIFO_RST);
    CLEAR_PERI_REG_MASK(I2S_LC_CONF_REG(0), I2S_IN_RST | I2S_OUT_RST | I2S_AHBM_RST | I2S_AHBM_FIFO_RST);

    //Reset I2S FIFO
    SET_PERI_REG_MASK(I2S_CONF_REG(0), I2S_RX_RESET | I2S_TX_RESET | I2S_TX_FIFO_RESET | I2S_RX_FIFO_RESET);
    CLEAR_PERI_REG_MASK(I2S_CONF_REG(0), I2S_RX_RESET | I2S_TX_RESET | I2S_TX_FIFO_RESET | I2S_RX_FIFO_RESET);

	//Enable and configure DMA
	SET_PERI_REG_MASK(I2S_LC_CONF_REG(0), I2S_CHECK_OWNER | I2S_OUT_EOF_MODE);

	//Configure interrupt
	esp_intr_alloc(ETS_I2S0_INTR_SOURCE, 0, &i2s_isr, NULL, &ih);
	SET_PERI_REG_BITS(I2S_INT_ENA_REG(0), 0x1, 1, I2S_OUT_EOF_INT_ENA_S);
	esp_intr_enable(ih);

	//Initialize DMA buffer descriptors in such a way that they will form a circular
	//buffer.
	for (x=0; x<I2SDMABUFCNT; x++) {
		i2sBufDesc[x].owner=1;
		i2sBufDesc[x].eof=1;
		i2sBufDesc[x].sosf=0;
		i2sBufDesc[x].length=I2SDMABUFLEN*4;
		i2sBufDesc[x].size=I2SDMABUFLEN*4;
		i2sBufDesc[x].buf=&i2sBuf[x][0];
		i2sBufDesc[x].offset=0;
		i2sBufDesc[x].empty=(uint32_t)((x<(I2SDMABUFCNT-1))?(&i2sBufDesc[x+1]):(&i2sBufDesc[0]));
	}
	
	//Feed dma the 1st buffer desc addr
	//To send data to the I2S subsystem, counter-intuitively we use the RXLINK part, not the TXLINK as you might
	//expect. The TXLINK part still needs a valid DMA descriptor, even if it's unused: the DMA engine will throw
	//an error at us otherwise. Just feed it any random descriptor.
	CLEAR_PERI_REG_MASK(I2S_OUT_LINK_REG(0), I2S_OUTLINK_ADDR);
    SET_PERI_REG_MASK(I2S_OUT_LINK_REG(0), ((uint32_t)(&i2sBufDesc[0]))&I2S_OUTLINK_ADDR);
    CLEAR_PERI_REG_MASK(I2S_IN_LINK_REG(0), I2S_INLINK_ADDR);
    SET_PERI_REG_MASK(I2S_IN_LINK_REG(0), ((uint32_t)(&i2sBufDesc[1]))&I2S_INLINK_ADDR);


	//We use a queue to keep track of the DMA buffers that are empty. The ISR will push buffers to the back of the queue,
	//the mp3 decode will pull them from the front and fill them. For ease, the queue will contain *pointers* to the DMA
	//buffers, not the data itself. The queue depth is one smaller than the amount of buffers we have, because there's
	//always a buffer that is being used by the DMA subsystem *right now* and we don't want to be able to write to that
	//simultaneously.
	dmaQueue=xQueueCreate(I2SDMABUFCNT-1, sizeof(int*));

	//Init pins to i2s functions
	//Use GPIO 16/17/18 as I2S port
	gpio_config_t io_conf;
    io_conf.intr_type = GPIO_INTR_DISABLE; //disable interrupt
    io_conf.mode = GPIO_MODE_OUTPUT; //set as output mode
    io_conf.pin_bit_mask = GPIO_SEL_17 | GPIO_SEL_18 | GPIO_SEL_19; //bit mask of the pins that you want to set,e.g.GPIO21/22
    io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE; //disable pull-down mode
    io_conf.pull_up_en = GPIO_PULLUP_DISABLE; //disable pull-up mode
    gpio_config(&io_conf); //configure GPIO with the given settings

    gpio_matrix_out(GPIO_NUM_17, I2S0O_DATA_OUT23_IDX, 0, 0);
    gpio_matrix_out(GPIO_NUM_19, I2S0O_BCK_OUT_IDX, 0, 0);
    gpio_matrix_out(GPIO_NUM_18, I2S0O_WS_OUT_IDX, 0, 0);

	//Reset I2S subsystem
	CLEAR_PERI_REG_MASK(I2S_CONF_REG(0), I2S_RX_RESET | I2S_TX_RESET);
    SET_PERI_REG_MASK(I2S_CONF_REG(0), I2S_RX_RESET | I2S_TX_RESET);
    CLEAR_PERI_REG_MASK(I2S_CONF_REG(0), I2S_RX_RESET | I2S_TX_RESET);

    WRITE_PERI_REG(I2S_CONF_REG(0), 0);//I2S_SIG_LOOPBACK);
    WRITE_PERI_REG(I2S_CONF2_REG(0), 0);

	//Select 16bits per channel (FIFO_MOD=0), no DMA access (FIFO only)
	CLEAR_PERI_REG_MASK(I2S_FIFO_CONF_REG(0), I2S_DSCR_EN | I2S_TX_FIFO_MOD_M | I2S_RX_FIFO_MOD_M);

    WRITE_PERI_REG(I2S_FIFO_CONF_REG(0),
                   (32 << I2S_TX_DATA_NUM_S) |     //Low watermark for IRQ
                   (32 << I2S_RX_DATA_NUM_S));
	
	//Enable DMA in i2s subsystem
	SET_PERI_REG_MASK(I2S_FIFO_CONF_REG(0), I2S_DSCR_EN);

	//tx/rx binaureal
	WRITE_PERI_REG(I2S_CONF_CHAN_REG(0), (0 << I2S_TX_CHAN_MOD_S) | (0 << I2S_RX_CHAN_MOD_S));

	//Clear int
	// SET_PERI_REG_MASK(I2S_INT_CLR_REG(0),   I2S_TX_REMPTY_INT_CLR|I2S_TX_WFULL_INT_CLR|
	// 		I2S_RX_WFULL_INT_CLR|I2S_PUT_DATA_INT_CLR|I2S_TAKE_DATA_INT_CLR);
	// CLEAR_PERI_REG_MASK(I2S_INT_CLR_REG(0), I2S_TX_REMPTY_INT_CLR|I2S_TX_WFULL_INT_CLR|
	// 		I2S_RX_WFULL_INT_CLR|I2S_PUT_DATA_INT_CLR|I2S_TAKE_DATA_INT_CLR);
	
	//trans master&rece slave,MSB shift,right_first,msb right
	// SET_PERI_REG_MASK(I2S_CONF_REG(0), I2S_TX_RIGHT_FIRST | I2S_TX_MSB_RIGHT | I2S_TX_MSB_SHIFT | I2S_RX_SLAVE_MOD | I2S_RX_MSB_SHIFT);
	SET_PERI_REG_MASK(I2S_CONF_REG(0), I2S_TX_MSB_SHIFT);

	i2sSetRate(I2S_DEFAULT_SAMPLE_RATE, 0);
	WRITE_PERI_REG(I2S_TIMING_REG(0), (1 << I2S_TX_WS_OUT_DELAY_S));

	//No idea if ints are needed...
	WRITE_PERI_REG(I2S_INT_CLR_REG(0), 0xFFFFFFFF);
	//Start transmission
	SET_PERI_REG_MASK(I2S_OUT_LINK_REG(0), I2S_OUTLINK_START);
	SET_PERI_REG_MASK(I2S_CONF_REG(0), I2S_TX_START);
}


#define BASEFREQ (160000000L)
#define ABS(x) (((x)>0)?(x):(-(x)))

//Set the I2S sample rate, in HZ
void i2sSetRate(int rate, int enaWordlenFuzzing) {
	//Find closest divider 
	int bestclkmdiv=5, bestbckdiv=2, bestbits=16, bestfreq=-10000;
	int tstfreq;
	int bckdiv, clkmdiv, bits=16;
	/*
		CLK_I2S = 160MHz / I2S_CLKM_DIV_NUM
		BCLK = CLK_I2S / I2S_BCK_DIV_NUM
		WS = BCLK/ 2 / (16 + I2S_BITS_MOD)
		Note that I2S_CLKM_DIV_NUM must be >5 for I2S data
		I2S_CLKM_DIV_NUM - 5-127
		I2S_BCK_DIV_NUM - 2-127
		
		We also have the option to send out more than 2x16 bit per sample. Most I2S codecs will
		ignore the extra bits and in the case of the 'fake' PWM/delta-sigma outputs, they will just lower the output
		voltage a bit, so we add them when it makes sense.
	*/
	for (bckdiv=2; bckdiv<64; bckdiv++) {
		for (clkmdiv=5; clkmdiv<64; clkmdiv++) {
			for (bits=16; bits<(enaWordlenFuzzing?20:17); bits++) {
				tstfreq=BASEFREQ/(bckdiv*clkmdiv*bits*2);
				if (ABS(rate-tstfreq)<ABS(rate-bestfreq)) {
					bestfreq=tstfreq;
					bestclkmdiv=clkmdiv;
					bestbckdiv=bckdiv;
					bestbits=bits;
				}
			}
		}
	}

	printf("ReqRate %d MDiv %d BckDiv %d Bits %d  Frq %d\n", 
		rate, bestclkmdiv, bestbckdiv, bestbits, (int)(BASEFREQ/(bestbckdiv*bestclkmdiv*bestbits*2)));	
	
	SET_PERI_REG_BITS(I2S_SAMPLE_RATE_CONF_REG(0), I2S_RX_BITS_MOD, bestbits, I2S_RX_BITS_MOD_S);
    SET_PERI_REG_BITS(I2S_SAMPLE_RATE_CONF_REG(0), I2S_TX_BITS_MOD, bestbits, I2S_TX_BITS_MOD_S);

    SET_PERI_REG_BITS(I2S_SAMPLE_RATE_CONF_REG(0), I2S_RX_BCK_DIV_NUM, bestbckdiv, I2S_RX_BCK_DIV_NUM_S);
    SET_PERI_REG_BITS(I2S_SAMPLE_RATE_CONF_REG(0), I2S_TX_BCK_DIV_NUM, bestbckdiv, I2S_TX_BCK_DIV_NUM_S);


	SET_PERI_REG_BITS(I2S_CLKM_CONF_REG(0), I2S_CLKM_DIV_A, 0, I2S_CLKM_DIV_A_S);
    SET_PERI_REG_BITS(I2S_CLKM_CONF_REG(0), I2S_CLKM_DIV_B, 0, I2S_CLKM_DIV_B_S);
    SET_PERI_REG_BITS(I2S_CLKM_CONF_REG(0), I2S_CLKM_DIV_NUM, bestclkmdiv, I2S_CLKM_DIV_NUM_S);  //Setting to 0 wrecks it up.
}

//Current DMA buffer we're writing to
static unsigned int *currDMABuff=NULL;
//Current position in that DMA buffer
static int currDMABuffPos=0;


//This routine pushes a single, 32-bit sample to the I2S buffers. Call this at (on average) 
//at least the current sample rate. You can also call it quicker: it will suspend the calling
//thread if the buffer is full and resume when there's room again.
void i2sPushSample(unsigned int sample) {
	//Check if current DMA buffer is full.
	if (currDMABuffPos==I2SDMABUFLEN || currDMABuff==NULL) {
		//We need a new buffer. Pop one from the queue.
		xQueueReceive(dmaQueue, &currDMABuff, portMAX_DELAY);
		currDMABuffPos=0;
	}
	currDMABuff[currDMABuffPos++]=sample;
}

long i2sGetUnderrunCnt() {
	return underrunCnt;
}