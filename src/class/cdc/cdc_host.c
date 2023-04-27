/*
 * The MIT License (MIT)
 *
 * Copyright (c) 2019 Ha Thach (tinyusb.org)
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 *
 * This file is part of the TinyUSB stack.
 */

#include "tusb_option.h"

#if (CFG_TUH_ENABLED && CFG_TUH_CDC)

#include "host/usbh.h"
#include "host/usbh_classdriver.h"

#include "cdc_host.h"

#if CFG_TUH_CDC_FTDI
  #include "serial/ftdi_sio.h"
#endif

#if CFG_TUH_CDC_CP210X
  #include "serial/cp210x.h"
#endif

// Debug level, TUSB_CFG_DEBUG must be at least this level for debug message
#define CDCH_DEBUG   2

#define TU_LOG_CDCH(...)   TU_LOG(CDCH_DEBUG, __VA_ARGS__)

//--------------------------------------------------------------------+
// MACRO CONSTANT TYPEDEF
//--------------------------------------------------------------------+

enum {
  SERIAL_PROTOCOL_ACM = 0,
  SERIAL_PROTOCOL_FTDI,
  SERIAL_PROTOCOL_CP210X,
};

typedef struct {
  uint8_t daddr;
  uint8_t bInterfaceNumber;
  uint8_t bInterfaceSubClass;
  uint8_t bInterfaceProtocol;

  uint8_t serial_protocol;
  cdc_acm_capability_t acm_capability;
  uint8_t ep_notif;

  cdc_line_coding_t line_coding;  // Baudrate, stop bits, parity, data width
  uint8_t line_state;             // DTR (bit0), RTS (bit1)

  tuh_xfer_cb_t user_control_cb;

  struct {
    tu_edpt_stream_t tx;
    tu_edpt_stream_t rx;

    uint8_t tx_ff_buf[CFG_TUH_CDC_TX_BUFSIZE];
    CFG_TUH_MEM_ALIGN uint8_t tx_ep_buf[CFG_TUH_CDC_TX_EPSIZE];

    uint8_t rx_ff_buf[CFG_TUH_CDC_TX_BUFSIZE];
    CFG_TUH_MEM_ALIGN uint8_t rx_ep_buf[CFG_TUH_CDC_TX_EPSIZE];
  } stream;

} cdch_interface_t;

//--------------------------------------------------------------------+
// INTERNAL OBJECT & FUNCTION DECLARATION
//--------------------------------------------------------------------+

CFG_TUH_MEM_SECTION
static cdch_interface_t cdch_data[CFG_TUH_CDC];

static inline cdch_interface_t* get_itf(uint8_t idx)
{
  TU_ASSERT(idx < CFG_TUH_CDC, NULL);
  cdch_interface_t* p_cdc = &cdch_data[idx];

  return (p_cdc->daddr != 0) ? p_cdc : NULL;
}

static inline uint8_t get_idx_by_ep_addr(uint8_t daddr, uint8_t ep_addr)
{
  for(uint8_t i=0; i<CFG_TUH_CDC; i++)
  {
    cdch_interface_t* p_cdc = &cdch_data[i];
    if ( (p_cdc->daddr == daddr) &&
         (ep_addr == p_cdc->ep_notif || ep_addr == p_cdc->stream.rx.ep_addr || ep_addr == p_cdc->stream.tx.ep_addr))
    {
      return i;
    }
  }

  return TUSB_INDEX_INVALID_8;
}


static cdch_interface_t* make_new_itf(uint8_t daddr, tusb_desc_interface_t const *itf_desc)
{
  for(uint8_t i=0; i<CFG_TUH_CDC; i++)
  {
    if (cdch_data[i].daddr == 0) {
      cdch_interface_t* p_cdc = &cdch_data[i];

      p_cdc->daddr              = daddr;
      p_cdc->bInterfaceNumber   = itf_desc->bInterfaceNumber;
      p_cdc->bInterfaceSubClass = itf_desc->bInterfaceSubClass;
      p_cdc->bInterfaceProtocol = itf_desc->bInterfaceProtocol;
      p_cdc->line_state         = 0;
      return p_cdc;
    }
  }

  return NULL;
}

static inline bool support_line_request(cdch_interface_t const* p_cdc) {
  return (p_cdc->serial_protocol == SERIAL_PROTOCOL_ACM && p_cdc->acm_capability.support_line_request) ||
          (p_cdc->serial_protocol == SERIAL_PROTOCOL_FTDI);
}

static bool open_ep_stream_pair(cdch_interface_t* p_cdc , tusb_desc_endpoint_t const *desc_ep);
static void set_config_complete(cdch_interface_t * p_cdc, uint8_t idx, uint8_t itf_num);
static void cdch_internal_control_complete(tuh_xfer_t* xfer);

//--------------------------------------------------------------------+
// FTDI
//--------------------------------------------------------------------+
#if CFG_TUH_CDC_FTDI

static uint16_t const ftdi_pids[] = { TU_FTDI_PID_LIST };
enum {
  FTDI_PID_COUNT = sizeof(ftdi_pids) / sizeof(ftdi_pids[0])
};

