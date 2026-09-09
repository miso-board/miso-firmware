/**
  ******************************************************************************
  * @file    usbd_composite.h
  * @brief   Composite USB device: CDC ACM (virtual COM port) + USB-MIDI.
  *
  * Hand written because CubeMX generates only single-class devices. The CDC
  * half keeps the exact app-layer API the companion protocol already uses
  * (CDC_Transmit_FS / CDC_IsTxBusy / Miso_CDC_OnRx); the MIDI half adds a
  * standard 1-in/1-out USB-MIDI 1.0 port for note output.
  ******************************************************************************
  */

#ifndef __USBD_COMPOSITE_H
#define __USBD_COMPOSITE_H

#ifdef __cplusplus
extern "C" {
#endif

#include "usbd_ioreq.h"
#include "usbd_cdc.h"

/* Interface numbers */
#define COMP_ITF_CDC_CTRL   0x00U
#define COMP_ITF_CDC_DATA   0x01U
#define COMP_ITF_MIDI_AC    0x02U
#define COMP_ITF_MIDI_MS    0x03U

/* Endpoints (unchanged for CDC so the existing protocol is untouched) */
#define CDC_IN_EP           0x81U
#define CDC_OUT_EP          0x01U
#define CDC_CMD_EP          0x82U
#define MIDI_IN_EP          0x83U
#define MIDI_OUT_EP         0x03U

#define CDC_DATA_FS_MAX_PACKET_SIZE  64U
#define CDC_CMD_PACKET_SIZE          8U
#define MIDI_PACKET_SIZE             64U

extern USBD_ClassTypeDef USBD_Composite;

/* MIDI: queue a 4-byte USB-MIDI event packet. Returns 1 if queued. */
uint8_t USBD_MIDI_Send(const uint8_t packet[4]);
/* Drain the MIDI queue to the endpoint; safe to call from the main loop. */
void    USBD_MIDI_Flush(void);
/* True once the host has configured the device. */
uint8_t USBD_MIDI_Ready(void);

/* CDC side, used by usbd_cdc_if.c and usb_device.c */
uint8_t USBD_Composite_CDC_Transmit(USBD_HandleTypeDef *pdev, uint8_t *buf, uint16_t len);
uint8_t USBD_Composite_CDC_IsBusy(USBD_HandleTypeDef *pdev);
uint8_t USBD_Composite_RegisterCDCInterface(USBD_HandleTypeDef *pdev, USBD_CDC_ItfTypeDef *fops);

#ifdef __cplusplus
}
#endif

#endif /* __USBD_COMPOSITE_H */
