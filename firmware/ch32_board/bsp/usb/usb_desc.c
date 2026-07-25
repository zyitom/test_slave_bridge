/********************************** (C) COPYRIGHT *******************************
 * File Name          : usb_desc.c
 * Author             : WCH
 * Version            : V1.0.1
 * Date               : 2025/10/24
 * Description        : usb device descriptor,configuration descriptor,
 *                      string descriptors and other descriptors.
*********************************************************************************
* Copyright (c) 2025 Nanjing Qinheng Microelectronics Co., Ltd.
* Attention: This software (modified or not) and binary are used for
* microcontroller manufactured by Nanjing Qinheng Microelectronics.
*******************************************************************************/

#include "ch32h417.h"
#include "usb_desc.h"
#include "ch32h417_usbss_device.h"

/* Device Descriptor */
const uint8_t  MyDevDescr[ ] =
{
    0x12,                                                   // bLength
    0x01,                                                   // bDescriptorType
    0x00, 0x02,                                             // bcdUSB
    0xFF,                                                   // bDeviceClass
    0xFF,                                                   // bDeviceSubClass
    0xFF,                                                   // bDeviceProtocol
    DEF_USBD_UEP0_SIZE,                                     // bMaxPacketSize0
    (uint8_t)DEF_USB_VID, (uint8_t)( DEF_USB_VID >> 8 ),    // idVendor
    (uint8_t)DEF_USB_PID, (uint8_t)( DEF_USB_PID >> 8 ),    // idProduct
    DEF_IC_PRG_VER, 0x00,                                   // bcdDevice
    0x01,                                                   // iManufacturer
    0x02,                                                   // iProduct
    0x03,                                                   // iSerialNumber
    0x01,                                                   // bNumConfigurations
};

/* Configuration Descriptor(high speed)  */
const uint8_t  MyCfgDescr_HS[ ] =
{
    0x09,                                                   // bLength
    0x02,                                                   // bDescriptorType (Configuration)
    0x3C, 0x00,                                             // wTotalLength 60
    0x01,                                                   // bNumInterfaces 1
    0x01,                                                   // bConfigurationValue
    0x00,                                                   // iConfiguration (String Index)
    0x80,                                                   // bmAttributes
    0x32,                                                   // bMaxPower 100mA

    0x09,                                                   // bLength
    0x04,                                                   // bDescriptorType (Interface)
    0x00,                                                   // bInterfaceNumber 0
    0x00,                                                   // bAlternateSetting
    0x06,                                                   // bNumEndpoints 6
    0xFF,                                                   // bInterfaceClass
    0xFF,                                                   // bInterfaceSubClass
    0xFF,                                                   // bInterfaceProtocol
    0x00,                                                   // iInterface (String Index)

    0x07,                                                   // bLength
    0x05,                                                   // bDescriptorType (Endpoint)
    0x01,                                                   // bEndpointAddress (OUT/H2D)
    0x02,                                                   // bmAttributes (Bulk)
    (uint8_t)DEF_USB_EP1_HS_SIZE, (uint8_t)( DEF_USB_EP1_HS_SIZE >> 8 ), // wMaxPacketSize 512
    0x00,                                                   // bInterval 0 (unit depends on device speed)

    0x07,                                                   // bLength
    0x05,                                                   // bDescriptorType (Endpoint)
    0x81,                                                   // bEndpointAddress (IN/D2H)
    0x02,                                                   // bmAttributes (Bulk)
    (uint8_t)DEF_USB_EP2_HS_SIZE, (uint8_t)( DEF_USB_EP2_HS_SIZE >> 8 ),  // wMaxPacketSize 512
    0x00,                                                   // bInterval 0 (unit depends on device speed)

    0x07,                                                   // bLength
    0x05,                                                   // bDescriptorType (Endpoint)
    0x03,                                                   // bEndpointAddress (OUT/H2D)
    0x02,                                                   // bmAttributes (Bulk)
    (uint8_t)DEF_USB_EP3_HS_SIZE, (uint8_t)( DEF_USB_EP3_HS_SIZE >> 8 ),  // wMaxPacketSize 512
    0x00,                                                   // bInterval 0 (unit depends on device speed)

    0x07,                                                   // bLength
    0x05,                                                   // bDescriptorType (Endpoint)
    0x84,                                                   // bEndpointAddress (IN/D2H)
    0x02,                                                   // bmAttributes (Bulk)
    (uint8_t)DEF_USB_EP4_HS_SIZE, (uint8_t)( DEF_USB_EP4_HS_SIZE >> 8 ),  // wMaxPacketSize 512
    0x00,                                                   // bInterval 0 (unit depends on device speed)

    0x07,                                                   // bLength
    0x05,                                                   // bDescriptorType (Endpoint)
    0x05,                                                   // bEndpointAddress (OUT/H2D)
    0x02,                                                   // bmAttributes (Bulk)
    (uint8_t)DEF_USB_EP5_HS_SIZE, (uint8_t)( DEF_USB_EP5_HS_SIZE >> 8 ),  // wMaxPacketSize 512
    0x00,                                                   // bInterval 0 (unit depends on device speed)

    0x07,                                                   // bLength
    0x05,                                                   // bDescriptorType (Endpoint)
    0x86,                                                   // bEndpointAddress (IN/D2H)
    0x02,                                                   // bmAttributes (Bulk)
    (uint8_t)DEF_USB_EP6_HS_SIZE, (uint8_t)( DEF_USB_EP6_HS_SIZE >> 8 ), // wMaxPacketSize 512
    0x00,                                                   // bInterval 0 (unit depends on device speed)
};