enum {
  CONFIG_FTDI_RESET = 0,
  CONFIG_FTDI_MODEM_CTRL,
  CONFIG_FTDI_SET_BAUDRATE,
  CONFIG_FTDI_SET_DATA,
  CONFIG_FTDI_COMPLETE
};

static bool ftdi_open(uint8_t daddr, const tusb_desc_interface_t *itf_desc, uint16_t max_len) {
  // FTDI Interface includes 1 vendor interface + 2 bulk endpoints
  TU_VERIFY(itf_desc->bInterfaceSubClass == 0xff && itf_desc->bInterfaceProtocol == 0xff && itf_desc->bNumEndpoints == 2);
  TU_VERIFY(sizeof(tusb_desc_interface_t) + 2*sizeof(tusb_desc_endpoint_t) <= max_len);

  cdch_interface_t * p_cdc = make_new_itf(daddr, itf_desc);
  TU_VERIFY(p_cdc);

  TU_LOG_CDCH("FTDI opened\r\n");

  p_cdc->serial_protocol = SERIAL_PROTOCOL_FTDI;

  // endpoint pair
  tusb_desc_endpoint_t const * desc_ep = (tusb_desc_endpoint_t const *) tu_desc_next(itf_desc);

  // data endpoints expected to be in pairs
  return open_ep_stream_pair(p_cdc, desc_ep);
}

// set request without data
static bool ftdi_sio_set_request(cdch_interface_t* p_cdc, uint8_t command, uint16_t value, tuh_xfer_cb_t complete_cb, uintptr_t user_data) {
  tusb_control_request_t const request = {
    .bmRequestType_bit = {
      .recipient = TUSB_REQ_RCPT_DEVICE,
      .type      = TUSB_REQ_TYPE_VENDOR,
      .direction = TUSB_DIR_OUT
    },
    .bRequest = command,
    .wValue   = tu_htole16(value),
    .wIndex   = 0,
    .wLength  = 0
  };

  tuh_xfer_t xfer = {
    .daddr       = p_cdc->daddr,
    .ep_addr     = 0,
    .setup       = &request,
    .buffer      = NULL,
    .complete_cb = complete_cb,
    .user_data   = user_data
  };

  return tuh_control_xfer(&xfer);
}

static bool ftdi_sio_reset(cdch_interface_t* p_cdc, tuh_xfer_cb_t complete_cb, uintptr_t user_data)
{
  return ftdi_sio_set_request(p_cdc, FTDI_SIO_RESET, FTDI_SIO_RESET_SIO, complete_cb, user_data);
}

static bool ftdi_sio_set_modem_ctrl(cdch_interface_t* p_cdc, uint16_t line_state, tuh_xfer_cb_t complete_cb, uintptr_t user_data)
{
  p_cdc->user_control_cb = complete_cb;
  TU_ASSERT(ftdi_sio_set_request(p_cdc, FTDI_SIO_MODEM_CTRL, 0x0300 | line_state, cdch_internal_control_complete, user_data));
  return true;
}

static bool ftdi_sio_set_baudrate(cdch_interface_t* p_cdc, uint32_t baudrate, tuh_xfer_cb_t complete_cb, uintptr_t user_data)
{
  // TODO baudrate to baud divisor
  (void) baudrate;
  uint16_t divisor = 0x4138; // FIXME hardcoded to 9600 baud

  p_cdc->user_control_cb = complete_cb;
  TU_ASSERT(ftdi_sio_set_request(p_cdc, FTDI_SIO_SET_BAUD_RATE, divisor, cdch_internal_control_complete, user_data));
  return true;
}

static void process_ftdi_config(tuh_xfer_t* xfer) {
  uintptr_t const state = xfer->user_data;
  uint8_t const itf_num = (uint8_t) tu_le16toh(xfer->setup->wIndex);
  uint8_t const idx = tuh_cdc_itf_get_index(xfer->daddr, itf_num);
  cdch_interface_t * p_cdc = get_itf(idx);
  TU_ASSERT(p_cdc, );

  switch(state) {
    // Note may need to read FTDI eeprom
    case CONFIG_FTDI_RESET:
      TU_ASSERT(ftdi_sio_reset(p_cdc, process_ftdi_config, CONFIG_FTDI_MODEM_CTRL),);
      break;

    case CONFIG_FTDI_MODEM_CTRL:
      #if CFG_TUH_CDC_LINE_CONTROL_ON_ENUM
      TU_ASSERT(
        ftdi_sio_set_modem_ctrl(p_cdc, CFG_TUH_CDC_LINE_CONTROL_ON_ENUM, process_ftdi_config, CONFIG_FTDI_SET_BAUDRATE),);
      break;
      #else
      TU_ATTR_FALLTHROUGH;
      #endif

    case CONFIG_FTDI_SET_BAUDRATE: {
      #ifdef CFG_TUH_CDC_LINE_CODING_ON_ENUM
      cdc_line_coding_t line_coding = CFG_TUH_CDC_LINE_CODING_ON_ENUM;
      TU_ASSERT(ftdi_sio_set_baudrate(p_cdc, line_coding.bit_rate, process_ftdi_config, CONFIG_FTDI_SET_DATA),);
      break;
      #else
      TU_ATTR_FALLTHROUGH;
      #endif
    }

    case CONFIG_FTDI_SET_DATA: {
  #if 0 // TODO set data format
      #ifdef CFG_TUH_CDC_LINE_CODING_ON_ENUM
      cdc_line_coding_t line_coding = CFG_TUH_CDC_LINE_CODING_ON_ENUM;
      TU_ASSERT(ftdi_sio_set_data(p_cdc, process_ftdi_config, CONFIG_FTDI_COMPLETE),);
      break;
      #endif
  #endif

      TU_ATTR_FALLTHROUGH;
    }

    case CONFIG_FTDI_COMPLETE:
      set_config_complete(p_cdc, idx, itf_num);
      break;

    default:
      break;
  }
}

