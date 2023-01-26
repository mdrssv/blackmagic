/*
 * This file is part of the Black Magic Debug project.
 *
 * Copyright (C) 2023 1BitSquared <info@1bitsquared.com>
 * Written by Rachel Mant <git@dragonmux.network>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice, this
 *    list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *
 * 3. Neither the name of the copyright holder nor the names of its
 *    contributors may be used to endorse or promote products derived from
 *    this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <string.h>
#include "general.h"
#include "target.h"
#include "target_internal.h"
#include "cortexm.h"

/*
 * For detailed information on how this code works, see:
 * https://www.nxp.com/docs/en/nxp/data-sheets/IMXRT1060CEC.pdf
 * and (behind their login wall):
 * https://cache.nxp.com/secured/assets/documents/en/reference-manual/IMXRT1060RM.pdf?fileExt=.pdf
 */

#define IMXRT_SRC_BASE       UINT32_C(0x400f8000)
#define IMXRT_SRC_BOOT_MODE1 (IMXRT_SRC_BASE + 0x004U)
#define IMXRT_SRC_BOOT_MODE2 (IMXRT_SRC_BASE + 0x01cU)

#define IMXRT_OCRAM1_BASE UINT32_C(0x20280000)
#define IMXRT_OCRAM1_SIZE 0x00080000U
#define IMXRT_OCRAM2_BASE UINT32_C(0x20200000)
#define IMXRT_OCRAM2_SIZE 0x00080000U

#define IMXRT_FLEXSPI1_BASE UINT32_C(0x402a8000)
/* We only carry definitions for FlexSPI1 Flash controller A1. */
#define IMXRT_FLEXSPI1_MOD_CTRL0          (IMXRT_FLEXSPI1_BASE + 0x000U)
#define IMXRT_FLEXSPI1_INT                (IMXRT_FLEXSPI1_BASE + 0x014U)
#define IMXRT_FLEXSPI1_LUT_KEY            (IMXRT_FLEXSPI1_BASE + 0x018U)
#define IMXRT_FLEXSPI1_LUT_CTRL           (IMXRT_FLEXSPI1_BASE + 0x01cU)
#define IMXRT_FLEXSPI1_CTRL0              (IMXRT_FLEXSPI1_BASE + 0x060U)
#define IMXRT_FLEXSPI1_CTRL1              (IMXRT_FLEXSPI1_BASE + 0x070U)
#define IMXRT_FLEXSPI1_CTRL2              (IMXRT_FLEXSPI1_BASE + 0x080U)
#define IMXRT_FLEXSPI1_PRG_CTRL0          (IMXRT_FLEXSPI1_BASE + 0x0a0U)
#define IMXRT_FLEXSPI1_PRG_CTRL1          (IMXRT_FLEXSPI1_BASE + 0x0a4U)
#define IMXRT_FLEXSPI1_PRG_CMD            (IMXRT_FLEXSPI1_BASE + 0x0b0U)
#define IMXRT_FLEXSPI1_PRG_READ_FIFO_CTRL (IMXRT_FLEXSPI1_BASE + 0x0b8U)
#define IMXRT_FLEXSPI1_STAT1              (IMXRT_FLEXSPI1_BASE + 0x0e4U)
#define IMXRT_FLEXSPI1_PRG_READ_FIFO      (IMXRT_FLEXSPI1_BASE + 0x100U)
#define IMXRT_FLEXSPI1_PRG_WRITE_FIFO     (IMXRT_FLEXSPI1_BASE + 0x180U)
#define IMXRT_FLEXSPI1_LUT_BASE           (IMXRT_FLEXSPI1_BASE + 0x200U)

