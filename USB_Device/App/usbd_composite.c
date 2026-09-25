/**
  ******************************************************************************
  * @file    usbd_composite.c
  * @brief   Composite USB device: CDC ACM + USB-MIDI 1.0.
  *
  * Layout of the configuration descriptor:
  *   IAD  -> itf 0  CDC ACM control   (EP 0x82 interrupt IN)
  *           itf 1  CDC data          (EP 0x81 bulk IN, 0x01 bulk OUT)
  *   IAD  -> itf 2  Audio Control     (no endpoints)
  *           itf 3  MIDIStreaming     (EP 0x83 bulk IN, 0x03 bulk OUT)
  *
  * The CDC endpoints and the app-layer API are identical to the CubeMX CDC
  * class that came before, so the companion protocol needed no changes.
  ******************************************************************************
  */

#include "usbd_composite.h"
#include "usbd_ctlreq.h"
#include "usbd_cdc_if.h"

/* CDC ACM class requests */
#define CDC_SET_LINE_CODING_REQ         0x20U
#define CDC_GET_LINE_CODING_REQ         0x21U
#define CDC_SET_CONTROL_LINE_STATE_REQ  0x22U

/* 9 config + 8 IAD + (9+5+5+4+5+7) CDC ctrl + (9+7+7) CDC data
 * + 8 IAD + (9+9) audio control + (9+7+6+6+9+9+9+5+9+5) MIDIStreaming */
#define USB_CONFIG_DESC_SIZE  175U

typedef struct {
  uint8_t  cdc_rx[CDC_DATA_FS_MAX_PACKET_SIZE];
  uint8_t  cdc_cmd_op;
  uint8_t  cdc_cmd_len;
  uint8_t  cdc_line_coding[7];
  __IO uint32_t cdc_tx_busy;

  uint8_t  midi_rx[MIDI_PACKET_SIZE];
  /* Ring of 4-byte MIDI event packets awaiting transmission. */
  uint8_t  midi_q[64][4];
  __IO uint16_t midi_head;
  __IO uint16_t midi_tail;
  __IO uint8_t  midi_tx_busy;
  uint8_t  midi_tx_buf[MIDI_PACKET_SIZE];
} COMPOSITE_HandleTypeDef;

static COMPOSITE_HandleTypeDef comp_state;
static USBD_HandleTypeDef *midi_pdev;

/* ---------------------------------------------------------------------------
 * Configuration descriptor
 * ------------------------------------------------------------------------- */