#endif

//--------------------------------------------------------------------+
// CP210x
//--------------------------------------------------------------------+

#if CFG_TUH_CDC_CP210X

static uint16_t const cp210x_pids[] = { TU_CP210X_PID_LIST };
enum {
  CP210X_PID_COUNT = sizeof(cp210x_pids) / sizeof(cp210x_pids[0])
};

enum {
  CONFIG_CP210X_IFC_ENABLE = 0,
  CONFIG_CP210X_SET_BAUDRATE,
  CONFIG_CP210X_SET_LINE_CTL,
  CONFIG_CP210X_SET_DTR_RTS,
  CONFIG_CP210X_COMPLETE
};

static bool cp210x_open(uint8_t daddr, tusb_desc_interface_t const *itf_desc, uint16_t max_len) {
  // CP210x Interface includes 1 vendor interface + 2 bulk endpoints
  TU_VERIFY(itf_desc->bInterfaceSubClass == 0 && itf_desc->bInterfaceProtocol == 0 && itf_desc->bNumEndpoints == 2);
  TU_VERIFY(sizeof(tusb_desc_interface_t) + 2*sizeof(tusb_desc_endpoint_t) <= max_len);

  cdch_interface_t * p_cdc = make_new_itf(daddr, itf_desc);
  TU_VERIFY(p_cdc);

  TU_LOG_CDCH("CP210x opened\r\n");
  p_cdc->serial_protocol = SERIAL_PROTOCOL_CP210X;

  // endpoint pair
  tusb_desc_endpoint_t const * desc_ep = (tusb_desc_endpoint_t const *) tu_desc_next(itf_desc);

  // data endpoints expected to be in pairs
  return open_ep_stream_pair(p_cdc, desc_ep);
}

static bool cp210x_set_request(cdch_interface_t* p_cdc, uint8_t command, uint16_t value, uint8_t* buffer, uint16_t length, tuh_xfer_cb_t complete_cb, uintptr_t user_data) {
  tusb_control_request_t const request = {
    .bmRequestType_bit = {
      .recipient = TUSB_REQ_RCPT_INTERFACE,
      .type      = TUSB_REQ_TYPE_VENDOR,
      .direction = TUSB_DIR_OUT
    },
    .bRequest = command,
    .wValue   = tu_htole16(value),
    .wIndex   = p_cdc->bInterfaceNumber,
    .wLength  = tu_htole16(length)
  };

  // use usbh enum buf since application variable does not live long enough
  uint8_t* enum_buf = NULL;

  if (buffer && length > 0) {
    enum_buf = usbh_get_enum_buf();
    tu_memcpy_s(enum_buf, CFG_TUH_ENUMERATION_BUFSIZE, buffer, length);
  }

  tuh_xfer_t xfer = {
    .daddr       = p_cdc->daddr,
    .ep_addr     = 0,
    .setup       = &request,
    .buffer      = enum_buf,
    .complete_cb = complete_cb,
    .user_data   = user_data
  };

  return tuh_control_xfer(&xfer);
}

static bool cp210x_ifc_enable(cdch_interface_t* p_cdc, uint16_t enabled, tuh_xfer_cb_t complete_cb, uintptr_t user_data) {
  return cp210x_set_request(p_cdc, CP210X_IFC_ENABLE, enabled, NULL, 0, complete_cb, user_data);
}

static bool cp210x_set_baudrate(cdch_interface_t* p_cdc, uint32_t baudrate, tuh_xfer_cb_t complete_cb, uintptr_t user_data) {
  baudrate = tu_htole32(baudrate);
  return cp210x_set_request(p_cdc, CP210X_SET_BAUDRATE, 0, (uint8_t *) &baudrate, 4, complete_cb, user_data);
}

static bool cp210x_set_modem_ctrl(cdch_interface_t* p_cdc, uint16_t line_state, tuh_xfer_cb_t complete_cb, uintptr_t user_data)
{
  p_cdc->user_control_cb = complete_cb;
  return cp210x_set_request(p_cdc, CP210X_SET_MHS, 0x0300 | line_state, NULL, 0, cdch_internal_control_complete, user_data);
}

