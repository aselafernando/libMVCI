/* mvci_io.h — byte transport abstraction.
 *
 *   Linux  : termios on a /dev/ttyUSBx node (ftdi_sio VCP driver)
 *   Windows: FTDI D2XX (ftd2xx.dll), opened by description "M-VCI"
 *
 * mvci_io_open() configures 115200 8N1 and pulses DTR to hard-reset the
 * adapter MCU, then waits for it to boot — identical on both platforms.
 */
#ifndef MVCI_IO_H
#define MVCI_IO_H

#include <stdint.h>

typedef struct mvci_io mvci_io_t;

/* port: Linux = tty path (e.g. "/dev/ttyUSB0"); Windows = FTDI description
 * (NULL/empty => "M-VCI"). Returns NULL on failure. */
mvci_io_t *mvci_io_open(const char *port);
void       mvci_io_close(mvci_io_t *io);

/* write all n bytes; returns n on success, -1 on error */
int  mvci_io_write(mvci_io_t *io, const uint8_t *buf, int n);

/* read up to n bytes within timeout_ms; returns bytes read (may be < n) or -1 */
int  mvci_io_read(mvci_io_t *io, uint8_t *buf, int n, int timeout_ms);

/* discard any buffered input */
void mvci_io_purge_rx(mvci_io_t *io);

#endif /* MVCI_IO_H */