/* Configuration Descriptor(full speed) */
const uint8_t  MyCfgDescr_FS[ ] =
{
    0x09,                                                   // bLength
    0x02,                                                   // bDescriptorType (Configuration)
    0x3C, 0x00,                                             // wTotalLength 60
    0x01,                                                   // bNumInterfaces 1
    0x01,                                                   // bConfigurationValue
    0x00,                                                   // iConfiguration (String Index)
    0x80,                                                   // bmAttributes
    0x32,                                                   // bMaxPower 100mA

    0x09,                                                   // bLength
    0x04,                                                   // bDescriptorType (Interface)
    0x00,                                                   // bInterfaceNumber 0
    0x00,                                                   // bAlternateSetting
    0x06,                                                   // bNumEndpoints 6
    0xFF,                                                   // bInterfaceClass
    0xFF,                                                   // bInterfaceSubClass
    0xFF,                                                   // bInterfaceProtocol
    0x00,                                                   // iInterface (String Index)

    0x07,                                                   // bLength
    0x05,                                                   // bDescriptorType (Endpoint)
    0x01,                                                   // bEndpointAddress (OUT/H2D)
    0x02,                                                   // bmAttributes (Bulk)
    (uint8_t)DEF_USB_EP1_FS_SIZE, (uint8_t)( DEF_USB_EP1_FS_SIZE >> 8 ), // wMaxPacketSize 64
    0x00,                                                   // bInterval 0 (unit depends on device speed)

    0x07,                                                   // bLength
    0x05,                                                   // bDescriptorType (Endpoint)
    0x81,                                                   // bEndpointAddress (IN/D2H)
    0x02,                                                   // bmAttributes (Bulk)
    (uint8_t)DEF_USB_EP2_FS_SIZE, (uint8_t)( DEF_USB_EP2_FS_SIZE >> 8 ),  // wMaxPacketSize 64
    0x00,                                                   // bInterval 0 (unit depends on device speed)

    0x07,                                                   // bLength
    0x05,                                                   // bDescriptorType (Endpoint)
    0x03,                                                   // bEndpointAddress (OUT/H2D)
    0x02,                                                   // bmAttributes (Bulk)
    (uint8_t)DEF_USB_EP3_FS_SIZE, (uint8_t)( DEF_USB_EP3_FS_SIZE >> 8 ),  // wMaxPacketSize 64
    0x00,                                                   // bInterval 0 (unit depends on device speed)

    0x07,                                                   // bLength
    0x05,                                                   // bDescriptorType (Endpoint)
    0x84,                                                   // bEndpointAddress (IN/D2H)
    0x02,                                                   // bmAttributes (Bulk)
    (uint8_t)DEF_USB_EP4_FS_SIZE, (uint8_t)( DEF_USB_EP4_FS_SIZE >> 8 ),  // wMaxPacketSize 64
    0x00,                                                   // bInterval 0 (unit depends on device speed)

    0x07,                                                   // bLength
    0x05,                                                   // bDescriptorType (Endpoint)
    0x05,                                                   // bEndpointAddress (OUT/H2D)
    0x02,                                                   // bmAttributes (Bulk)
    (uint8_t)DEF_USB_EP5_FS_SIZE, (uint8_t)( DEF_USB_EP5_FS_SIZE >> 8 ),  // wMaxPacketSize 64
    0x00,                                                   // bInterval 0 (unit depends on device speed)

    0x07,                                                   // bLength
    0x05,                                                   // bDescriptorType (Endpoint)
    0x86,                                                   // bEndpointAddress (IN/D2H)
    0x02,                                                   // bmAttributes (Bulk)
    (uint8_t)DEF_USB_EP6_FS_SIZE, (uint8_t)( DEF_USB_EP6_FS_SIZE >> 8 ), // wMaxPacketSize 64
    0x00,                                                   // bInterval 0 (unit depends on device speed)
};