static void process_cp210x_config(tuh_xfer_t* xfer) {
  uintptr_t const state   = xfer->user_data;
  uint8_t const   itf_num = (uint8_t) tu_le16toh(xfer->setup->wIndex);
  uint8_t const   idx     = tuh_cdc_itf_get_index(xfer->daddr, itf_num);
  cdch_interface_t *p_cdc = get_itf(idx);
  TU_ASSERT(p_cdc,);

  switch (state) {
    case CONFIG_CP210X_IFC_ENABLE:
      TU_ASSERT(cp210x_ifc_enable(p_cdc, 1, process_cp210x_config, CONFIG_CP210X_SET_BAUDRATE),);
      break;

    case CONFIG_CP210X_SET_BAUDRATE: {
      #ifdef CFG_TUH_CDC_LINE_CODING_ON_ENUM
      cdc_line_coding_t line_coding = CFG_TUH_CDC_LINE_CODING_ON_ENUM;
      TU_ASSERT(cp210x_set_baudrate(p_cdc, line_coding.bit_rate, process_cp210x_config, CONFIG_CP210X_SET_LINE_CTL),);
      break;
      #else
      TU_ATTR_FALLTHROUGH;
      #endif
    }

    case CONFIG_CP210X_SET_LINE_CTL: {
      #if defined(CFG_TUH_CDC_LINE_CODING_ON_ENUM) && 0 // skip for now
      cdc_line_coding_t line_coding = CFG_TUH_CDC_LINE_CODING_ON_ENUM;
      break;
      #else
      TU_ATTR_FALLTHROUGH;
      #endif
    }

    case CONFIG_CP210X_SET_DTR_RTS:
      #if CFG_TUH_CDC_LINE_CONTROL_ON_ENUM
      TU_ASSERT(cp210x_set_modem_ctrl(p_cdc, CFG_TUH_CDC_LINE_CONTROL_ON_ENUM, process_cp210x_config, CONFIG_CP210X_COMPLETE),);
      break;
      #else
      TU_ATTR_FALLTHROUGH;
      #endif

    case CONFIG_CP210X_COMPLETE:
      set_config_complete(p_cdc, idx, itf_num);
      break;

    default: break;
  }
}

#endif

//--------------------------------------------------------------------+
// APPLICATION API
//--------------------------------------------------------------------+

uint8_t tuh_cdc_itf_get_index(uint8_t daddr, uint8_t itf_num)
{
  for(uint8_t i=0; i<CFG_TUH_CDC; i++)
  {
    const cdch_interface_t* p_cdc = &cdch_data[i];

    if (p_cdc->daddr == daddr && p_cdc->bInterfaceNumber == itf_num) return i;
  }

  return TUSB_INDEX_INVALID_8;
}

bool tuh_cdc_itf_get_info(uint8_t idx, tuh_itf_info_t* info)
{
  cdch_interface_t* p_cdc = get_itf(idx);
  TU_VERIFY(p_cdc && info);

  info->daddr = p_cdc->daddr;

  // re-construct descriptor
  tusb_desc_interface_t* desc = &info->desc;
  desc->bLength            = sizeof(tusb_desc_interface_t);
  desc->bDescriptorType    = TUSB_DESC_INTERFACE;

  desc->bInterfaceNumber   = p_cdc->bInterfaceNumber;
  desc->bAlternateSetting  = 0;
  desc->bNumEndpoints      = 2u + (p_cdc->ep_notif ? 1u : 0u);
  desc->bInterfaceClass    = TUSB_CLASS_CDC;
  desc->bInterfaceSubClass = p_cdc->bInterfaceSubClass;
  desc->bInterfaceProtocol = p_cdc->bInterfaceProtocol;
  desc->iInterface         = 0; // not used yet

  return true;
}

bool tuh_cdc_mounted(uint8_t idx)
{
  cdch_interface_t* p_cdc = get_itf(idx);
  return p_cdc != NULL;
}

bool tuh_cdc_get_dtr(uint8_t idx)
{
  cdch_interface_t* p_cdc = get_itf(idx);
  TU_VERIFY(p_cdc);

  return (p_cdc->line_state & CDC_CONTROL_LINE_STATE_DTR) ? true : false;
}

bool tuh_cdc_get_rts(uint8_t idx)
{
  cdch_interface_t* p_cdc = get_itf(idx);
  TU_VERIFY(p_cdc);

  return (p_cdc->line_state & CDC_CONTROL_LINE_STATE_RTS) ? true : false;
}

bool tuh_cdc_get_local_line_coding(uint8_t idx, cdc_line_coding_t* line_coding)
{
  cdch_interface_t* p_cdc = get_itf(idx);
  TU_VERIFY(p_cdc);

  *line_coding = p_cdc->line_coding;

  return true;
}

//--------------------------------------------------------------------+
// Write
//--------------------------------------------------------------------+

