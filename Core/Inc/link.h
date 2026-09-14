/**
  ******************************************************************************
  * @file    link.h
  * @brief   Hot-plug-safe UART link to the four adjacent boards.
  *
  * Miso boards tile through four pogo-pin connectors carrying +5V and a UART
  * pair each. A TX pin driven push-pull at 3.3V into an *unpowered* neighbour
  * back-feeds its 3V3 rail through the RX pad's ESD clamp diode and parks it
  * near 2.6V, which stops that board's POR from ever releasing (it needs VDD
  * to rise from below ~1.6V). So TX pins default to high-impedance here and
  * are only driven once the port knows a live board is on the other end.
  *
  * See README.md ("Board linking") for the full state machine.
  ******************************************************************************
  */

#ifndef __LINK_H
#define __LINK_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

/* Physical sides. TOP is the bit-banged port (pins not assigned yet); its
 * descriptor exists but carries no UART, so link_tick() skips it. */
typedef enum {
  LINK_PORT_TOP = 0,
  LINK_PORT_BOTTOM,
  LINK_PORT_LEFT,
  LINK_PORT_RIGHT,
  LINK_PORT_COUNT
} link_port_t;

typedef enum {
  LINK_BLOCKED = 0,  /* RX low: an unpowered board is clamping the line */
  LINK_DOWN,         /* nothing there (or nothing heard yet); TX high-Z */
  LINK_PROBE,        /* driving TX for the duration of one HELLO */
  LINK_HANDSHAKE,    /* heard a peer, TX latched on, waiting for its ACK */
  LINK_UP            /* link established */
} link_state_t;

/* Wire format, same family as the CDC scan frame in main.c:
 *   A5 5A | type | len | payload[len] | checksum
 * checksum = byte sum of type, len and payload.
 * Types start at 0x10 to stay clear of FRAME_TYPE_SCAN (0x01). */
#define LINK_MSG_HELLO      0x10   /* uid[12] | fw | port | has_usb */
#define LINK_MSG_HELLO_ACK  0x11   /* same payload as HELLO */
#define LINK_MSG_PING       0x12   /* no payload; keeps the link alive */

#define LINK_MAX_PAYLOAD    32

void         link_init(void);
void         link_tick(uint32_t tick);
int          link_send(link_port_t p, uint8_t type, const uint8_t *payload, uint8_t len);
link_state_t link_state(link_port_t p);
const char  *link_state_name(link_port_t p);
char         link_state_char(link_port_t p);   /* b . p h U, for one-line summaries */
const char  *link_port_name(link_port_t p);

/* Peer identity, valid while the port is LINK_UP. */
const uint32_t *link_peer_uid(link_port_t p);
uint8_t         link_peer_port(link_port_t p);
uint8_t         link_peer_has_usb(link_port_t p);

void link_stats(link_port_t p, uint32_t *rx, uint32_t *tx, uint32_t *err);

/* True once this board has been enumerated over USB, i.e. it is the one
 * plugged into a host and is feeding +5V to the rest of the chain. Carried
 * in the handshake; the bring-up state machine does not depend on it. */
uint8_t link_self_has_usb(void);

/* ISR entry points, called from stm32g4xx_it.c. */
void link_irq_lpuart1(void);
void link_irq_usart1(void);
void link_irq_usart2(void);

#ifdef __cplusplus
}
#endif

#endif /* __LINK_H */
