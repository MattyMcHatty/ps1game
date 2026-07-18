#ifndef MEMCARD_H
#define MEMCARD_H

#include <stdint.h>

/* Low-level PlayStation memory-card access. We cannot use the BIOS card API
   because the project's custom SPI driver (spi.c) owns the bus, and we cannot
   queue card commands through that driver either — its tick-driven completion
   only fits small pad packets, not 140-byte card transfers. Instead, begin()
   suspends the driver (SPI_Acquire) and each command is exchanged synchronously
   on the SIO port, polling the /ACK handshake per byte like the BIOS does.

   Card geometry: 128 KB = 16 blocks x 8 KB; each block = 64 frames x 128 bytes.
   Frame addresses ("LBA") are absolute: block B, frame F -> lba = B*64 + F.
   Block 0 is the directory; blocks 1..15 hold saves. */

#define MC_FRAME_SIZE        128
#define MC_FRAMES_PER_BLOCK   64
#define MC_NUM_BLOCKS         16

/* Result codes (0 = success, negatives = failure). */
#define MC_OK          0
#define MC_NO_CARD    (-1)   /* no card responded / bad handshake */
#define MC_BAD_DATA   (-2)   /* card responded but checksum/status was bad */
#define MC_TIMEOUT    (-3)   /* the SPI transfer never completed */

/* Bracket a batch of card operations: begin() suspends the SPI driver and takes
   the port (pad polling pauses); end() hands it back. Every read/write below
   must happen between the two. */
void memcard_begin(void);
void memcard_end(void);

/* port: 0 = memory card slot 1, 1 = slot 2. */
int  memcard_present(int port);                              /* 1 if a card is there */
int  memcard_read_frame(int port, int lba, uint8_t *out);   /* out: 128 bytes */
int  memcard_write_frame(int port, int lba, const uint8_t *data); /* data: 128 bytes */

#endif