uint32_t tuh_cdc_write(uint8_t idx, void const* buffer, uint32_t bufsize)
{
  cdch_interface_t* p_cdc = get_itf(idx);
  TU_VERIFY(p_cdc);

  return tu_edpt_stream_write(&p_cdc->stream.tx, buffer, bufsize);
}

uint32_t tuh_cdc_write_flush(uint8_t idx)
{
  cdch_interface_t* p_cdc = get_itf(idx);
  TU_VERIFY(p_cdc);

  return tu_edpt_stream_write_xfer(&p_cdc->stream.tx);
}

bool tuh_cdc_write_clear(uint8_t idx)
{
  cdch_interface_t* p_cdc = get_itf(idx);
  TU_VERIFY(p_cdc);

  return tu_edpt_stream_clear(&p_cdc->stream.tx);
}

uint32_t tuh_cdc_write_available(uint8_t idx)
{
  cdch_interface_t* p_cdc = get_itf(idx);
  TU_VERIFY(p_cdc);

  return tu_edpt_stream_write_available(&p_cdc->stream.tx);
}

//--------------------------------------------------------------------+
// Read
//--------------------------------------------------------------------+

uint32_t tuh_cdc_read (uint8_t idx, void* buffer, uint32_t bufsize)
{
  cdch_interface_t* p_cdc = get_itf(idx);
  TU_VERIFY(p_cdc);

  return tu_edpt_stream_read(&p_cdc->stream.rx, buffer, bufsize);
}

uint32_t tuh_cdc_read_available(uint8_t idx)
{
  cdch_interface_t* p_cdc = get_itf(idx);
  TU_VERIFY(p_cdc);

  return tu_edpt_stream_read_available(&p_cdc->stream.rx);
}

bool tuh_cdc_peek(uint8_t idx, uint8_t* ch)
{
  cdch_interface_t* p_cdc = get_itf(idx);
  TU_VERIFY(p_cdc);

  return tu_edpt_stream_peek(&p_cdc->stream.rx, ch);
}

bool tuh_cdc_read_clear (uint8_t idx)
{
  cdch_interface_t* p_cdc = get_itf(idx);
  TU_VERIFY(p_cdc);

  bool ret = tu_edpt_stream_clear(&p_cdc->stream.rx);
  tu_edpt_stream_read_xfer(&p_cdc->stream.rx);
  return ret;
}

//--------------------------------------------------------------------+
// Control Endpoint API
//--------------------------------------------------------------------+

// internal control complete to update state such as line state, encoding
static void cdch_internal_control_complete(tuh_xfer_t* xfer)
{
  uint8_t const itf_num = (uint8_t) tu_le16toh(xfer->setup->wIndex);
  uint8_t idx = tuh_cdc_itf_get_index(xfer->daddr, itf_num);
  cdch_interface_t* p_cdc = get_itf(idx);
  TU_ASSERT(p_cdc, );

  if (xfer->result == XFER_RESULT_SUCCESS)
  {
    if (p_cdc->serial_protocol == SERIAL_PROTOCOL_ACM) {
      switch (xfer->setup->bRequest) {
        case CDC_REQUEST_SET_CONTROL_LINE_STATE:
          p_cdc->line_state = (uint8_t) tu_le16toh(xfer->setup->wValue);
          break;

        case CDC_REQUEST_SET_LINE_CODING: {
          uint16_t const len = tu_min16(sizeof(cdc_line_coding_t), tu_le16toh(xfer->setup->wLength));
          memcpy(&p_cdc->line_coding, xfer->buffer, len);
        }
          break;

        default: break;
      }
    }
    #if CFG_TUH_CDC_FTDI
    else if (p_cdc->serial_protocol == SERIAL_PROTOCOL_FTDI) {
      switch (xfer->setup->bRequest) {
        case FTDI_SIO_MODEM_CTRL:
          p_cdc->line_state = (uint8_t) (tu_le16toh(xfer->setup->wValue) & 0x00ff);
          break;

        default: break;
      }
    }
    #endif
  }

  xfer->complete_cb = p_cdc->user_control_cb;
  xfer->complete_cb(xfer);
}

static bool acm_set_control_line_state(cdch_interface_t* p_cdc, uint16_t line_state, tuh_xfer_cb_t complete_cb, uintptr_t user_data) {
  tusb_control_request_t const request = {
    .bmRequestType_bit = {
      .recipient = TUSB_REQ_RCPT_INTERFACE,
      .type      = TUSB_REQ_TYPE_CLASS,
      .direction = TUSB_DIR_OUT
    },
    .bRequest = CDC_REQUEST_SET_CONTROL_LINE_STATE,
    .wValue   = tu_htole16(line_state),
    .wIndex   = tu_htole16((uint16_t) p_cdc->bInterfaceNumber),
    .wLength  = 0
  };

  p_cdc->user_control_cb = complete_cb;

  tuh_xfer_t xfer = {
    .daddr       = p_cdc->daddr,
    .ep_addr     = 0,
    .setup       = &request,
    .buffer      = NULL,
    .complete_cb = cdch_internal_control_complete,
    .user_data   = user_data
  };

  TU_ASSERT(tuh_control_xfer(&xfer));
  return true;
}

