/**
 * @file spider_can.c
 *
 * @section desc File description
 *
 * Renesas R-Car S4 CAN 2.0 driver.
 *
 * @section copyright Copyright
 *
 * Trampoline OS
 *
 * Trampoline is copyright (c) IRCCyN 2005+
 * Trampoline is protected by the French intellectual property law.
 *
 * (C) BayLibre 2023
 *
 * This software is distributed under the Lesser GNU Public Licence
 *
 *  @section infos File informations
 *
 *  $Date$
 *  $Rev$
 *  $Author$
 *  $URL$
 */
#include <Can.h>
#include <iodefine.h>
#include <string.h>
#include "spider_can.h"

// TODO make this value accessible globally
#define PROTECTION_DISABLE_KEY 0xA5A5A501
// TODO make this value accessible globally
#define PROTECTION_ENABLE_KEY 0xA5A5A500

#define SPIDER_CAN_RECEIVED_DATA_FLAG(ctrl) (((volatile struct __tag5586 *) ctrl->base_address)->CFDRMND0.UINT32 & 0x00000001)

/**
 * The CAN clock that feeds the prescalers (in Hertz).
 */
#define SPIDER_CAN_CLOCK (80000000)
#define SPIDER_CAN_COMPUTE_PRESCALER(baud_rate, prop_seg, tseg1, tseg2) ((SPIDER_CAN_CLOCK / (baud_rate * (1 + prop_seg + tseg1 + tseg2))) - 1)

static int spider_can_init(struct tpl_can_controller_config_t *config);
static int spider_set_baudrate(struct tpl_can_controller_t *ctrl, CanControllerBaudrateConfig *baud_rate_config);
static Std_ReturnType spider_transmit(struct tpl_can_controller_t *ctrl, const Can_PduType *pdu_info);
static Std_ReturnType spider_receive(struct tpl_can_controller_t *ctrl, Can_PduType *pdu_info);
static int spider_is_data_available(struct tpl_can_controller_t *ctrl);

tpl_can_controller_t spider_can_controller_0 =
{
	RSCFD0_BASE_ADDR,
	spider_can_init,
	spider_set_baudrate,
	spider_transmit,
	spider_receive,
	spider_is_data_available
};

/**
 * Refer to the figure 6.2 of the datasheet "158_23_uciaprcn0140kai_IPSpec_v010401.pdf" to find the TSEG1 and TSEG2 for each desired baud rate.
 * Then, compute the number of time quanta per bit : Total_TQ_Per_Bit = TSEG1 + TSEG2 + 1 (see datasheet section 6.1.1 for more information).
 * It is now possible to compute the prescaler value : Prescaler = (DLL / (Baud_Rate * Total_TQ_Per_Bit)) - 1.
 * DLL is the CAN module clock in MHz, Baud_Rate is the desired baud rate in bit/s and Total_TQ_Per_Bit has been computed previously.
 * On the R-Car S4 Spider board, the DLL clock is 80MHz.
 * The computed prescaler does not correspond to the measured baud rate, so adding 1 or 2 to Total_TQ_Per_Bit allows to get a correct value.
 * For this, use the CanControllerPropSeg value from the baud rate configuration.
 */
static int spider_configure_baud_rate_registers(struct tpl_can_controller_t *ctrl, CanControllerBaudrateConfig *baud_rate_config)
{
	struct __tag743 nominal_bitrate_configuration_register;
	volatile struct __tag5586 *ctrl_base_address = (volatile struct __tag5586 *) ctrl->base_address;

	// Make sure the CAN baud rates are in the allowed range
	if (baud_rate_config->CanControllerBaudRate > 1000)
		return -1;
	if (baud_rate_config->use_fd_configuration && baud_rate_config->can_fd_config.CanControllerFdBaudRate > 8000)
		return -1;

	// Set the CAN 2.0 baud rate registers
	nominal_bitrate_configuration_register.NBRP = SPIDER_CAN_COMPUTE_PRESCALER(
		baud_rate_config->CanControllerBaudRate * 1000,
		baud_rate_config->CanControllerPropSeg,
		baud_rate_config->CanControllerSeg1,
		baud_rate_config->CanControllerSeg2);
	nominal_bitrate_configuration_register.NSJW = baud_rate_config->CanControllerSyncJumpWidth;
	nominal_bitrate_configuration_register.NTSEG1 = baud_rate_config->CanControllerSeg1;
	nominal_bitrate_configuration_register.NTSEG2 = baud_rate_config->CanControllerSeg2;
	ctrl_base_address->CFDC0NCFG.BIT = nominal_bitrate_configuration_register;

	return 0;
}