__ALIGN_BEGIN static uint8_t USBD_Composite_CfgDesc[USB_CONFIG_DESC_SIZE] __ALIGN_END = {
  /* Configuration */
  0x09, USB_DESC_TYPE_CONFIGURATION,
  LOBYTE(USB_CONFIG_DESC_SIZE), HIBYTE(USB_CONFIG_DESC_SIZE),
  0x04,                   /* bNumInterfaces */
  0x01,                   /* bConfigurationValue */
  0x00,                   /* iConfiguration */
  0xC0,                   /* bmAttributes: self powered */
  0x32,                   /* bMaxPower 100 mA */

  /* ---- IAD: CDC (interfaces 0-1) ---- */
  0x08, 0x0B, COMP_ITF_CDC_CTRL, 0x02, 0x02, 0x02, 0x01, 0x00,

  /* Interface 0: CDC ACM control */
  0x09, USB_DESC_TYPE_INTERFACE, COMP_ITF_CDC_CTRL, 0x00, 0x01,
  0x02, 0x02, 0x01, 0x00,
  /* CDC header */
  0x05, 0x24, 0x00, 0x10, 0x01,
  /* CDC call management */
  0x05, 0x24, 0x01, 0x00, COMP_ITF_CDC_DATA,
  /* CDC ACM */
  0x04, 0x24, 0x02, 0x02,
  /* CDC union */
  0x05, 0x24, 0x06, COMP_ITF_CDC_CTRL, COMP_ITF_CDC_DATA,
  /* Notification endpoint */
  0x07, USB_DESC_TYPE_ENDPOINT, CDC_CMD_EP, 0x03,
  LOBYTE(CDC_CMD_PACKET_SIZE), HIBYTE(CDC_CMD_PACKET_SIZE), 0x10,

  /* Interface 1: CDC data */
  0x09, USB_DESC_TYPE_INTERFACE, COMP_ITF_CDC_DATA, 0x00, 0x02,
  0x0A, 0x00, 0x00, 0x00,
  0x07, USB_DESC_TYPE_ENDPOINT, CDC_OUT_EP, 0x02,
  LOBYTE(CDC_DATA_FS_MAX_PACKET_SIZE), HIBYTE(CDC_DATA_FS_MAX_PACKET_SIZE), 0x00,
  0x07, USB_DESC_TYPE_ENDPOINT, CDC_IN_EP, 0x02,
  LOBYTE(CDC_DATA_FS_MAX_PACKET_SIZE), HIBYTE(CDC_DATA_FS_MAX_PACKET_SIZE), 0x00,

  /* ---- IAD: Audio/MIDI (interfaces 2-3) ---- */
  0x08, 0x0B, COMP_ITF_MIDI_AC, 0x02, 0x01, 0x01, 0x00, 0x00,

  /* Interface 2: Audio Control */
  0x09, USB_DESC_TYPE_INTERFACE, COMP_ITF_MIDI_AC, 0x00, 0x00,
  0x01, 0x01, 0x00, 0x00,
  /* Class-specific AC header: one streaming interface (itf 3) */
  0x09, 0x24, 0x01, 0x00, 0x01, 0x09, 0x00, 0x01, COMP_ITF_MIDI_MS,

  /* Interface 3: MIDIStreaming */
  0x09, USB_DESC_TYPE_INTERFACE, COMP_ITF_MIDI_MS, 0x00, 0x02,
  0x01, 0x03, 0x00, 0x00,
  /* Class-specific MS header, wTotalLength = 7+6+6+9+9 = 37 */
  0x07, 0x24, 0x01, 0x00, 0x01, 0x25, 0x00,
  /* MIDI IN jack, embedded, ID 1 */
  0x06, 0x24, 0x02, 0x01, 0x01, 0x00,
  /* MIDI IN jack, external, ID 2 */
  0x06, 0x24, 0x02, 0x02, 0x02, 0x00,
  /* MIDI OUT jack, embedded, ID 3, sourced from external in jack 2 */
  0x09, 0x24, 0x03, 0x01, 0x03, 0x01, 0x02, 0x01, 0x00,
  /* MIDI OUT jack, external, ID 4, sourced from embedded in jack 1 */
  0x09, 0x24, 0x03, 0x02, 0x04, 0x01, 0x01, 0x01, 0x00,
  /* Bulk OUT endpoint (host -> device) */
  0x09, USB_DESC_TYPE_ENDPOINT, MIDI_OUT_EP, 0x02,
  LOBYTE(MIDI_PACKET_SIZE), HIBYTE(MIDI_PACKET_SIZE), 0x00, 0x00, 0x00,
  /* Class-specific MS bulk OUT: associated with embedded in jack 1 */
  0x05, 0x25, 0x01, 0x01, 0x01,
  /* Bulk IN endpoint (device -> host) */
  0x09, USB_DESC_TYPE_ENDPOINT, MIDI_IN_EP, 0x02,
  LOBYTE(MIDI_PACKET_SIZE), HIBYTE(MIDI_PACKET_SIZE), 0x00, 0x00, 0x00,
  /* Class-specific MS bulk IN: associated with embedded out jack 3 */
  0x05, 0x25, 0x01, 0x01, 0x03,
};

