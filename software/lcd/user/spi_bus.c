#include "board.h"
#include "lcd.h"
#include "spi_bus.h"

#define LCD_CS_PIN      WM_IO_PB_11
#define FLASH_CS_PIN    WM_IO_PB_15
#define SD_CS_PIN       WM_IO_PB_16

#define BUS_QUEUE_LEN   8
#define BUS_TASK_PRIO   31
#define BUS_TASK_STK    1024

struct spi_bus_job {
    spi_dev_t dev;
    int (*fn)(void *arg);
    void *arg;
    tls_os_sem_t *done;
    int result;
};

static tls_os_queue_t *g_bus_q;
static u32 g_bus_stk[BUS_TASK_STK];
static volatile spi_dev_t g_owner;
static volatile spi_dev_t g_cs_low_dev;
static volatile u32 g_bb_loops = 40u;

static u32 bb_loops_for_hz(u32 hz)
{
    /* Conservative GPIO SPI half-periods for an 80 MHz core. */
    if (hz <= 256000u)
        return 128u;
    if (hz <= 512000u)
        return 80u;
    if (hz <= 1000000u)
        return 40u;
    if (hz <= 4000000u)
        return 10u;
    if (hz <= 10000000u)
        return 4u;
    return 2u;
}

int spi_bus_set_clock(spi_dev_t dev, u32 hz)
{
    if (dev == SPI_DEV_NONE || dev != g_owner || hz == 0)
        return -1;

    if (dev == SPI_DEV_LCD) {
        int ret = tls_spi_setup(TLS_SPI_MODE_0, TLS_SPI_CS_LOW, hz);

        if (ret != 0)
            ret = tls_spi_setup(TLS_SPI_MODE_0, TLS_SPI_CS_LOW, 10000000u);
        return ret;
    }

    if (dev == SPI_DEV_FLASH || dev == SPI_DEV_SD) {
        g_bb_loops = bb_loops_for_hz(hz);
        return 0;
    }
    return -1;
}

void spi_bus_bb_delay(void)
{
    volatile u32 i = g_bb_loops;

    while (i--)
        ;
}

static void cs_pin_high(spi_dev_t dev)
{
    u16 pin;

    switch (dev) {
    case SPI_DEV_LCD:   pin = LCD_CS_PIN; break;
    case SPI_DEV_FLASH: pin = FLASH_CS_PIN; break;
    case SPI_DEV_SD:    pin = SD_CS_PIN; break;
    default: return;
    }
    tls_gpio_cfg(pin, WM_GPIO_DIR_OUTPUT,
                 pin == LCD_CS_PIN ? WM_GPIO_ATTR_FLOATING
                                   : WM_GPIO_ATTR_PULLHIGH);
    tls_gpio_write(pin, 1);
}

static void bus_deselect_all(void)
{
    tls_gpio_cfg(LCD_CS_PIN, WM_GPIO_DIR_OUTPUT, WM_GPIO_ATTR_FLOATING);
    tls_gpio_write(LCD_CS_PIN, 1);
    tls_gpio_cfg(FLASH_CS_PIN, WM_GPIO_DIR_OUTPUT, WM_GPIO_ATTR_PULLHIGH);
    tls_gpio_write(FLASH_CS_PIN, 1);
    tls_gpio_cfg(SD_CS_PIN, WM_GPIO_DIR_OUTPUT, WM_GPIO_ATTR_PULLHIGH);
    tls_gpio_write(SD_CS_PIN, 1);
    g_cs_low_dev = SPI_DEV_NONE;
}

