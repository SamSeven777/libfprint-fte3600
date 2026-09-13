#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/spi/spidev.h>
#include <string.h>

int main() {
    int fd = open("/dev/spidev0.0", O_RDWR);
    if (fd < 0) return 1;

    for (int cs_high = 0; cs_high <= 1; cs_high++) {
        for (int m = 0; m < 4; m++) {
            unsigned char mode = m | (cs_high ? SPI_CS_HIGH : 0);
            ioctl(fd, SPI_IOC_WR_MODE, &mode);

            unsigned char tx[6] = { 0x10, 0xef, 0x80, 0x00, 0x00, 0x00 };
            unsigned char rx[6] = { 0 };
            struct spi_ioc_transfer tr;
            memset(&tr, 0, sizeof(tr));
            tr.tx_buf = (unsigned long)tx;
            tr.rx_buf = (unsigned long)rx;
            tr.len = 6;
            tr.speed_hz = 1000000;
            tr.bits_per_word = 8;
            ioctl(fd, SPI_IOC_MESSAGE(1), &tr);
            if (rx[4] != 0 || rx[5] != 0) {
                printf("FOUND RESPONSE! CS_HIGH=%d Mode=%d: %02x %02x\n", cs_high, m, rx[4], rx[5]);
            }
        }
    }
    printf("CS test done.\n");
    close(fd);
    return 0;
}