__ALIGN_BEGIN static uint8_t USBD_Composite_DeviceQualifierDesc[USB_LEN_DEV_QUALIFIER_DESC] __ALIGN_END = {
  USB_LEN_DEV_QUALIFIER_DESC, USB_DESC_TYPE_DEVICE_QUALIFIER,
  0x00, 0x02, 0x00, 0x00, 0x00, 0x40, 0x01, 0x00,
};

/* ---------------------------------------------------------------------------
 * MIDI transmit queue
 * ------------------------------------------------------------------------- */
#define MIDI_Q_LEN  (sizeof(comp_state.midi_q) / sizeof(comp_state.midi_q[0]))

uint8_t USBD_MIDI_Ready(void)
{
  return (midi_pdev != NULL) && (midi_pdev->dev_state == USBD_STATE_CONFIGURED);
}

uint8_t USBD_MIDI_Send(const uint8_t packet[4])
{
  uint16_t next = (uint16_t)((comp_state.midi_head + 1U) % MIDI_Q_LEN);
  if (next == comp_state.midi_tail) {
    return 0; /* queue full: drop rather than block the scan loop */
  }
  for (uint8_t i = 0; i < 4U; i++) {
    comp_state.midi_q[comp_state.midi_head][i] = packet[i];
  }
  comp_state.midi_head = next;
  USBD_MIDI_Flush();
  return 1;
}

void USBD_MIDI_Flush(void)
{
  if (!USBD_MIDI_Ready() || comp_state.midi_tx_busy) {
    return;
  }
  if (comp_state.midi_head == comp_state.midi_tail) {
    return;
  }
  uint16_t len = 0;
  while ((comp_state.midi_tail != comp_state.midi_head) && (len <= (MIDI_PACKET_SIZE - 4U))) {
    for (uint8_t i = 0; i < 4U; i++) {
      comp_state.midi_tx_buf[len + i] = comp_state.midi_q[comp_state.midi_tail][i];
    }
    len += 4U;
    comp_state.midi_tail = (uint16_t)((comp_state.midi_tail + 1U) % MIDI_Q_LEN);
  }
  if (len > 0U) {
    comp_state.midi_tx_busy = 1U;
    if (USBD_LL_Transmit(midi_pdev, MIDI_IN_EP, comp_state.midi_tx_buf, len) != USBD_OK) {
      comp_state.midi_tx_busy = 0U;
    }
  }
}

/* ---------------------------------------------------------------------------
 * Class interface
 * ------------------------------------------------------------------------- */
static uint8_t Composite_Init(USBD_HandleTypeDef *pdev, uint8_t cfgidx)
{
  UNUSED(cfgidx);

  USBD_LL_OpenEP(pdev, CDC_IN_EP,   USBD_EP_TYPE_BULK, CDC_DATA_FS_MAX_PACKET_SIZE);
  USBD_LL_OpenEP(pdev, CDC_OUT_EP,  USBD_EP_TYPE_BULK, CDC_DATA_FS_MAX_PACKET_SIZE);
  USBD_LL_OpenEP(pdev, CDC_CMD_EP,  USBD_EP_TYPE_INTR, CDC_CMD_PACKET_SIZE);
  USBD_LL_OpenEP(pdev, MIDI_IN_EP,  USBD_EP_TYPE_BULK, MIDI_PACKET_SIZE);
  USBD_LL_OpenEP(pdev, MIDI_OUT_EP, USBD_EP_TYPE_BULK, MIDI_PACKET_SIZE);

  pdev->ep_in[CDC_IN_EP & 0xFU].is_used    = 1U;
  pdev->ep_out[CDC_OUT_EP & 0xFU].is_used  = 1U;
  pdev->ep_in[CDC_CMD_EP & 0xFU].is_used   = 1U;
  pdev->ep_in[MIDI_IN_EP & 0xFU].is_used   = 1U;
  pdev->ep_out[MIDI_OUT_EP & 0xFU].is_used = 1U;

  pdev->pClassData = &comp_state;
  midi_pdev = pdev;

  comp_state.cdc_tx_busy = 0U;
  comp_state.cdc_cmd_op = 0xFFU;
  comp_state.midi_tx_busy = 0U;
  comp_state.midi_head = 0U;
  comp_state.midi_tail = 0U;

  /* 115200 8N1 — nominal for CDC, but hosts read it back. */
  comp_state.cdc_line_coding[0] = 0x00;
  comp_state.cdc_line_coding[1] = 0xC2;
  comp_state.cdc_line_coding[2] = 0x01;
  comp_state.cdc_line_coding[3] = 0x00;
  comp_state.cdc_line_coding[4] = 0x00;
  comp_state.cdc_line_coding[5] = 0x00;
  comp_state.cdc_line_coding[6] = 0x08;

  ((USBD_CDC_ItfTypeDef *)pdev->pUserData)->Init();

  USBD_LL_PrepareReceive(pdev, CDC_OUT_EP, comp_state.cdc_rx, CDC_DATA_FS_MAX_PACKET_SIZE);
  USBD_LL_PrepareReceive(pdev, MIDI_OUT_EP, comp_state.midi_rx, MIDI_PACKET_SIZE);
  return USBD_OK;
}