/* Device Qualified Descriptor */
const uint8_t MyQuaDesc[ ] =
{
    0x0A, 0x06, 0x00, 0x02, 0xFF, 0xFF, 0xFF, 0x40, 0x01, 0x00,
};

/* Device BOS Descriptor */
const uint8_t MyBOSDesc[ ] =
{
    /* Binary Object Store (BOS) Descriptor */
    0x05, 0x0F, 0x2A, 0x00, 0x01,
    /* USB 2.0 Extension Descriptor */
    0x07, 0x10, 0x02, 0x02, 0x00, 0x00, 0x00,
    /* SuperSpeed USB Device Capability Descriptor */
    0x0A, 0x10, 0x03, 0x00, 0x0E, 0x00, 0x10, 0x0A, 0x20, 0x00,
    /* SuperSpeedPlus USB Device Capability */
    0x14, 0x10, 0x0A, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x11,
    0x00, 0x00, 0x30, 0x40, 0x0A, 0x00, 0xB0, 0x40, 0x0A, 0x00,
    
};

/* USB Full-Speed Mode, Other speed configuration Descriptor */
uint8_t TAB_USB_FS_OSC_DESC[ sizeof(MyCfgDescr_HS) ] =
{
    /* Other parts are copied through the program */
    0x09, 0x07,
};

/* USB High-Speed Mode, Other speed configuration Descriptor */
uint8_t TAB_USB_HS_OSC_DESC[ sizeof(MyCfgDescr_FS) ] =
{
    /* Other parts are copied through the program */
    0x09, 0x07,
};

/*Superspeed device descriptor*/
const uint8_t SS_DeviceDescriptor[ ] =
{
    0x12,                                                   // bLength
    0x01,                                                   // bDescriptorType
    0x00, 0x03,                                             // bcdUSB
    0xff,                                                   // bDeviceClass
    0x80,                                                   // bDeviceSubClass
    0x55,                                                   // bDeviceProtocol
    0x09,                                                   // bMaxPacketSize0
    (uint8_t)DEF_USB_VID, (uint8_t)(DEF_USB_VID >> 8),      // idVendor
    (uint8_t)DEF_USB30_PID, (uint8_t)(DEF_USB30_PID >> 8),  // idProduct
    DEF_IC_PRG_VER, 0x00,                                   // bcdDevice
    0x01,                                                   // iManufacturer
    0x02,                                                   // iProduct
    0x03,                                                   // iSerialNumber
    0x01,                                                   // bNumConfigurations
};

