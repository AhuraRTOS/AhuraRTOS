/**
 * @file soc_sleep.h
 * @brief RP235x Arm checks before the shared system clock is slowed for DEEP idle.
 *
 * Private to soc_cb.c. Run after both cores have masked interrupts and the secondary
 * core has acknowledged its idle park. These checks never wait, acknowledge an IRQ,
 * or change a peripheral's configuration. An ineligible window keeps clocks running.
 *
 * @copyright (c) 2026 Ahura Project Contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef SOC_RP235X_ARM_SLEEP_H
#define SOC_RP235X_ARM_SLEEP_H

#include <stdbool.h>
#include <stdint.h>

#include "hardware/structs/clocks.h"
#include "hardware/structs/dma.h"
#include "hardware/structs/hstx_ctrl.h"
#include "hardware/structs/i2c.h"
#include "hardware/structs/pio.h"
#include "hardware/structs/pll.h"
#include "hardware/structs/pwm.h"
#include "hardware/structs/spi.h"
#include "hardware/structs/uart.h"
#include "hardware/structs/usb.h"

/* UART BUSY describes transmission, not an asynchronous receiver. Keep an armed
 * receive wake source at its configured baud rate by declining DEEP. Polling-only
 * input cannot be inferred here; the board veto must cover that use case. */
static inline bool soc_deep_uart_ready(const uart_hw_t *uart)
{
    uint32_t control = uart->cr;
    bool ready = true;

    if ((control & UART_UARTCR_UARTEN_BITS) != 0U)
    {
        uint32_t flags = uart->fr;
        uint32_t receive_irqs = UART_UARTIMSC_RXIM_BITS | UART_UARTIMSC_RTIM_BITS |
                                UART_UARTIMSC_FEIM_BITS | UART_UARTIMSC_PEIM_BITS |
                                UART_UARTIMSC_BEIM_BITS | UART_UARTIMSC_OEIM_BITS;

        ready = ((flags & UART_UARTFR_BUSY_BITS) == 0U) &&
                ((flags & UART_UARTFR_TXFE_BITS) != 0U) &&
                ((flags & UART_UARTFR_RXFE_BITS) != 0U) &&
                (((control & UART_UARTCR_RXE_BITS) == 0U) ||
                 ((uart->imsc & receive_irqs) == 0U));
    }

    return ready;
}

static inline bool soc_deep_spi_ready(const spi_hw_t *spi)
{
    uint32_t control = spi->cr1;
    bool ready = true;

    if ((control & SPI_SSPCR1_SSE_BITS) != 0U)
    {
        uint32_t status = spi->sr;

        /* An external master can start a slave transaction after this read. */
        ready = ((control & SPI_SSPCR1_MS_BITS) == 0U) &&
                ((status & (SPI_SSPSR_BSY_BITS | SPI_SSPSR_RNE_BITS)) == 0U) &&
                ((status & SPI_SSPSR_TFE_BITS) != 0U);
    }

    return ready;
}

static inline bool soc_deep_i2c_ready(const i2c_hw_t *i2c)
{
    bool ready = true;

    if ((i2c->enable & I2C_IC_ENABLE_ENABLE_BITS) != 0U)
    {
        ready = ((i2c->status & I2C_IC_STATUS_ACTIVITY_BITS) == 0U) &&
                ((i2c->con & I2C_IC_CON_IC_SLAVE_DISABLE_BITS) != 0U) &&
                (i2c->txflr == 0U) && (i2c->rxflr == 0U);
    }

    return ready;
}