static int spider_can_init(struct tpl_can_controller_config_t *config)
{
	uint32 val;
	struct tpl_can_controller_t *ctrl = config->controller;
	volatile struct __tag5586 *ctrl_base_address = (volatile struct __tag5586 *) ctrl->base_address;

	// Clock the CAN module with a 80MHz clock to be able to reach 8Mbit/s bus speed in CAN-FD mode (see datasheet table 13.6)
	SYSCTRL.CLKKCPROT1.UINT32 = PROTECTION_DISABLE_KEY; // Allow access to the clock controller registers
	SYSCTRL.CLKD_PLLC.UINT32 = 0x00000001; // Make sure the PLL output clock is not divided
	while (!SYSCTRL.CLKD_PLLS.BIT.PLLCLKDSYNC);
	SYSCTRL.CKSC_CPUC.UINT32 = 0; // Select the PLL output as the source for the system clock
	while (SYSCTRL.CKSC_CPUS.BIT.CPUCLKSACT);
	SYSCTRL.CLKKCPROT1.UINT32 = PROTECTION_ENABLE_KEY; // Re-enable the clock controller registers protection

	// Allow access to the standby controller registers
	SYSCTRL.STBCKCPROT.UINT32 = PROTECTION_DISABLE_KEY;
	SYSCTRL.MSRKCPROT.UINT32 = PROTECTION_DISABLE_KEY;

	// Enable the CAN controller clock
	val = SYSCTRL.MSR_RSCFD.UINT32;
	if (ctrl->base_address == RSCFD0_BASE_ADDR)
		val &= ~0x00000001;
	else if (ctrl->base_address == RSCFD1_BASE_ADDR)
		val &= ~0x00000002;
	else
		return -1;
	SYSCTRL.MSR_RSCFD.UINT32 = val;

	// Re-enable the standby controller registers protection
	SYSCTRL.STBCKCPROT.UINT32 = PROTECTION_ENABLE_KEY;
	SYSCTRL.MSRKCPROT.UINT32 = PROTECTION_ENABLE_KEY;

	// Wait for the CAN RAM initialization to terminate
	while (ctrl_base_address->CFDGSTS.BIT.GRAMINIT);

	// Make sure the module is in reset mode
	ctrl_base_address->CFDGCTR.UINT32 = 0x00000001;
	while (!ctrl_base_address->CFDGSTS.BIT.GRSTSTS);

	// Release the channel 0 from sleep mode (TODO add support for a bitmask of enabled channels)
	ctrl_base_address->CFDC0CTR.UINT32 = 0x00000001;
	while (ctrl_base_address->CFDC0STS.BIT.CSLPSTS);

	// Configure bus speed (TODO there is only classic CAN for now)
	if (spider_configure_baud_rate_registers(ctrl, &config->baud_rate_config) != 0)
		return E_NOT_OK;

	// Configure rule table (create 2 rules that match all possible frames in reception and transmission)
	ctrl_base_address->CFDGAFLECTR.UINT32 = 1 << 8; // Enable write access for page 0
	ctrl_base_address->CFDGAFLCFG0.UINT32 = 1 << 16; // Configure one rule for channel 0
	ctrl_base_address->CFDGAFLID1.UINT32 = 0; // Do not set IDs as they won't be taken into account with the mask register
	ctrl_base_address->CFDGAFLM1.UINT32 = 0; // Accept all received CAN frames
	ctrl_base_address->CFDGAFLP01.UINT32 = 0x00008000; // Disable DLC check, use RX message address 0
	ctrl_base_address->CFDGAFLP11.UINT32 = 0; // Do not use FIFO for now
	ctrl_base_address->CFDGAFLECTR.UINT32 = 0; // Disable write access for page 0

	// Configure the reception message buffers
	ctrl_base_address->CFDRMNB.UINT32 = (0x7 << 8) | 1; // Allocate one RX message buffer with a 64-byte payload

	// Switch the module to global operation mode
	ctrl_base_address->CFDGCTR.UINT32 = 0;
	while ((ctrl_base_address->CFDGSTS.UINT32 & 0x0000000F) != 0);

	// Switch the required CAN channels to operation mode, TODO handle multiple channels
	ctrl_base_address->CFDC0CTR.UINT32 = 0;
	while (ctrl_base_address->CFDC0STS.BIT.CRSTSTS);

	// Assign the CAN pins to the CAN module (the PMMR register value needs to be written before writing another register with the inverse of the register value)
	// Enable multiplexing register usage
	PFC1.PMMER7_B0A0 = 0x00000001;
	// Select the peripheral function for the CAN pins
	val = PFC1.GPSR7_B0A0 | 0x00000003;
	PFC1.PMMR7_B0A0 = ~val;
	PFC1.GPSR7_B0A0 = val;
	// Select the CAN function for the CAN GPIOs
	val = PFC1.IP0SR7_B0A0 & ~0x00000003;
	PFC1.PMMR7_B0A0 = ~val;
	PFC1.IP0SR7_B0A0 = val;
	// Set the drive strength of the CAN TX pin to 7/8
	val = PFC1.DRV0CTRL7_B0A0 & ~0x0000000F;
	val |= 0x00000003;
	PFC1.PMMR7_B0A0 = ~val;
	PFC1.DRV0CTRL7_B0A0 = val;

	return E_OK;
}