static uint8_t Composite_DeInit(USBD_HandleTypeDef *pdev, uint8_t cfgidx)
{
  UNUSED(cfgidx);
  USBD_LL_CloseEP(pdev, CDC_IN_EP);
  USBD_LL_CloseEP(pdev, CDC_OUT_EP);
  USBD_LL_CloseEP(pdev, CDC_CMD_EP);
  USBD_LL_CloseEP(pdev, MIDI_IN_EP);
  USBD_LL_CloseEP(pdev, MIDI_OUT_EP);

  pdev->ep_in[CDC_IN_EP & 0xFU].is_used    = 0U;
  pdev->ep_out[CDC_OUT_EP & 0xFU].is_used  = 0U;
  pdev->ep_in[CDC_CMD_EP & 0xFU].is_used   = 0U;
  pdev->ep_in[MIDI_IN_EP & 0xFU].is_used   = 0U;
  pdev->ep_out[MIDI_OUT_EP & 0xFU].is_used = 0U;

  ((USBD_CDC_ItfTypeDef *)pdev->pUserData)->DeInit();
  pdev->pClassData = NULL;
  midi_pdev = NULL;
  return USBD_OK;
}

static uint8_t Composite_Setup(USBD_HandleTypeDef *pdev, USBD_SetupReqTypedef *req)
{
  COMPOSITE_HandleTypeDef *h = &comp_state;
  uint16_t status_info = 0U;
  uint8_t ret = USBD_OK;

  switch (req->bmRequest & USB_REQ_TYPE_MASK) {
    case USB_REQ_TYPE_CLASS:
      /* Only the CDC interfaces define class requests; MIDI defines none. */
      if (req->wLength != 0U) {
        if ((req->bmRequest & 0x80U) != 0U) {
          if (req->bRequest == CDC_GET_LINE_CODING_REQ) {
            USBD_CtlSendData(pdev, h->cdc_line_coding, MIN(req->wLength, 7U));
          } else {
            ((USBD_CDC_ItfTypeDef *)pdev->pUserData)->Control(req->bRequest,
                                                              (uint8_t *)h->cdc_rx,
                                                              req->wLength);
            USBD_CtlSendData(pdev, (uint8_t *)h->cdc_rx, req->wLength);
          }
        } else {
          h->cdc_cmd_op = (uint8_t)req->bRequest;
          h->cdc_cmd_len = (uint8_t)MIN(req->wLength, CDC_DATA_FS_MAX_PACKET_SIZE);
          USBD_CtlPrepareRx(pdev, (uint8_t *)h->cdc_rx, h->cdc_cmd_len);
        }
      } else {
        ((USBD_CDC_ItfTypeDef *)pdev->pUserData)->Control(req->bRequest,
                                                          (uint8_t *)req, 0U);
      }
      break;

    case USB_REQ_TYPE_STANDARD:
      switch (req->bRequest) {
        case USB_REQ_GET_STATUS:
          if (pdev->dev_state == USBD_STATE_CONFIGURED) {
            USBD_CtlSendData(pdev, (uint8_t *)&status_info, 2U);
          } else {
            USBD_CtlError(pdev, req);
            ret = USBD_FAIL;
          }
          break;
        case USB_REQ_GET_INTERFACE:
          if (pdev->dev_state == USBD_STATE_CONFIGURED) {
            status_info = 0U;
            USBD_CtlSendData(pdev, (uint8_t *)&status_info, 1U);
          } else {
            USBD_CtlError(pdev, req);
            ret = USBD_FAIL;
          }
          break;
        case USB_REQ_SET_INTERFACE:
          if (pdev->dev_state != USBD_STATE_CONFIGURED) {
            USBD_CtlError(pdev, req);
            ret = USBD_FAIL;
          }
          break;
        case USB_REQ_CLEAR_FEATURE:
          break;
        default:
          USBD_CtlError(pdev, req);
          ret = USBD_FAIL;
          break;
      }
      break;

    default:
      USBD_CtlError(pdev, req);
      ret = USBD_FAIL;
      break;
  }
  return ret;
}