const uint8_t SS_ConfigDescriptor[ ] =
{
    /* LIBRMCS LOCAL PATCH: one vendor interface with a single bulk pair (EP1
     * IN/OUT), plus a DFU run-time interface. The CH372 demo advertised three
     * bulk pairs; EP2/EP3 are gone from the endpoint enable, the endpoint init
     * and the ISR (see ch32h417_usbss_it.c). Interface 1 is what
     * app/src/usb/dfu_runtime.cpp answers class requests for -- without this
     * descriptor the host could never address it. Layout mirrors mc02's
     * TUD_DFU_RT_DESCRIPTOR so both boards look the same to the host tooling.
     * wTotalLength = 9 + 9 + 2*(7+6) + 9 + 9 = 62. */

    /* Configuration Descriptor */
    0x09,                                                   // bLength
    0x02,                                                   // bDescriptorType
    0x3E, 0x00,                                             // wTotalLength
    0x02,                                                   // bNumInterfaces
    0x01,                                                   // bConfigurationValue
    0x00,                                                   // iConfiguration
    0x80,                                                   // bmAttributes: Bus Powered; Remote Wakeup
    0x64,                                                   // MaxPower: 100mA

    /* Interface Descriptor (librmcs vendor) */
    0x09,                                                   // bLength
    0x04,                                                   // bDescriptorType
    0x00,                                                   // bInterfaceNumber
    0x00,                                                   // bAlternateSetting
    0x02,                                                   // bNumEndpoints
    0xff,                                                   // bInterfaceClass
    0xff,                                                   // bInterfaceSubClass
    0xff,                                                   // bInterfaceProtocol
    0x00,                                                   // iInterface

    /* Endpoint Descriptor */
    0x07,                                                   // bLength
    0x05,                                                   // bDescriptorType
    0x81,                                                   // bEndpointAddress: IN Endpoint 1
    0x02,                                                   // bmAttributes
    0x00, 0x04,                                             // wMaxPacketSize
    0x00,                                                   // bInterval

    /* Endpoint Compansion Descriptor */
    0x06,                                                   // bLength
    0x30,                                                   // bDescriptorType
    DEF_ENDP1_IN_BURST_LEVEL - 1,                           // bMaxBurst
    0x00,                                                   // bmAttributes
    0x00, 0x00,                                             // wBytesPerInterval 

    /* Endpoint Descriptor */
    0x07,                                                   // bLength
    0x05,                                                   // bDescriptorType
    0x01,                                                   // bEndpointAddress: OUT Endpoint 1
    0x02,                                                   // bmAttributes
    0x00, 0x04,                                             // wMaxPacketSize
    0x00,                                                   // bInterval

    /* Endpoint Compansion Descriptor */
    0x06,                                                   // bLength
    0x30,                                                   // bDescriptorType
    DEF_ENDP1_OUT_BURST_LEVEL - 1,                          // bMaxBurst
    0x00,                                                   // bmAttributes
    0x00, 0x00,                                             // wBytesPerInterval

    /* Interface Descriptor (DFU run-time) */
    0x09,                                                   // bLength
    0x04,                                                   // bDescriptorType
    0x01,                                                   // bInterfaceNumber
    0x00,                                                   // bAlternateSetting
    0x00,                                                   // bNumEndpoints
    0xfe,                                                   // bInterfaceClass: Application Specific
    0x01,                                                   // bInterfaceSubClass: DFU
    0x01,                                                   // bInterfaceProtocol: run-time
    0x00,                                                   // iInterface: none -- this stack only
                                                            //   serves string indices 0..3

    /* DFU Functional Descriptor */
    0x09,                                                   // bLength
    0x21,                                                   // bDescriptorType: DFU FUNCTIONAL
    0x09,                                                   // bmAttributes: CAN_DOWNLOAD | WILL_DETACH
    0xe8, 0x03,                                             // wDetachTimeOut: 1000ms
    0x00, 0x04,                                             // wTransferSize: 1024
    0x01, 0x01,                                             // bcdDFUVersion: 1.01
};

/*String Descriptor Lang ID*/
const uint8_t MyLangDescr[ ] =
{
    0x04,
    0x03,
    0x09,
    0x04
};

/* LIBRMCS LOCAL PATCH: the manufacturer / product / serial-number string
 * descriptors are the librmcs device identity, not WCH demo data, so they are
 * defined in app/src/usb/descriptors.cpp instead of here. The product string in
 * particular has to carry LIBRMCS_PROJECT_VERSION_STRING, which the host SDK
 * checks. The declarations stay in usb_desc.h. */