bool tuh_cdc_set_control_line_state(uint8_t idx, uint16_t line_state, tuh_xfer_cb_t complete_cb, uintptr_t user_data)
{
  cdch_interface_t* p_cdc = get_itf(idx);
  TU_VERIFY(p_cdc && support_line_request(p_cdc));
  TU_LOG_CDCH("CDC Set Control Line State\r\n");

  if(p_cdc->serial_protocol == SERIAL_PROTOCOL_ACM ) {
    return acm_set_control_line_state(p_cdc, line_state, complete_cb, user_data);
  }
  #if CFG_TUH_CDC_FTDI
  else if (p_cdc->serial_protocol == SERIAL_PROTOCOL_FTDI) {
    return ftdi_sio_set_modem_ctrl(p_cdc, line_state, complete_cb, user_data);
  }
  #endif
  else {
    return false;
  }
}

bool acm_set_line_coding(cdch_interface_t* p_cdc, cdc_line_coding_t const* line_coding, tuh_xfer_cb_t complete_cb, uintptr_t user_data) {
  tusb_control_request_t const request = {
    .bmRequestType_bit = {
      .recipient = TUSB_REQ_RCPT_INTERFACE,
      .type      = TUSB_REQ_TYPE_CLASS,
      .direction = TUSB_DIR_OUT
    },
    .bRequest = CDC_REQUEST_SET_LINE_CODING,
    .wValue   = 0,
    .wIndex   = tu_htole16(p_cdc->bInterfaceNumber),
    .wLength  = tu_htole16(sizeof(cdc_line_coding_t))
  };

  // use usbh enum buf to hold line coding since user line_coding variable does not live long enough
  uint8_t* enum_buf = usbh_get_enum_buf();
  memcpy(enum_buf, line_coding, sizeof(cdc_line_coding_t));

  p_cdc->user_control_cb = complete_cb;
  tuh_xfer_t xfer = {
    .daddr       = p_cdc->daddr,
    .ep_addr     = 0,
    .setup       = &request,
    .buffer      = enum_buf,
    .complete_cb = cdch_internal_control_complete,
    .user_data   = user_data
  };

  TU_ASSERT(tuh_control_xfer(&xfer));
  return true;
}

bool tuh_cdc_set_line_coding(uint8_t idx, cdc_line_coding_t const* line_coding, tuh_xfer_cb_t complete_cb, uintptr_t user_data)
{
  cdch_interface_t* p_cdc = get_itf(idx);
  TU_VERIFY(p_cdc && support_line_request(p_cdc));
  TU_LOG_CDCH("CDC Set Line Conding\r\n");

  if (p_cdc->serial_protocol == SERIAL_PROTOCOL_ACM) {
    return acm_set_line_coding(p_cdc, line_coding, complete_cb, user_data);
  }
  #if CFG_TUH_CDC_FTDI
  else if (p_cdc->serial_protocol == SERIAL_PROTOCOL_FTDI) {
    // FTDI need to set baud rate and data bits, parity, stop bits separately
    return ftdi_sio_set_baudrate(p_cdc, line_coding->bit_rate, complete_cb, user_data);
  }
  #endif
  else {
    return false;
  }
}

//--------------------------------------------------------------------+
// CLASS-USBH API
//--------------------------------------------------------------------+

void cdch_init(void)
{
  tu_memclr(cdch_data, sizeof(cdch_data));

  for(size_t i=0; i<CFG_TUH_CDC; i++)
  {
    cdch_interface_t* p_cdc = &cdch_data[i];

    tu_edpt_stream_init(&p_cdc->stream.tx, true, true, false,
                          p_cdc->stream.tx_ff_buf, CFG_TUH_CDC_TX_BUFSIZE,
                          p_cdc->stream.tx_ep_buf, CFG_TUH_CDC_TX_EPSIZE);

    tu_edpt_stream_init(&p_cdc->stream.rx, true, false, false,
                          p_cdc->stream.rx_ff_buf, CFG_TUH_CDC_RX_BUFSIZE,
                          p_cdc->stream.rx_ep_buf, CFG_TUH_CDC_RX_EPSIZE);
  }
}

void cdch_close(uint8_t daddr)
{
  for(uint8_t idx=0; idx<CFG_TUH_CDC; idx++)
  {
    cdch_interface_t* p_cdc = &cdch_data[idx];
    if (p_cdc->daddr == daddr)
    {
      // Invoke application callback
      if (tuh_cdc_umount_cb) tuh_cdc_umount_cb(idx);

      //tu_memclr(p_cdc, sizeof(cdch_interface_t));
      p_cdc->daddr = 0;
      p_cdc->bInterfaceNumber = 0;
      tu_edpt_stream_close(&p_cdc->stream.tx);
      tu_edpt_stream_close(&p_cdc->stream.rx);
    }
  }
}