#define IMXRT_FLEXSPI1_MOD_CTRL0_SUSPEND          0x00000002U
#define IMXRT_FLEXSPI1_INT_PRG_CMD_DONE           0x00000001U
#define IMXRT_FLEXSPI1_INT_READ_FIFO_FULL         0x00000020U
#define IMXRT_FLEXSPI1_LUT_KEY_VALUE              0x5af05af0U
#define IMXRT_FLEXSPI1_LUT_CTRL_LOCK              0x00000001U
#define IMXRT_FLEXSPI1_LUT_CTRL_UNLOCK            0x00000002U
#define IMXRT_FLEXSPI1_CTRL1_CAS_MASK             0x00007800U
#define IMXRT_FLEXSPI1_CTRL1_CAS_SHIFT            11U
#define IMXRT_FLEXSPI1_PRG_LENGTH(x)              ((x)&0x0000ffffU)
#define IMXRT_FLEXSPI1_PRG_LUT_INDEX_0            0U
#define IMXRT_FLEXSPI1_PRG_RUN                    0x00000001U
#define IMXRT_FLEXSPI1_PRG_FIFO_CTRL_CLR          0x00000001U
#define IMXRT_FLEXSPI1_PRG_FIFO_CTRL_WATERMARK(x) ((((((x) + 7U) >> 3U) - 1U) & 0xfU) << 2U)

#define IMXRT_FLEXSPI_LUT_OPCODE(x)   (((x)&0x3fU) << 2U)
#define IMXRT_FLEXSPI_LUT_MODE_SERIAL 0x0U
#define IMXRT_FLEXSPI_LUT_MODE_DUAL   0x1U
#define IMXRT_FLEXSPI_LUT_MODE_QUAD   0x2U
#define IMXRT_FLEXSPI_LUT_MODE_OCT    0x3U

#define IMXRT_FLEXSPI_LUT_OP_STOP         0x00U
#define IMXRT_FLEXSPI_LUT_OP_COMMAND      0x01U
#define IMXRT_FLEXSPI_LUT_OP_CADDR        0x03U
#define IMXRT_FLEXSPI_LUT_OP_RADDR        0x02U
#define IMXRT_FLEXSPI_LUT_OP_DUMMY_CYCLES 0x0cU
#define IMXRT_FLEXSPI_LUT_OP_READ         0x09U
#define IMXRT_FLEXSPI_LUT_OP_WRITE        0x08U

#define IMXRT_SPI_FLASH_OPCODE_MASK      0x000000ffU
#define IMXRT_SPI_FLASH_OPCODE(x)        ((x)&IMXRT_SPI_FLASH_OPCODE_MASK)
#define IMXRT_SPI_FLASH_DUMMY_MASK       0x0000ff00U
#define IMXRT_SPI_FLASH_DUMMY_SHIFT      8U
#define IMXRT_SPI_FLASH_DUMMY_LEN(x)     (((x) << IMXRT_SPI_FLASH_DUMMY_SHIFT) & IMXRT_SPI_FLASH_DUMMY_MASK)
#define IMXRT_SPI_FLASH_OPCODE_MODE_MASK 0x00010000U
#define IMXRT_SPI_FLASH_OPCODE_ONLY      (0U << 16U)
#define IMXRT_SPI_FLASH_OPCODE_3B_ADDR   (1U << 16U)
#define IMXRT_SPI_FLASH_DATA_IN          (0U << 17U)
#define IMXRT_SPI_FLASH_DATA_OUT         (1U << 17U)

#define SPI_FLASH_CMD_READ_JEDEC_ID                                                         \
	(IMXRT_SPI_FLASH_OPCODE_ONLY | IMXRT_SPI_FLASH_DATA_IN | IMXRT_SPI_FLASH_DUMMY_LEN(0) | \
		IMXRT_SPI_FLASH_OPCODE(0x9fU))

typedef enum imxrt_boot_src {
	boot_spi_flash_nor,
	boot_sd_card,
	boot_emmc,
	boot_slc_nand,
	boot_parallel_nor,
	boot_spi_flash_nand,
} imxrt_boot_src_e;

typedef struct imxrt_flexspi_lut_insn {
	uint8_t value;
	uint8_t opcode_mode;
} imxrt_flexspi_lut_insn_s;