const uint8_t OSStringDescriptor[ ] =
{
    0x12, 
    0x03,
    'M',
    0x00,
    'S',
    0x00,
    'F',
    0x00,
    'T',
    0x00,
    '1',
    0x00,
    '0',
    0x00,
    '0',
    0x00,
    0x01,
    0x00
};

const uint8_t MyBOSDesc_SS[ ] =
{
    0x05,
    0x0f, 
    0x16,
    0x00,
    0x02,
   
    0x07,
    0x10,
    0x02, 
    0x06,
    0x00,
    0x00,
    0x00,
   
    0x0a,
    0x10,
    0x03,
    0x00,
    0x0e, 
    0x00,
    0x01, 
    0x0a, 
    0xff, 
    0x07
};

const uint8_t MSOS20DescriptorSet[ ] =
{
    // Microsoft OS 2.0 Descriptor Set Header
    0x0A, 0x00,            
    0x00, 0x00,          
    0x00, 0x00, 0x03, 0x06, 
    0x48, 0x00,           
                             
    0x3E, 0x00,          
    0x04, 0x00,          
    0x04, 0x00,          
    0x30, 0x00,            
    0x53, 0x00, 0x65, 0x00, 
    0x6C, 0x00, 0x65, 0x00,
    0x63, 0x00, 0x74, 0x00,
    0x69, 0x00, 0x76, 0x00,
    0x65, 0x00, 0x53, 0x00,
    0x75, 0x00, 0x73, 0x00,
    0x70, 0x00, 0x65, 0x00,
    0x6E, 0x00, 0x64, 0x00,
    0x45, 0x00, 0x6E, 0x00,
    0x61, 0x00, 0x62, 0x00,
    0x6C, 0x00, 0x65, 0x00,
    0x64, 0x00, 0x00, 0x00,
    0x04, 0x00,            
    0x01, 0x00, 0x00, 0x00
};

const uint8_t PropertyHeader[ ] =
{
    0x8e, 0x00, 0x00, 0x00, 0x00, 01, 05, 00, 01, 00,
    0x84, 0x00, 0x00, 0x00,
    0x01, 0x00, 0x00, 0x00,
    0x28, 0x00,
    0x44, 0x00, 0x65, 0x00, 0x76, 0x00, 0x69, 0x00, 0x63, 0x00, 0x65, 0x00, 0x49, 0x00, 0x6e,
    0x00, 0x74, 0x00, 0x65, 0x00, 0x72, 0x00, 0x66, 0x00, 0x61, 0x00, 0x63, 0x00, 0x65, 0x00, 0x47, 0x00, 0x55, 0x00, 0x49, 0x00, 0x44, 0x00, 0x00, 0x00,

    0x4e, 0x00, 0x00, 0x00,
    0x7b, 0x00, 0x31, 0x00, 0x32, 0x00, 0x33, 0x00, 0x34, 0x00, 0x35, 0x00, 0x36, 0x00, 0x37, 0x00, 0x38, 0x00,
    0x2d, 0x00, 0x31, 0x00, 0x32, 0x00, 0x33, 0x00, 0x34, 0x00,
    0x2d, 0x00, 0x31, 0x00, 0x32, 0x00, 0x33, 0x00, 0x34, 0x00,
    0x2d, 0x00, 0x31, 0x00, 0x32, 0x00, 0x33, 0x00, 0x34, 0x00, 0x2d, 0x00, 0x31, 0x00, 0x32, 0x00, 0x33, 0x00,
    0x34, 0x00, 0x35, 0x00, 0x36, 0x00, 0x37, 0x00, 0x38, 0x00, 0x39, 0x00, 0x41, 0x00, 0x42, 0x00, 0x43, 0x00,
    0x7d, 0x00, 0x00, 0x00
};

const uint8_t CompactId[ ] =
{
    0x28, 0x00, 0x00, 0x00, 0x00, 0x01, 0x04, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01,
    0x57, 0x49, 0x4e, 0x55, 0x53, 0x42, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};

uint8_t GetStatus[ ] =
{
    0x01, 0x00
};