bool cdch_xfer_cb(uint8_t daddr, uint8_t ep_addr, xfer_result_t event, uint32_t xferred_bytes)
{
  // TODO handle stall response, retry failed transfer ...
  TU_ASSERT(event == XFER_RESULT_SUCCESS);

  uint8_t const idx = get_idx_by_ep_addr(daddr, ep_addr);
  cdch_interface_t * p_cdc = get_itf(idx);
  TU_ASSERT(p_cdc);

  if ( ep_addr == p_cdc->stream.tx.ep_addr )
  {
    // invoke tx complete callback to possibly refill tx fifo
    if (tuh_cdc_tx_complete_cb) tuh_cdc_tx_complete_cb(idx);

    if ( 0 == tu_edpt_stream_write_xfer(&p_cdc->stream.tx) )
    {
      // If there is no data left, a ZLP should be sent if:
      // - xferred_bytes is multiple of EP Packet size and not zero
      tu_edpt_stream_write_zlp_if_needed(&p_cdc->stream.tx, xferred_bytes);
    }
  }
  else if ( ep_addr == p_cdc->stream.rx.ep_addr )
  {
    tu_edpt_stream_read_xfer_complete(&p_cdc->stream.rx, xferred_bytes);

    #if CFG_TUH_CDC_FTDI
    // FTDI reserve 2 bytes for status
    if (p_cdc->serial_protocol == SERIAL_PROTOCOL_FTDI) {
      uint8_t status[2];
      tu_edpt_stream_read(&p_cdc->stream.rx, status, 2);
      (void) status; // TODO handle status
    }
    #endif

    // invoke receive callback
    if (tuh_cdc_rx_cb) tuh_cdc_rx_cb(idx);

    // prepare for next transfer if needed
    tu_edpt_stream_read_xfer(&p_cdc->stream.rx);
  }else if ( ep_addr == p_cdc->ep_notif )
  {
    // TODO handle notification endpoint
  }else
  {
    TU_ASSERT(false);
  }

  return true;
}

//--------------------------------------------------------------------+
// Enumeration
//--------------------------------------------------------------------+
enum
{
  // ACM
  CONFIG_ACM_SET_CONTROL_LINE_STATE = 0,
  CONFIG_ACM_SET_LINE_CODING,
  CONFIG_ACM_COMPLETE,
};

static bool open_ep_stream_pair(cdch_interface_t* p_cdc , tusb_desc_endpoint_t const *desc_ep)
{
  for(size_t i=0; i<2; i++)
  {
    TU_ASSERT(TUSB_DESC_ENDPOINT == desc_ep->bDescriptorType &&
              TUSB_XFER_BULK     == desc_ep->bmAttributes.xfer);

    TU_ASSERT(tuh_edpt_open(p_cdc->daddr, desc_ep));

    if ( tu_edpt_dir(desc_ep->bEndpointAddress) == TUSB_DIR_IN )
    {
      tu_edpt_stream_open(&p_cdc->stream.rx, p_cdc->daddr, desc_ep);
    }else
    {
      tu_edpt_stream_open(&p_cdc->stream.tx, p_cdc->daddr, desc_ep);
    }

    desc_ep = (tusb_desc_endpoint_t const*) tu_desc_next(desc_ep);
  }

  return true;
}

static bool acm_open(uint8_t daddr, tusb_desc_interface_t const *itf_desc, uint16_t max_len)
{
  uint8_t const * p_desc_end = ((uint8_t const*) itf_desc) + max_len;

  cdch_interface_t * p_cdc = make_new_itf(daddr, itf_desc);
  TU_VERIFY(p_cdc);

  p_cdc->serial_protocol = SERIAL_PROTOCOL_ACM;

  //------------- Control Interface -------------//
  uint8_t const * p_desc = tu_desc_next(itf_desc);

  // Communication Functional Descriptors
  while( (p_desc < p_desc_end) && (TUSB_DESC_CS_INTERFACE == tu_desc_type(p_desc)) )
  {
    if ( CDC_FUNC_DESC_ABSTRACT_CONTROL_MANAGEMENT == cdc_functional_desc_typeof(p_desc) )
    {
      // save ACM bmCapabilities
      p_cdc->acm_capability = ((cdc_desc_func_acm_t const *) p_desc)->bmCapabilities;
    }

    p_desc = tu_desc_next(p_desc);
  }

  // Open notification endpoint of control interface if any
  if (itf_desc->bNumEndpoints == 1)
  {
    TU_ASSERT(TUSB_DESC_ENDPOINT == tu_desc_type(p_desc));
    tusb_desc_endpoint_t const * desc_ep = (tusb_desc_endpoint_t const *) p_desc;

    TU_ASSERT( tuh_edpt_open(daddr, desc_ep) );
    p_cdc->ep_notif = desc_ep->bEndpointAddress;

    p_desc = tu_desc_next(p_desc);
  }

  //------------- Data Interface (if any) -------------//
  if ( (TUSB_DESC_INTERFACE == tu_desc_type(p_desc)) &&
       (TUSB_CLASS_CDC_DATA == ((tusb_desc_interface_t const *) p_desc)->bInterfaceClass) )
  {
    // next to endpoint descriptor
    p_desc = tu_desc_next(p_desc);

    // data endpoints expected to be in pairs
    TU_ASSERT(open_ep_stream_pair(p_cdc, (tusb_desc_endpoint_t const *) p_desc));
  }

  return true;
}