typedef struct imxrt_priv {
	imxrt_boot_src_e boot_source;
	imxrt_flexspi_lut_insn_s flexspi_lut_seq[8];
} imxrt_priv_s;

static void imxrt_enter_flash_mode(target_s *target);
static void imxrt_spi_read(target_s *target, uint32_t command, target_addr_t address, void *buffer, size_t length);
static imxrt_boot_src_e imxrt_boot_source(uint32_t boot_cfg);

bool imxrt_probe(target_s *target)
{
	/* If the part number fails to match, instantly return. */
	if (target->part_id != 0x88cU)
		return false;

	/* XXX: Would really like to find some way to have a more positive identification on the part */

	imxrt_priv_s *priv = calloc(1, sizeof(imxrt_priv_s));
	if (!priv) { /* calloc faled: heap exhaustion */
		DEBUG_WARN("calloc: failed in %s\n", __func__);
		return false;
	}
	target->target_storage = priv;
	target->target_options |= CORTEXM_TOPT_INHIBIT_NRST;
	target->driver = "i.MXRT10xx";

#if ENABLE_DEBUG
	const uint8_t boot_mode = (target_mem_read32(target, IMXRT_SRC_BOOT_MODE2) >> 24U) & 3U;
#endif
	DEBUG_TARGET("i.MXRT boot mode is %x\n", boot_mode);
	const uint32_t boot_cfg = target_mem_read32(target, IMXRT_SRC_BOOT_MODE1);
	DEBUG_TARGET("i.MXRT boot config is %08" PRIx32 "\n", boot_cfg);
	priv->boot_source = imxrt_boot_source(boot_cfg);
	switch (priv->boot_source) {
	case boot_spi_flash_nor:
		DEBUG_TARGET("-> booting from SPI Flash (NOR)\n");
		break;
	case boot_sd_card:
		DEBUG_TARGET("-> booting from SD Card\n");
		break;
	case boot_emmc:
		DEBUG_TARGET("-> booting from eMMC via uSDHC\n");
		break;
	case boot_slc_nand:
		DEBUG_TARGET("-> booting from SLC NAND via SEMC\n");
		break;
	case boot_parallel_nor:
		DEBUG_TARGET("-> booting from parallel Flash (NOR) via SEMC\n");
		break;
	case boot_spi_flash_nand:
		DEBUG_TARGET("-> booting from SPI Flash (NAND)\n");
		break;
	}

	/* Build the RAM map for the part */
	target_add_ram(target, IMXRT_OCRAM1_BASE, IMXRT_OCRAM1_SIZE);
	target_add_ram(target, IMXRT_OCRAM2_BASE, IMXRT_OCRAM2_SIZE);

	if (priv->boot_source == boot_spi_flash_nor || priv->boot_source == boot_spi_flash_nand) {
		imxrt_enter_flash_mode(target);
		const uint32_t flash_size = target_mem_read32(target, IMXRT_FLEXSPI1_CTRL0);
		DEBUG_INFO("i.MXRT configured for %" PRIu32 " bytes (%" PRIx32 ") of Flash\n", flash_size, flash_size);
	}

	return true;
}

static imxrt_boot_src_e imxrt_boot_source(const uint32_t boot_cfg)
{
	/*
	 * See table 9-9 in §9.6, pg210 of the reference manual for how all these constants and masks were derived.
	 * The bottom 8 bits of boot_cfg must be the value of register BOOT_CFG1.
	 * The boot source is the upper 4 bits of this register (BOOT_CFG1[7:4])
	 */
	const uint8_t boot_src = boot_cfg & 0xf0U;
	if (boot_src == 0x00U)
		return boot_spi_flash_nor;
	if ((boot_src & 0xc0U) == 0x40U)
		return boot_sd_card;
	if ((boot_src & 0xc0U) == 0x80U)
		return boot_emmc;
	if ((boot_src & 0xe0U) == 0x20U)
		return boot_slc_nand;
	if (boot_src == 0x10U)
		return boot_parallel_nor;
	/* The only upper bits combination not tested by this point is 0b11xx. */
	return boot_spi_flash_nand;
}