static uint8_t Composite_DataIn(USBD_HandleTypeDef *pdev, uint8_t epnum)
{
  if ((epnum & 0x7FU) == (MIDI_IN_EP & 0x7FU)) {
    /* Release the transmitter, and nothing else.
     *
     * Flushing from here raced the main loop and silently ate notes.
     * USBD_MIDI_Flush() advances midi_tail and fills midi_tx_buf in its copy
     * loop and only sets midi_tx_busy afterwards, so this handler -- USB_LP at
     * preempt priority 0, above everything -- could interrupt a flush mid-copy,
     * find busy == 0 with a non-empty queue, drain the remaining entries over
     * the buffer the main loop had already staged, and transmit. The main loop
     * then resumed, finished its own loop, and transmitted its stale len over
     * the overwritten buffer. Packets were consumed from the ring and never
     * delivered, and two transfers were queued on one endpoint -- uncounted,
     * and the reason a fast trill dropped roughly one note a second.
     *
     * USBD_MIDI_Flush() is now reached only from main-loop context: the loop in
     * main(), and USBD_MIDI_Send() below, which is only ever called from it
     * (midi_send -> midi_note_on/off <- keys_process, mesh dispatch, reconcile).
     * That context is single-threaded, so the window is closed. The cost is up
     * to one scan of latency, ~439 us, before the drain is re-armed -- well
     * under the 1 ms USB frame the packets are waiting for anyway. */
    comp_state.midi_tx_busy = 0U;
    return USBD_OK;
  }
  if ((epnum & 0x7FU) == (CDC_IN_EP & 0x7FU)) {
    PCD_HandleTypeDef *hpcd = (PCD_HandleTypeDef *)pdev->pData;
    if (hpcd->IN_ep[epnum].xfer_len &&
        (hpcd->IN_ep[epnum].xfer_len % hpcd->IN_ep[epnum].maxpacket) == 0U) {
      /* Zero-length packet terminates a transfer that filled the last packet. */
      hpcd->IN_ep[epnum].xfer_len = 0U;
      USBD_LL_Transmit(pdev, epnum, NULL, 0U);
    } else {
      comp_state.cdc_tx_busy = 0U;
    }
  }
  return USBD_OK;
}