bool cdch_open(uint8_t rhport, uint8_t daddr, tusb_desc_interface_t const *itf_desc, uint16_t max_len)
{
  (void) rhport;

  // Only support ACM subclass
  // Note: Protocol 0xFF can be RNDIS device
  if ( TUSB_CLASS_CDC                           == itf_desc->bInterfaceClass &&
       CDC_COMM_SUBCLASS_ABSTRACT_CONTROL_MODEL == itf_desc->bInterfaceSubClass)
  {
    return acm_open(daddr, itf_desc, max_len);
  }
  #if CFG_TUH_CDC_FTDI || CFG_TUH_CDC_CP210X
  else if ( 0xff == itf_desc->bInterfaceClass )
  {
    uint16_t vid, pid;
    TU_VERIFY(tuh_vid_pid_get(daddr, &vid, &pid));

    #if CFG_TUH_CDC_FTDI
    if (TU_FTDI_VID == vid) {
      for (size_t i = 0; i < FTDI_PID_COUNT; i++) {
        if (ftdi_pids[i] == pid) {
          return ftdi_open(daddr, itf_desc, max_len);
        }
      }
    }
    #endif

    #if CFG_TUH_CDC_CP210X
    if (TU_CP210X_VID == vid) {
      for (size_t i = 0; i < CP210X_PID_COUNT; i++) {
        if (cp210x_pids[i] == pid) {
          return cp210x_open(daddr, itf_desc, max_len);
        }
      }
    }
    #endif
  }
  #endif

  return false;
}

static void set_config_complete(cdch_interface_t * p_cdc, uint8_t idx, uint8_t itf_num) {
  if (tuh_cdc_mount_cb) tuh_cdc_mount_cb(idx);

  // Prepare for incoming data
  tu_edpt_stream_read_xfer(&p_cdc->stream.rx);

  // notify usbh that driver enumeration is complete
  usbh_driver_set_config_complete(p_cdc->daddr, itf_num);
}

static void process_acm_config(tuh_xfer_t* xfer)
{
  uintptr_t const state = xfer->user_data;
  uint8_t const itf_num = (uint8_t) tu_le16toh(xfer->setup->wIndex);
  uint8_t const idx = tuh_cdc_itf_get_index(xfer->daddr, itf_num);
  cdch_interface_t * p_cdc = get_itf(idx);
  TU_ASSERT(p_cdc, );

  switch(state)
  {
    case CONFIG_ACM_SET_CONTROL_LINE_STATE:
      #if CFG_TUH_CDC_LINE_CONTROL_ON_ENUM
      if (p_cdc->acm_capability.support_line_request)
      {
        TU_ASSERT(tuh_cdc_set_control_line_state(idx, CFG_TUH_CDC_LINE_CONTROL_ON_ENUM, process_acm_config,
                                                 CONFIG_ACM_SET_LINE_CODING), );
        break;
      }
      #endif
      TU_ATTR_FALLTHROUGH;

    case CONFIG_ACM_SET_LINE_CODING:
      #ifdef CFG_TUH_CDC_LINE_CODING_ON_ENUM
      if (p_cdc->acm_capability.support_line_request)
      {
        cdc_line_coding_t line_coding = CFG_TUH_CDC_LINE_CODING_ON_ENUM;
        TU_ASSERT(tuh_cdc_set_line_coding(idx, &line_coding, process_acm_config, CONFIG_ACM_COMPLETE), );
        break;
      }
      #endif
      TU_ATTR_FALLTHROUGH;

    case CONFIG_ACM_COMPLETE:
      // itf_num+1 to account for data interface as well
      set_config_complete(p_cdc, idx, itf_num+1);
    break;

    default: break;
  }
}

bool cdch_set_config(uint8_t daddr, uint8_t itf_num)
{
  tusb_control_request_t request;
  request.wIndex = tu_htole16((uint16_t) itf_num);

  tuh_xfer_t xfer;
  xfer.daddr  = daddr;
  xfer.result = XFER_RESULT_SUCCESS;
  xfer.setup  = &request;
  xfer.user_data = 0;

  // fake transfer to kick-off process
  uint8_t const idx = tuh_cdc_itf_get_index(daddr, itf_num);
  cdch_interface_t * p_cdc = get_itf(idx);
  TU_ASSERT(p_cdc);

  switch (p_cdc->serial_protocol) {
    case SERIAL_PROTOCOL_ACM:
      process_acm_config(&xfer);
      break;

    #if CFG_TUH_CDC_FTDI
    case SERIAL_PROTOCOL_FTDI:
      process_ftdi_config(&xfer);
      break;
    #endif

    #if CFG_TUH_CDC_CP210X
    case SERIAL_PROTOCOL_CP210X:
      process_cp210x_config(&xfer);
      break;
    #endif

      default: return false;
  }

  return true;
}

#endif