static int spider_set_baudrate(struct tpl_can_controller_t *ctrl, CanControllerBaudrateConfig *baud_rate_config)
{
	// Does nothing for now
	(void) ctrl;
	(void) baud_rate_config;

	return E_OK;
}

static Std_ReturnType spider_transmit(struct tpl_can_controller_t *ctrl, const Can_PduType *pdu_info)
{
	volatile struct __tag5586 *ctrl_base_address = (volatile struct __tag5586 *) ctrl->base_address;
	int i;
	volatile uint8 *src, *dest;

	if (ctrl == NULL)
		return E_NOT_OK;
	if (pdu_info == NULL)
		return E_NOT_OK;

	// Wait for the previous frame to finish transmission
	while (ctrl_base_address->CFDTMSTS0.BIT.TMTSTS);
	ctrl_base_address->CFDTMSTS0.UINT8 = 0; // Clear TMTRF bits to allow the CFDTMCi.TMTR bit to be set again

	// Set the CAN ID
	ctrl_base_address->CFD0TMID0.UINT32 = pdu_info->id & 0x000007FF;

	// Set the payload size
	ctrl_base_address->CFD0TMPTR0.UINT32 = pdu_info->length << 28;

	// Set the frame payload
	if (pdu_info->length > 8)
		return E_NOT_OK;
	src = pdu_info->sdu;
	dest = ctrl_base_address->CFD0TMDF0_0.UINT8;
	// Use a for loop instead of memcpy() to make sure the buffer registers are accessed one byte at a time
	// Using memcpy() triggers a data abort exception for a 7-byte CAN payload
	for (i = 0; i < pdu_info->length; i++)
	{
		*dest = *src;
		src++;
		dest++;
	}

	// Configure the frame format
	ctrl_base_address->CFD0TMFDCTR0.UINT32 = 0;

	// Start the frame transmission
	ctrl_base_address->CFDTMC0.UINT8 = 0x01;

	return E_OK;
}

static Std_ReturnType spider_receive(struct tpl_can_controller_t *ctrl, Can_PduType *pdu_info)
{
	volatile struct __tag5586 *ctrl_base_address = (volatile struct __tag5586 *) ctrl->base_address;
	int i;
	volatile uint8 *src, *dest;

	// Do not block if no data are available
	if (!SPIDER_CAN_RECEIVED_DATA_FLAG(ctrl))
		return E_NOT_OK;

	// Retrieve the CAN ID
	pdu_info->id = ctrl_base_address->CFDRMID0.UINT32 & 0x000007FF;

	// Retrieve the frame length
	pdu_info->length = ctrl_base_address->CFDRMPTR0.UINT32 >> 28;
	if (pdu_info->length > 8)
		goto Exit;

	// Retrieve the frame payload
	src = ctrl_base_address->CFDRMDF0_0.UINT8;
	dest = pdu_info->sdu;
	// Use a for loop instead of memcpy() to make sure the buffer registers are accessed one byte at a time
	// Using memcpy() triggers a data abort exception for a 7-byte CAN payload
	for (i = 0; i < pdu_info->length; i++)
	{
		*dest = *src;
		src++;
		dest++;
	}

Exit:
	// Clear the reception flag
	ctrl_base_address->CFDRMND0.UINT32 &= ~0x00000001;

	return E_OK;
}

static int spider_is_data_available(struct tpl_can_controller_t *ctrl)
{
	return SPIDER_CAN_RECEIVED_DATA_FLAG(ctrl);
}