static inline bool soc_deep_peripherals_ready(void)
{
    uint32_t sys_ctrl = clocks_hw->clk[clk_sys].ctrl;
    uint32_t ref_ctrl = clocks_hw->clk[clk_ref].ctrl;
    uint32_t peri_ctrl = clocks_hw->clk[clk_peri].ctrl;
    uint32_t hstx_ctrl = clocks_hw->clk[clk_hstx].ctrl;
    uint32_t peri_source = (peri_ctrl & CLOCKS_CLK_PERI_CTRL_AUXSRC_BITS) >>
                           CLOCKS_CLK_PERI_CTRL_AUXSRC_LSB;
    uint32_t hstx_source = (hstx_ctrl & CLOCKS_CLK_HSTX_CTRL_AUXSRC_BITS) >>
                           CLOCKS_CLK_HSTX_CTRL_AUXSRC_LSB;
    bool peri_changes = (peri_source == CLOCKS_CLK_PERI_CTRL_AUXSRC_VALUE_CLK_SYS) ||
                        (peri_source == CLOCKS_CLK_PERI_CTRL_AUXSRC_VALUE_CLKSRC_PLL_SYS);
    bool hstx_changes = (hstx_source == CLOCKS_CLK_HSTX_CTRL_AUXSRC_VALUE_CLK_SYS) ||
                        (hstx_source == CLOCKS_CLK_HSTX_CTRL_AUXSRC_VALUE_CLKSRC_PLL_SYS);
    bool ready;

    /* Only the normal, locked PLL_SYS/XOSC tree is restored by this path. A board
     * using a bypassed PLL, external source, or a source transition keeps LIGHT. */
    ready = ((sys_ctrl & (CLOCKS_CLK_SYS_CTRL_SRC_BITS | CLOCKS_CLK_SYS_CTRL_AUXSRC_BITS)) ==
             CLOCKS_CLK_SYS_CTRL_SRC_VALUE_CLKSRC_CLK_SYS_AUX) &&
            (clocks_hw->clk[clk_sys].selected ==
             (1U << CLOCKS_CLK_SYS_CTRL_SRC_VALUE_CLKSRC_CLK_SYS_AUX)) &&
            ((ref_ctrl & CLOCKS_CLK_REF_CTRL_SRC_BITS) == CLOCKS_CLK_REF_CTRL_SRC_VALUE_XOSC_CLKSRC) &&
            (clocks_hw->clk[clk_ref].selected == (1U << CLOCKS_CLK_REF_CTRL_SRC_VALUE_XOSC_CLKSRC)) &&
            ((pll_sys_hw->cs & (PLL_CS_LOCK_BITS | PLL_CS_BYPASS_BITS)) == PLL_CS_LOCK_BITS) &&
            ((pll_sys_hw->pwr & (PLL_PWR_PD_BITS | PLL_PWR_VCOPD_BITS | PLL_PWR_POSTDIVPD_BITS)) == 0U);

    /* All channels are checked, including channels owned by applications/SDKs.
     * Chaining cannot start another channel once no DMA channel is in flight. */
    for (uint32_t channel = 0U; ready && (channel < NUM_DMA_CHANNELS); channel++)
    {
        ready = ((dma_hw->ch[channel].al1_ctrl & DMA_CH0_CTRL_TRIG_BUSY_BITS) == 0U);
    }

    if (ready)
    {
        /* RP2350-E12 requires clk_sys to stay above the active USB clock.
         * Keeping clk_usb running alone is insufficient when clk_sys falls to XOSC. */
        ready = ((usb_hw->main_ctrl & USB_MAIN_CTRL_CONTROLLER_EN_BITS) == 0U) &&
                (((pio0_hw->ctrl | pio1_hw->ctrl | pio2_hw->ctrl) & PIO_CTRL_SM_ENABLE_BITS) == 0U) &&
                ((pwm_hw->en & PWM_EN_BITS) == 0U) &&
                (!hstx_changes || ((hstx_ctrl_hw->csr & HSTX_CTRL_CSR_EN_BITS) == 0U)) &&
                soc_deep_i2c_ready(i2c0_hw) && soc_deep_i2c_ready(i2c1_hw);
    }

    if (ready && peri_changes)
    {
        ready = soc_deep_uart_ready(uart0_hw) && soc_deep_uart_ready(uart1_hw) &&
                soc_deep_spi_ready(spi0_hw) && soc_deep_spi_ready(spi1_hw);
    }

    /* Other enabled slices must not lose their source when PLL_SYS is stopped.
     * clk_peri/clk_hstx following clk_sys are handled by the activity checks above. */
    if (ready)
    {
        uint32_t usb_ctrl = clocks_hw->clk[clk_usb].ctrl;
        uint32_t adc_ctrl = clocks_hw->clk[clk_adc].ctrl;

        ready = !(((peri_ctrl & CLOCKS_CLK_PERI_CTRL_ENABLE_BITS) != 0U) &&
                  (peri_source == CLOCKS_CLK_PERI_CTRL_AUXSRC_VALUE_CLKSRC_PLL_SYS)) &&
                !(((hstx_ctrl & CLOCKS_CLK_HSTX_CTRL_ENABLE_BITS) != 0U) &&
                  (hstx_source == CLOCKS_CLK_HSTX_CTRL_AUXSRC_VALUE_CLKSRC_PLL_SYS)) &&
                !(((usb_ctrl & CLOCKS_CLK_USB_CTRL_ENABLE_BITS) != 0U) &&
                  (((usb_ctrl & CLOCKS_CLK_USB_CTRL_AUXSRC_BITS) >> CLOCKS_CLK_USB_CTRL_AUXSRC_LSB) ==
                   CLOCKS_CLK_USB_CTRL_AUXSRC_VALUE_CLKSRC_PLL_SYS)) &&
                !(((adc_ctrl & CLOCKS_CLK_ADC_CTRL_ENABLE_BITS) != 0U) &&
                  (((adc_ctrl & CLOCKS_CLK_ADC_CTRL_AUXSRC_BITS) >> CLOCKS_CLK_ADC_CTRL_AUXSRC_LSB) ==
                   CLOCKS_CLK_ADC_CTRL_AUXSRC_VALUE_CLKSRC_PLL_SYS));
    }

    for (uint32_t output = (uint32_t)clk_gpout0; ready && (output <= (uint32_t)clk_gpout3); output++)
    {
        uint32_t control = clocks_hw->clk[output].ctrl;

        if ((control & CLOCKS_CLK_GPOUT0_CTRL_ENABLE_BITS) != 0U)
        {
            /* All four GPOUT slices use the same source encoding. */
            uint32_t source = (control & CLOCKS_CLK_GPOUT0_CTRL_AUXSRC_BITS) >>
                              CLOCKS_CLK_GPOUT0_CTRL_AUXSRC_LSB;

            ready = (source != CLOCKS_CLK_GPOUT0_CTRL_AUXSRC_VALUE_CLKSRC_PLL_SYS) &&
                    (source != CLOCKS_CLK_GPOUT0_CTRL_AUXSRC_VALUE_CLK_SYS) &&
                    (!peri_changes || (source != CLOCKS_CLK_GPOUT0_CTRL_AUXSRC_VALUE_CLK_PERI)) &&
                    (!hstx_changes || (source != CLOCKS_CLK_GPOUT0_CTRL_AUXSRC_VALUE_CLK_HSTX));
        }
    }

    return ready;
}

#endif /* SOC_RP235X_ARM_SLEEP_H */