static void imxrt_enter_flash_mode(target_s *const target)
{
	/* Start by checking that the controller isn't suspended */
	const uint32_t module_state = target_mem_read32(target, IMXRT_FLEXSPI1_MOD_CTRL0);
	if (module_state & IMXRT_FLEXSPI1_MOD_CTRL0_SUSPEND)
		target_mem_write32(target, IMXRT_FLEXSPI1_MOD_CTRL0, module_state & ~IMXRT_FLEXSPI1_MOD_CTRL0_SUSPEND);
	/* Clear all outstanding interrupts so we can consume their status cleanly */
	target_mem_write32(target, IMXRT_FLEXSPI1_INT, target_mem_read32(target, IMXRT_FLEXSPI1_INT));
	/* Tell the controller we want to use the entire read FIFO */
	target_mem_write32(target, IMXRT_FLEXSPI1_PRG_READ_FIFO_CTRL,
		IMXRT_FLEXSPI1_PRG_FIFO_CTRL_WATERMARK(128) | IMXRT_FLEXSPI1_PRG_FIFO_CTRL_CLR);
	/* Then unlock the sequence LUT so we can use it to to run Flash commands */
	if (target_mem_read32(target, IMXRT_FLEXSPI1_LUT_CTRL) != IMXRT_FLEXSPI1_LUT_CTRL_UNLOCK) {
		target_mem_write32(target, IMXRT_FLEXSPI1_LUT_KEY, IMXRT_FLEXSPI1_LUT_KEY_VALUE);
		target_mem_write32(target, IMXRT_FLEXSPI1_LUT_CTRL, IMXRT_FLEXSPI1_LUT_CTRL_UNLOCK);
	}
}

