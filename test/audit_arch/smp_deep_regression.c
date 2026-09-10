/**
 * @file smp_deep_regression.c
 * @brief Executes the production RP235x Arm two-core sleep handshake.
 *
 * Drives the real soc_cb.c against controlled register and peer interleavings, including the
 * abort and rendezvous-timeout paths a board run cannot schedule on demand.
 *
 * @copyright (c) 2026 Ahura Project Contributors
 *            SPDX-License-Identifier: GPL-3.0-or-later
 *            See LICENSE in the project root for the full license text.
 */
/*
 * ***********************************************************************************************************
 * Includes
 * ***********************************************************************************************************
*/

#include "../../soc/raspberrypi/rp235x_arm/soc_cb.c"

volatile uint32_t audit_idle = 1U;
volatile uint32_t audit_time_us;
volatile uint32_t audit_board_allowed = 1U;

/*
 * ***********************************************************************************************************
 * Function implementations
 * ***********************************************************************************************************
*/

bool os_task_current_is_idle(void)
{
    return audit_idle != 0U;
}
uint64_t time_us_64(void)
{
    return audit_time_us++;
}

uint32_t audit_prepare(void)
{
    return os_arch_soc_sleep_prepare_cb() ? 1U : 0U;
}
void audit_finish(void)
{
    os_arch_soc_sleep_finish_cb();
}
void audit_peer(void)
{
    soc_sleep_peer_park();
}
void audit_sleep(void)
{
    os_arch_soc_sleep_cb();
}
uint32_t audit_eligible(void)
{
    return soc_deep_peripherals_ready() ? 1U : 0U;
}

/* Values come from the installed SDK headers used by the firmware build. */
const uint32_t audit_registers[] = {
    (uint32_t)&clocks_hw->clk[clk_sys].ctrl,
    (uint32_t)&clocks_hw->clk[clk_sys].div,
    (uint32_t)&clocks_hw->clk[clk_sys].selected,
    (uint32_t)&clocks_hw->clk[clk_ref].ctrl,
    (uint32_t)&clocks_hw->clk[clk_ref].selected,
    (uint32_t)&clocks_hw->clk[clk_peri].ctrl,
    (uint32_t)&pll_sys_hw->cs,
    (uint32_t)&pll_sys_hw->pwr,
    (uint32_t)&pll_sys_hw->fbdiv_int,
    (uint32_t)&pll_sys_hw->prim,
    (uint32_t)&scb_hw->icsr,
    (uint32_t)&scb_hw->scr,
    (uint32_t)&nvic_hw->iser[0],
    (uint32_t)&nvic_hw->ispr[0],
    (uint32_t)&OS_ARCH_REG_SYST_CSR,
    (uint32_t)&sio_hw->cpuid,
    (uint32_t)&sio_hw->doorbell_out_set,
    (uint32_t)&dma_hw->ch[0].al1_ctrl,
    (uint32_t)&uart0_hw->cr,
    (uint32_t)&uart0_hw->fr,
    (uint32_t)&uart0_hw->imsc,
    (uint32_t)&pio0_hw->ctrl,
    (uint32_t)&pwm_hw->en,
    (uint32_t)&spi0_hw->cr1,
    (uint32_t)&spi0_hw->sr,
    (uint32_t)&i2c0_hw->enable,
    (uint32_t)&i2c0_hw->con,
    (uint32_t)&hstx_ctrl_hw->csr,
    (uint32_t)&clocks_hw->clk[clk_usb].ctrl,
    (uint32_t)&clocks_hw->clk[clk_gpout0].ctrl,
    (uint32_t)&usb_hw->main_ctrl
};
const uint32_t audit_constants[] = {
    CLOCKS_CLK_SYS_CTRL_SRC_VALUE_CLKSRC_CLK_SYS_AUX,
    CLOCKS_CLK_REF_CTRL_SRC_VALUE_XOSC_CLKSRC,
    PLL_CS_LOCK_BITS, PLL_PWR_DSMPD_BITS, PLL_PWR_PD_BITS,
    M33_SCR_SEVONPEND_BITS, M33_SCR_SLEEPDEEP_BITS,
    M33_ICSR_PENDSTSET_BITS, M33_ICSR_PENDSVSET_BITS,
    DMA_CH0_CTRL_TRIG_BUSY_BITS,
    UART_UARTCR_UARTEN_BITS | UART_UARTCR_TXE_BITS | UART_UARTCR_RXE_BITS,
    UART_UARTFR_TXFE_BITS | UART_UARTFR_RXFE_BITS,
    UART_UARTFR_BUSY_BITS, UART_UARTIMSC_RXIM_BITS,
    SPI_SSPCR1_SSE_BITS | SPI_SSPCR1_MS_BITS,
    SPI_SSPSR_TFE_BITS,
    I2C_IC_ENABLE_ENABLE_BITS,
    HSTX_CTRL_CSR_EN_BITS,
    CLOCKS_CLK_USB_CTRL_ENABLE_BITS |
        (CLOCKS_CLK_USB_CTRL_AUXSRC_VALUE_CLKSRC_PLL_SYS << CLOCKS_CLK_USB_CTRL_AUXSRC_LSB),
    CLOCKS_CLK_GPOUT0_CTRL_ENABLE_BITS |
        (CLOCKS_CLK_GPOUT0_CTRL_AUXSRC_VALUE_CLK_SYS << CLOCKS_CLK_GPOUT0_CTRL_AUXSRC_LSB),
    USB_MAIN_CTRL_CONTROLLER_EN_BITS
};