static uint8_t Composite_DataOut(USBD_HandleTypeDef *pdev, uint8_t epnum)
{
  uint32_t len = USBD_LL_GetRxDataSize(pdev, epnum);

  if (epnum == (MIDI_OUT_EP & 0x7FU)) {
    /* Input from the host is accepted and discarded for now. */
    USBD_LL_PrepareReceive(pdev, MIDI_OUT_EP, comp_state.midi_rx, MIDI_PACKET_SIZE);
    return USBD_OK;
  }

  if (epnum == (CDC_OUT_EP & 0x7FU)) {
    ((USBD_CDC_ItfTypeDef *)pdev->pUserData)->Receive(comp_state.cdc_rx, &len);
    USBD_LL_PrepareReceive(pdev, CDC_OUT_EP, comp_state.cdc_rx, CDC_DATA_FS_MAX_PACKET_SIZE);
  }
  return USBD_OK;
}

static uint8_t Composite_EP0_RxReady(USBD_HandleTypeDef *pdev)
{
  COMPOSITE_HandleTypeDef *h = &comp_state;
  if (h->cdc_cmd_op != 0xFFU) {
    if (h->cdc_cmd_op == CDC_SET_LINE_CODING_REQ) {
      for (uint8_t i = 0; i < 7U; i++) {
        h->cdc_line_coding[i] = h->cdc_rx[i];
      }
    }
    ((USBD_CDC_ItfTypeDef *)pdev->pUserData)->Control(h->cdc_cmd_op,
                                                      (uint8_t *)h->cdc_rx,
                                                      (uint16_t)h->cdc_cmd_len);
    h->cdc_cmd_op = 0xFFU;
  }
  return USBD_OK;
}

static uint8_t *Composite_GetCfgDesc(uint16_t *length)
{
  *length = (uint16_t)sizeof(USBD_Composite_CfgDesc);
  return USBD_Composite_CfgDesc;
}

static uint8_t *Composite_GetDeviceQualifierDesc(uint16_t *length)
{
  *length = (uint16_t)sizeof(USBD_Composite_DeviceQualifierDesc);
  return USBD_Composite_DeviceQualifierDesc;
}

USBD_ClassTypeDef USBD_Composite = {
  Composite_Init,
  Composite_DeInit,
  Composite_Setup,
  NULL,                      /* EP0_TxSent */
  Composite_EP0_RxReady,
  Composite_DataIn,
  Composite_DataOut,
  NULL,                      /* SOF */
  NULL,                      /* IsoINIncomplete */
  NULL,                      /* IsoOUTIncomplete */
  Composite_GetCfgDesc,      /* HS */
  Composite_GetCfgDesc,      /* FS */
  Composite_GetCfgDesc,      /* OtherSpeed */
  Composite_GetDeviceQualifierDesc,
};

/* ---------------------------------------------------------------------------
 * CDC app-layer API (same signatures as the CubeMX CDC class it replaces)
 * ------------------------------------------------------------------------- */
uint8_t USBD_Composite_CDC_Transmit(USBD_HandleTypeDef *pdev, uint8_t *buf, uint16_t len)
{
  if (pdev->dev_state != USBD_STATE_CONFIGURED) {
    return USBD_FAIL;
  }
  if (comp_state.cdc_tx_busy) {
    return USBD_BUSY;
  }
  comp_state.cdc_tx_busy = 1U;
  if (USBD_LL_Transmit(pdev, CDC_IN_EP, buf, len) != USBD_OK) {
    comp_state.cdc_tx_busy = 0U;
    return USBD_FAIL;
  }
  return USBD_OK;
}

uint8_t USBD_Composite_CDC_IsBusy(USBD_HandleTypeDef *pdev)
{
  if (pdev->dev_state != USBD_STATE_CONFIGURED) {
    return 1U;
  }
  return (uint8_t)comp_state.cdc_tx_busy;
}

/* Stands in for USBD_CDC_RegisterInterface: stores the app callback table. */
uint8_t USBD_Composite_RegisterCDCInterface(USBD_HandleTypeDef *pdev, USBD_CDC_ItfTypeDef *fops)
{
  if (fops == NULL) {
    return USBD_FAIL;
  }
  pdev->pUserData = fops;
  return USBD_OK;
}