static void imxrt_spi_configure_sequence(
	target_s *const target, const uint32_t command, const target_addr_t address, const size_t length)
{
	/* Read the current value of the LUT index to use */
	imxrt_priv_s *const priv = (imxrt_priv_s *)target->target_storage;
	target_mem_read(target, priv->flexspi_lut_seq, IMXRT_FLEXSPI1_LUT_BASE, sizeof(priv->flexspi_lut_seq));

	/* Build a new slot 0 sequence to run */
	imxrt_flexspi_lut_insn_s sequence[8] = {};
	/* Start by writing the command opcode to the Flash */
	sequence[0].opcode_mode = IMXRT_FLEXSPI_LUT_OPCODE(IMXRT_FLEXSPI_LUT_OP_COMMAND) | IMXRT_FLEXSPI_LUT_MODE_SERIAL;
	sequence[0].value = command & IMXRT_SPI_FLASH_OPCODE_MASK;
	size_t offset = 1;
	/* Then, if the command has an address, perform the necessary addressing */
	if ((command & IMXRT_SPI_FLASH_OPCODE_MODE_MASK) == IMXRT_SPI_FLASH_OPCODE_3B_ADDR) {
		const uint8_t column_address_bits =
			(target_mem_read32(target, IMXRT_FLEXSPI1_CTRL1) & IMXRT_FLEXSPI1_CTRL1_CAS_MASK) >>
			IMXRT_FLEXSPI1_CTRL1_CAS_SHIFT;
		sequence[offset].opcode_mode =
			IMXRT_FLEXSPI_LUT_OPCODE(IMXRT_FLEXSPI_LUT_OP_RADDR) | IMXRT_FLEXSPI_LUT_MODE_SERIAL;
		sequence[offset++].value = 24U - column_address_bits;
		if (column_address_bits) {
			sequence[offset].opcode_mode =
				IMXRT_FLEXSPI_LUT_OPCODE(IMXRT_FLEXSPI_LUT_OP_CADDR) | IMXRT_FLEXSPI_LUT_MODE_SERIAL;
			sequence[offset++].value = column_address_bits;
		}
	}
	sequence[offset].opcode_mode =
		IMXRT_FLEXSPI_LUT_OPCODE(IMXRT_FLEXSPI_LUT_OP_DUMMY_CYCLES) | IMXRT_FLEXSPI_LUT_MODE_SERIAL;
	sequence[offset++].value = (command & IMXRT_SPI_FLASH_DUMMY_MASK) >> IMXRT_SPI_FLASH_DUMMY_SHIFT;
	/* Now run the data phase based on the operation's data direction */
	if (command & IMXRT_SPI_FLASH_DATA_OUT)
		sequence[offset].opcode_mode =
			IMXRT_FLEXSPI_LUT_OPCODE(IMXRT_FLEXSPI_LUT_OP_WRITE) | IMXRT_FLEXSPI_LUT_MODE_SERIAL;
	else
		sequence[offset].opcode_mode =
			IMXRT_FLEXSPI_LUT_OPCODE(IMXRT_FLEXSPI_LUT_OP_READ) | IMXRT_FLEXSPI_LUT_MODE_SERIAL;
	sequence[offset++].value = 0;
	/* Because sequence gets 0 initalised above when it's declared, the STOP entry is already present */

	/* Write the new sequence to the programmable sequence LUT */
	target_mem_write(target, IMXRT_FLEXSPI1_LUT_BASE, sequence, sizeof(sequence));
	/* Write the address, if any, to the sequence address register */
	if ((command & IMXRT_SPI_FLASH_OPCODE_MODE_MASK) == IMXRT_SPI_FLASH_OPCODE_3B_ADDR)
		target_mem_write32(target, IMXRT_FLEXSPI1_PRG_CTRL0, address);
	/* Write the command data length and sequence index */
	target_mem_write32(
		target, IMXRT_FLEXSPI1_PRG_CTRL1, IMXRT_FLEXSPI1_PRG_LUT_INDEX_0 | IMXRT_FLEXSPI1_PRG_LENGTH(length));
}

static void imxrt_spi_restore(target_s *const target)
{
	imxrt_priv_s *const priv = (imxrt_priv_s *)target->target_storage;
	/* Write the original LUT contents back so as not to perterb the firmware */
	target_mem_write(target, IMXRT_FLEXSPI1_LUT_BASE, priv->flexspi_lut_seq, sizeof(priv->flexspi_lut_seq));
}

static void imxrt_spi_wait_complete(target_s *const target)
{
	/* Set the sequence running */
	target_mem_write32(target, IMXRT_FLEXSPI1_PRG_CMD, IMXRT_FLEXSPI1_PRG_RUN);
	/* Wait till it finishes */
	while (!(target_mem_read32(target, IMXRT_FLEXSPI1_INT) & IMXRT_FLEXSPI1_INT_PRG_CMD_DONE))
		continue;
	/* Then clear the interrupt bit it sets. */
	target_mem_write32(target, IMXRT_FLEXSPI1_INT, IMXRT_FLEXSPI1_INT_PRG_CMD_DONE);
}

static void imxrt_spi_read(target_s *const target, const uint32_t command, const target_addr_t address,
	void *const buffer, const size_t length)
{
	/* Configure the programmable sequence LUT and execute the read */
	imxrt_spi_configure_sequence(target, command, address, length);
	imxrt_spi_wait_complete(target);
	/* Transfer the resulting data into the target buffer */
	uint32_t data[32];
	target_mem_read(target, data, IMXRT_FLEXSPI1_PRG_READ_FIFO, 128);
	memcpy(buffer, data, length);
	target_mem_write32(target, IMXRT_FLEXSPI1_INT, IMXRT_FLEXSPI1_INT_READ_FIFO_FULL);
	/* And restore the sequence LUT when we're done */
	imxrt_spi_restore(target);
}