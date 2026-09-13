#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/spi/spidev.h>
#include <gpiod.h>

static int do_spi(int fd, const unsigned char *tx, unsigned char *rx, int len) {
    struct spi_ioc_transfer tr;
    memset(&tr, 0, sizeof(tr));
    tr.tx_buf = (unsigned long)tx;
    tr.rx_buf = (unsigned long)rx;
    tr.len = len;
    tr.speed_hz = 1000000;
    tr.bits_per_word = 8;
    return ioctl(fd, SPI_IOC_MESSAGE(1), &tr);
}

int main() {
    printf("1. Opening GPIO chip...\n");
    struct gpiod_chip *chip = gpiod_chip_open("/dev/gpiochip0");
    if (!chip) { perror("gpiod_chip_open"); return 1; }

    struct gpiod_line_settings *settings = gpiod_line_settings_new();
    gpiod_line_settings_set_direction(settings, GPIOD_LINE_DIRECTION_OUTPUT);
    gpiod_line_settings_set_output_value(settings, GPIOD_LINE_VALUE_ACTIVE);

    struct gpiod_line_config *lcfg = gpiod_line_config_new();
    unsigned int reset_offset = 85;
    gpiod_line_config_add_line_settings(lcfg, &reset_offset, 1, settings);

    struct gpiod_request_config *rcfg = gpiod_request_config_new();
    gpiod_request_config_set_consumer(rcfg, "test_hw");

    struct gpiod_line_request *req = gpiod_chip_request_lines(chip, rcfg, lcfg);
    if (!req) { perror("gpiod_chip_request_lines"); return 1; }

    printf("2. Pulsing Hardware Reset on pin 85 (assert low 20ms, release high 100ms)...\n");
    gpiod_line_request_set_value(req, 85, GPIOD_LINE_VALUE_INACTIVE); // 0 (Reset asserted)
    usleep(20000);
    gpiod_line_request_set_value(req, 85, GPIOD_LINE_VALUE_ACTIVE);   // 1 (Normal operating)
    usleep(100000);

    int spifd = open("/dev/spidev0.0", O_RDWR);
    if (spifd < 0) { perror("open spidev"); return 1; }

    for (int m = 0; m < 4; m++) {
        unsigned char mode = m;
        unsigned int speed = 1000000;
        ioctl(spifd, SPI_IOC_WR_MODE, &mode);
        ioctl(spifd, SPI_IOC_WR_MAX_SPEED_HZ, &speed);

        unsigned char tx_cmd[1] = { 0x70 };
        unsigned char rx_cmd[1] = { 0 };
        do_spi(spifd, tx_cmd, rx_cmd, 1);
        usleep(15000);

        unsigned char tx[6] = { 0x10, 0xef, 0x80, 0x00, 0x00, 0x00 };
        unsigned char rx[6] = { 0 };
        do_spi(spifd, tx, rx, 6);
        printf("Mode %d Status Read: %02x %02x %02x %02x %02x %02x\n",
               m, rx[0], rx[1], rx[2], rx[3], rx[4], rx[5]);
    }

    close(spifd);
    gpiod_line_request_release(req);
    gpiod_chip_close(chip);
    return 0;
}
