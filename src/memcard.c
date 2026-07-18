#include <stdint.h>
#include <string.h>
#include <psxpad.h>
#include <hwregs_c.h>
#include "spi.h"
#include "memcard.h"

/* All card I/O here is synchronous: the SPI driver is suspended (memcard_begin
   -> SPI_Acquire) and each byte is exchanged by banging the SIO port directly,
   polling the /ACK latch in IRQ_STAT between bytes — the same scheme the BIOS
   card driver uses.

   Why not queue card commands through the SPI driver? Its completion model is
   "one request per timer tick", which holds for 9-byte pad polls but not for a
   140-byte card read: even at the slowest poll rate (the 16-bit timer caps a
   tick at ~15.5 ms) the next tick fires while the card is still clocking bytes
   out, the callback reports a truncated response, and every probe looks like
   "no card". Polling /ACK per byte has no such deadline. */

/* Poll-loop iteration caps (each iteration is an uncached MMIO read, ~0.3 us).
   RX: a byte takes ~32 us on the wire — 30k iterations is a wide margin.
   ACK: normally < 40 us, but a real card commits its flash sector between the
   write command's checksum and acknowledge, so be generous; the full wait is
   only ever burned when probing an empty slot. */
#define MC_RX_SPINS   30000u
#define MC_ACK_SPINS 400000u

static int mc_port;   /* 0/1, captured per transaction (bit 13 of SIO_CTRL) */

static void mc_ctrl(uint16_t bits) {
    SIO_CTRL(0) = bits | (uint16_t)(mc_port << 13);
}

/* Exchange one byte. expect_ack: every byte of a transaction except the last
   is followed by the card pulling /ACK; a missing /ACK means no card (or the
   end of a shorter-than-expected response). */
static int mc_xchg(uint8_t tx, uint8_t *rx, int expect_ack) {
    uint32_t t;

    SIO_DATA(0) = tx;
    for (t = 0; !(SIO_STAT(0) & 0x0002); t++)     /* RX byte ready */
        if (t > MC_RX_SPINS) return MC_TIMEOUT;
    *rx = (uint8_t)SIO_DATA(0);

    if (expect_ack) {
        for (t = 0; !(IRQ_STAT & (1 << 7)); t++)  /* /ACK latched (IRQ7) */
            if (t > MC_ACK_SPINS) return MC_NO_CARD;
        for (t = 0; (SIO_STAT(0) & 0x0080) && t < MC_RX_SPINS; t++)
            ;                                     /* wait for /ACK release */
        mc_ctrl(0x1013);                          /* reset the SIO IRQ flag */
        IRQ_STAT = (uint16_t)~(1 << 7);           /* clear the latch */
    }
    return MC_OK;
}

/* Run one full transaction: send tx[0..len-1] (tx[0] is the 0x81 card-address
   byte), collecting rx[i] = the byte received while tx[i] was sent. The card's
   response runs one byte behind the transmit stream, so rx[1..] lines up with
   the MemCardResponse struct (rx[1] = flags). Caller must hold SPI_Acquire. */
static int mc_transaction(int port, const uint8_t *tx, uint8_t *rx, int len) {
    int i, rc = MC_OK;
    mc_port = port;

    SIO_CTRL(0) = 0x0010;                         /* deselect, clear IRQ flag */
    for (i = 0; i < 500; i++) __asm__ volatile("");
    IRQ_STAT = (uint16_t)~(1 << 7);
    mc_ctrl(0x1003);                              /* /CS low + ACK IRQ enable */
    for (i = 0; i < 2000; i++) __asm__ volatile("");  /* ~20us select settle */

    for (i = 0; i < len; i++) {
        rc = mc_xchg(tx[i], &rx[i], i < len - 1);
        if (rc != MC_OK) break;
    }

    SIO_CTRL(0) = 0x0010;                         /* deselect (ends the command) */
    return rc;
}

void memcard_begin(void) { SPI_Acquire(); }   /* pad polling pauses while held */
void memcard_end(void)   { SPI_Release(); }

int memcard_present(int port) {
    /* Probe by reading frame 0 — plain reads work on every card, including
       third-party ones that lack Sony's IDENTIFY command. A bad checksum still
       proves something answered, so it counts as present. */
    uint8_t dummy[MC_FRAME_SIZE];
    int rc = memcard_read_frame(port, 0, dummy);
    return (rc == MC_OK || rc == MC_BAD_DATA);
}

int memcard_read_frame(int port, int lba, uint8_t *out) {
    uint8_t txb[MEMCARD_CMD_READ_LEN + 1], rxb[MEMCARD_CMD_READ_LEN + 1];

    memset(txb, 0, sizeof(txb));
    txb[0] = 0x81;                     /* card on this port */
    txb[1] = MCD_CMD_READ_SECTOR;
    txb[4] = (uint8_t)(lba >> 8);
    txb[5] = (uint8_t)(lba & 0xFF);

    int rc = mc_transaction(port, txb, rxb, (int)sizeof(txb));
    if (rc != MC_OK) return rc;

    MemCardResponse *r = (MemCardResponse *)&rxb[1];
    if (r->type1 != 0x5a || r->type2 != 0x5d) return MC_NO_CARD;
    if (r->read.stat != MCD_STAT_OK)          return MC_BAD_DATA;

    uint8_t cs = r->read.lba_h ^ r->read.lba_l;
    for (int i = 0; i < 128; i++) cs ^= r->read.data[i];
    if (cs != r->read.checksum) return MC_BAD_DATA;

    memcpy(out, r->read.data, 128);
    return MC_OK;
}

int memcard_write_frame(int port, int lba, const uint8_t *data) {
    uint8_t txb[MEMCARD_CMD_WRITE_LEN + 1], rxb[MEMCARD_CMD_WRITE_LEN + 1];

    memset(txb, 0, sizeof(txb));
    txb[0] = 0x81;
    txb[1] = MCD_CMD_WRITE_SECTOR;
    txb[4] = (uint8_t)(lba >> 8);
    txb[5] = (uint8_t)(lba & 0xFF);
    memcpy(&txb[6], data, 128);
    {
        uint8_t cs = txb[4] ^ txb[5];
        for (int i = 0; i < 128; i++) cs ^= data[i];
        txb[134] = cs;
    }

    int rc = mc_transaction(port, txb, rxb, (int)sizeof(txb));
    if (rc != MC_OK) return rc;

    MemCardResponse *r = (MemCardResponse *)&rxb[1];
    if (r->type1 != 0x5a || r->type2 != 0x5d) return MC_NO_CARD;
    if (r->write.stat != MCD_STAT_OK)         return MC_BAD_DATA;
    return MC_OK;
}
