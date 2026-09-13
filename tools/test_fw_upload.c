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
    int ret = ioctl(fd, SPI_IOC_MESSAGE(1), &tr);
    if (ret < 0) {
        perror("SPI_IOC_MESSAGE");
    }
    return ret;
}

int main() {
    printf("=== FT9361 Firmware Upload Test ===\n");

    // 1. Read firmware file
    FILE *f = fopen("/tmp/ft9361_fw.bin", "rb");
    if (!f) { perror("fopen /tmp/ft9361_fw.bin"); return 1; }
    fseek(f, 0, SEEK_END);
    long fw_len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (fw_len != 10396) {
        printf("Unexpected fw_len: %ld (expected 10396)\n", fw_len);
        fclose(f);
        return 1;
    }
    unsigned char fw_buf[10396];
    if (fread(fw_buf, 1, fw_len, f) != fw_len) {
        perror("fread");
        fclose(f);
        return 1;
    }
    fclose(f);
    printf("Loaded firmware binary: %ld bytes\n", fw_len);

    // 2. Open GPIO and pulse reset, KEEPING LINE 85 DRIVEN HIGH!
    printf("Pulsing hardware reset on line 85...\n");
    struct gpiod_chip *chip = gpiod_chip_open("/dev/gpiochip0");
    if (!chip) { perror("gpiod_chip_open"); return 1; }

    struct gpiod_line_settings *settings = gpiod_line_settings_new();
    gpiod_line_settings_set_direction(settings, GPIOD_LINE_DIRECTION_OUTPUT);
    gpiod_line_settings_set_output_value(settings, GPIOD_LINE_VALUE_ACTIVE);

    struct gpiod_line_config *lcfg = gpiod_line_config_new();
    unsigned int reset_offset = 85;
    gpiod_line_config_add_line_settings(lcfg, &reset_offset, 1, settings);

    struct gpiod_request_config *rcfg = gpiod_request_config_new();
    gpiod_request_config_set_consumer(rcfg, "test_fw_upload");

    struct gpiod_line_request *req = gpiod_chip_request_lines(chip, rcfg, lcfg);
    if (!req) { perror("gpiod_chip_request_lines"); return 1; }

    // Assert reset (low) for 5ms as in Windows driver
    gpiod_line_request_set_value(req, 85, GPIOD_LINE_VALUE_INACTIVE);
    usleep(5000); // 5ms
    // Release reset (high)
    gpiod_line_request_set_value(req, 85, GPIOD_LINE_VALUE_ACTIVE);
    usleep(1000); // 1ms

    // 3. Open SPI
    int spifd = open("/dev/spidev0.0", O_RDWR);
    if (spifd < 0) { perror("open /dev/spidev0.0"); return 1; }

    unsigned char mode = SPI_MODE_0;
    unsigned int speed = 1000000;
    ioctl(spifd, SPI_IOC_WR_MODE, &mode);
    ioctl(spifd, SPI_IOC_WR_MAX_SPEED_HZ, &speed);

    // 4. Send 0x55, 0xaa (Opcode 0x57)
    printf("Sending bootloader sync 0x55 0xaa...\n");
    unsigned char sync_cmd[2] = { 0x55, 0xaa };
    unsigned char sync_rx[2] = { 0 };
    do_spi(spifd, sync_cmd, sync_rx, 2);
    printf("Sync rx: %02x %02x\n", sync_rx[0], sync_rx[1]);
    usleep(1000);

    // 5. Send unlock registers
    printf("Sending unlock sequence...\n");
    unsigned char cmd1[4] = { 0x09, 0xf6, 0xc8, 0xff };
    unsigned char rx1[4] = { 0 };
    do_spi(spifd, cmd1, rx1, 4);
    printf("Cmd 1 rx: %02x %02x %02x %02x\n", rx1[0], rx1[1], rx1[2], rx1[3]);
    usleep(1000);

    unsigned char cmd2[4] = { 0x09, 0xf6, 0xca, 0xff };
    do_spi(spifd, cmd2, NULL, 4);
    usleep(1000);

    unsigned char cmd3[4] = { 0x09, 0xf6, 0xcb, 0xff };
    do_spi(spifd, cmd3, NULL, 4);
    usleep(1000);

    unsigned char cmd4[4] = { 0x09, 0xf6, 0xb9, 0xbf };
    do_spi(spifd, cmd4, NULL, 4);
    usleep(1000);

    unsigned char cmd5[4] = { 0x09, 0xf6, 0xb9, 0xff };
    do_spi(spifd, cmd5, NULL, 4);
    usleep(20000); // Sleep 20ms as in Windows driver

    // 6. Send firmware packet
    printf("Uploading firmware packet (10403 bytes)...\n");
    unsigned char pkt[10403];
    pkt[0] = 0x05;
    pkt[1] = 0xfa;
    pkt[2] = 0x00; // addr high
    pkt[3] = 0x00; // addr low
    pkt[4] = (fw_len >> 8) & 0xff; // 0x28
    pkt[5] = fw_len & 0xff;        // 0x9c
    memcpy(&pkt[6], fw_buf, fw_len);
    pkt[10402] = 0x00; // tail dummy

    do_spi(spifd, pkt, NULL, sizeof(pkt));
    printf("Firmware packet sent! Waiting 50ms for MCU boot...\n");
    usleep(50000);

    // 7. Check MCU status
    for (int i = 0; i < 15; i++) {
        unsigned char tx_status[6] = { 0x10, 0xef, 0x20, 0x00, 0x00, 0x00 };
        unsigned char rx_status[6] = { 0 };
        do_spi(spifd, tx_status, rx_status, 6);
        printf("Poll %d: MCU Status = %02x %02x (full rx: %02x %02x %02x %02x %02x %02x)\n",
               i, rx_status[4], rx_status[5],
               rx_status[0], rx_status[1], rx_status[2], rx_status[3], rx_status[4], rx_status[5]);
        if (rx_status[4] == 0xa5 && rx_status[5] == 0x5a) {
            printf(">>> SUCCESS: MCU IS IDLE (a5 5a)! <<<\n");
            break;
        }
        usleep(20000);
    }

    // 8. Read Sensor ID High, Low, and FW Version
    unsigned char tx_id_hi[5] = { 0x10, 0xef, 0x14, 0x00, 0x00 };
    unsigned char rx_id_hi[5] = { 0 };
    do_spi(spifd, tx_id_hi, rx_id_hi, 5);

    unsigned char tx_id_lo[5] = { 0x10, 0xef, 0x15, 0x00, 0x00 };
    unsigned char rx_id_lo[5] = { 0 };
    do_spi(spifd, tx_id_lo, rx_id_lo, 5);

    unsigned char tx_ver[5] = { 0x10, 0xef, 0x1a, 0x00, 0x00 };
    unsigned char rx_ver[5] = { 0 };
    do_spi(spifd, tx_ver, rx_ver, 5);

    printf("Sensor ID High: 0x%02x (expected 0x40)\n", rx_id_hi[4]);
    printf("Sensor ID Low:  0x%02x (expected 0x50)\n", rx_id_lo[4]);
    printf("FW Version:     0x%02x (expected 0x30)\n", rx_ver[4]);

    close(spifd);
    gpiod_line_request_release(req);
    gpiod_chip_close(chip);
    return 0;
}