int spi_bus_cs_low(spi_dev_t dev)
{
    if (dev == SPI_DEV_NONE || dev != g_owner) {
        printf("SPIBUS: CS low %d without ownership (owner=%d)\r\n",
               (int)dev, (int)g_owner);
        return -1;
    }
    if (g_cs_low_dev != SPI_DEV_NONE) {
        printf("SPIBUS: CS interlock: dev %d already selected\r\n",
               (int)g_cs_low_dev);
        return -1;
    }

    switch (dev) {
    case SPI_DEV_LCD:
        tls_gpio_cfg(LCD_CS_PIN, WM_GPIO_DIR_OUTPUT, WM_GPIO_ATTR_FLOATING);
        tls_gpio_write(LCD_CS_PIN, 0);
        break;
    case SPI_DEV_FLASH:
        /* PB15 is also the W600 hspi_cs pin; force it back to GPIO before
         * every select in case a previous host-SPI transfer left it in AF. */
        tls_gpio_cfg(FLASH_CS_PIN, WM_GPIO_DIR_OUTPUT, WM_GPIO_ATTR_PULLHIGH);
        tls_gpio_write(FLASH_CS_PIN, 0);
        break;
    case SPI_DEV_SD:
        tls_gpio_cfg(SD_CS_PIN, WM_GPIO_DIR_OUTPUT, WM_GPIO_ATTR_PULLHIGH);
        tls_gpio_write(SD_CS_PIN, 0);
        break;
    default: return -1;
    }
    g_cs_low_dev = dev;
    return 0;
}

int spi_bus_cs_high(spi_dev_t dev)
{
    if (dev == SPI_DEV_NONE)
        return -1;
    if (dev != g_owner) {
        printf("SPIBUS: CS high %d without ownership (owner=%d)\r\n",
               (int)dev, (int)g_owner);
        return -1;
    }
    if (g_cs_low_dev != SPI_DEV_NONE && g_cs_low_dev != dev) {
        printf("SPIBUS: CS high %d while %d selected\r\n",
               (int)dev, (int)g_cs_low_dev);
        return -1;
    }
    cs_pin_high(dev);
    if (g_cs_low_dev == dev)
        g_cs_low_dev = SPI_DEV_NONE;
    return 0;
}

static void spi_bus_task(void *arg)
{
    (void)arg;

    for (;;) {
        void *msg = NULL;
        struct spi_bus_job *job;

        if (tls_os_queue_receive(g_bus_q, &msg, sizeof(msg), 0) != TLS_OS_SUCCESS)
            continue;
        job = (struct spi_bus_job *)msg;
        if (!job)
            continue;

        g_owner = job->dev;
        job->result = job->fn(job->arg);

        /* Never let a driver leave its CS low or the bus in bit-bang mode. */
        bus_deselect_all();
        g_owner = SPI_DEV_NONE;
        (void)lcd_spi_resume();

        tls_os_sem_release(job->done);
    }
}

void spi_bus_init(void)
{
    bus_deselect_all();

    if (tls_os_queue_create(&g_bus_q, BUS_QUEUE_LEN) != TLS_OS_SUCCESS) {
        printf("SPIBUS: queue create failed\r\n");
        g_bus_q = NULL;
        return;
    }
    if (tls_os_task_create(NULL,
                           "spibus",
                           spi_bus_task,
                           NULL,
                           (void *)g_bus_stk,
                           BUS_TASK_STK * sizeof(u32),
                           BUS_TASK_PRIO,
                           0) != TLS_OS_SUCCESS) {
        printf("SPIBUS: task create failed\r\n");
        g_bus_q = NULL;
    }
}

int spi_bus_run(spi_dev_t dev, int (*fn)(void *arg), void *arg)
{
    struct spi_bus_job job;
    tls_os_sem_t *done;

    if (!fn)
        return -1;

    if (!g_bus_q)
        return -1;
    if (tls_os_sem_create(&done, 0) != TLS_OS_SUCCESS)
        return -1;

    job.dev = dev;
    job.fn = fn;
    job.arg = arg;
    job.done = done;
    job.result = -1;

    if (tls_os_queue_send(g_bus_q, &job, sizeof(job)) != TLS_OS_SUCCESS) {
        tls_os_sem_delete(done);
        printf("SPIBUS: queue full\r\n");
        return -1;
    }

    (void)tls_os_sem_acquire(done, 0);
    tls_os_sem_delete(done);
    return job.result;
}
