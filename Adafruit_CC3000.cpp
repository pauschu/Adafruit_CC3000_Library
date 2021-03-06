/**************************************************************************/
/*!
  @file     Adafruit_CC3000.cpp
  @author   KTOWN (Kevin Townsend for Adafruit Industries)
	@license  BSD (see license.txt)

	This is a library for the Adafruit CC3000 WiFi breakout board
	This library works with the Adafruit CC3000 breakout
	----> https://www.adafruit.com/products/1469

	Check out the links above for our tutorials and wiring diagrams
	These chips use SPI to communicate.

	Adafruit invests time and resources providing this open source code,
	please support Adafruit and open-source hardware by purchasing
	products from Adafruit!

	@section  HISTORY

	v1.0    - Initial release
*/
/**************************************************************************/
#include <avr/wdt.h>
#include "Adafruit_CC3000.h"
#include "ccspi.h"

#include "utility/cc3000_common.h"
#include "utility/evnt_handler.h"
#include "utility/hci.h"
#include "utility/netapp.h"
#include "utility/nvmem.h"
#include "utility/security.h"
#include "utility/socket.h"
#include "utility/wlan.h"
#include "utility/debug.h"
#include "utility/sntp.h"

uint8_t g_csPin, g_irqPin, g_vbatPin, g_IRQnum, g_SPIspeed;

static const uint8_t dreqinttable[] = {
#if defined(__AVR_ATmega168__) || defined(__AVR_ATmega328P__) || defined (__AVR_ATmega328__) || defined(__AVR_ATmega8__) 
  2, 0,
  3, 1,
#elif defined(__AVR_ATmega1281__) || defined(__AVR_ATmega2561__) || defined(__AVR_ATmega2560__) || defined(__AVR_ATmega1280__) 
  2, 0,
  3, 1,
  21, 2, 
  20, 3,
  19, 4,
  18, 5,
#elif  defined(__AVR_ATmega32U4__) && defined(CORE_TEENSY)
  5, 0,
  6, 1,
  7, 2,
  8, 3,
#elif  defined(__AVR_AT90USB1286__) && defined(CORE_TEENSY)
  0, 0,
  1, 1,
  2, 2,
  3, 3,
  36, 4,
  37, 5,
  18, 6,
  19, 7,
#elif  defined(__arm__) && defined(CORE_TEENSY)
  0, 0, 1, 1, 2, 2, 3, 3, 4, 4,
  5, 5, 6, 6, 7, 7, 8, 8, 9, 9,
  10, 10, 11, 11, 12, 12, 13, 13, 14, 14,
  15, 15, 16, 16, 17, 17, 18, 18, 19, 19,
  20, 20, 21, 21, 22, 22, 23, 23, 24, 24,
  25, 25, 26, 26, 27, 27, 28, 28, 29, 29,
  30, 30, 31, 31, 32, 32, 33, 33,
#elif  defined(__AVR_ATmega32U4__) 
  7, 4,
  3, 0,
  2, 1,
  0, 2,
  1, 3,
#elif defined(__arm__) && defined(__SAM3X8E__) // Arduino Due  
  0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 
  5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 
  10, 10, 11, 11, 12, 12, 13, 13, 14, 14, 
  15, 15, 16, 16, 17, 17, 18, 18, 19, 19, 
  20, 20, 21, 21, 22, 22, 23, 23, 24, 24, 
  25, 25, 26, 26, 27, 27, 28, 28, 29, 29, 
  30, 30, 31, 31, 32, 32, 33, 33, 34, 34, 
  35, 35, 36, 36, 37, 37, 38, 38, 39, 39, 
  40, 40, 41, 41, 42, 42, 43, 43, 44, 44, 
  45, 45, 46, 46, 47, 47, 48, 48, 49, 49, 
  50, 50, 51, 51, 52, 52, 53, 53, 54, 54, 
  55, 55, 56, 56, 57, 57, 58, 58, 59, 59, 
  60, 60, 61, 61, 62, 62, 63, 63, 64, 64, 
  65, 65, 66, 66, 67, 67, 68, 68, 69, 69,
  70, 70, 71, 71,
#endif
};

/***********************/

uint8_t pingReportnum;
netapp_pingreport_args_t pingReport;

#define CC3000_SUCCESS                        (0)

#ifdef CC3000_TINY_DRIVER
#define CHECK_SUCCESS(func,Notify,errorCode)  {if ((func) != CC3000_SUCCESS) { Serial.println(F(Notify)); return errorCode;}}
#else
#define CHECK_SUCCESS(func,Notify,errorCode)  {if ((func) != CC3000_SUCCESS) { if (CC3KPrinter != 0) CC3KPrinter->println(F(Notify)); return errorCode;}}
#endif

#define MAXSSID					  (32)
#define MAXLENGTHKEY 			(32)  /* Cleared for 32 bytes by TI engineering 29/08/13 */

#define MAX_SOCKETS 32  // can change this
boolean closed_sockets[MAX_SOCKETS] = {false, false, false, false};

/* *********************************************************************** */
/*                                                                         */
/* PRIVATE FIELDS (SmartConfig)                                            */
/*                                                                         */
/* *********************************************************************** */
#ifndef CC3000_TINY_DRIVER
volatile unsigned long ulSmartConfigFinished;
volatile unsigned char ucStopSmartConfig;
volatile long ulSocket;

char _deviceName[] = "CC3000";
char _cc3000_prefix[] = { 'T', 'T', 'T' };
const unsigned char _smartConfigKey[] = { 0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37,
                                          0x38, 0x39, 0x30, 0x31, 0x32, 0x33, 0x34, 0x35 };
                                          // AES key for smart config = "0123456789012345"
#endif

volatile unsigned long ulCC3000Connected,
                       ulCC3000DHCP,
                       ulCC3000DHCP_configured,
                       OkToDoShutDown;

Print* CC3KPrinter; // user specified output stream for general messages and debug

/* *********************************************************************** */
/*                                                                         */
/* PRIVATE FUNCTIONS                                                       */
/*                                                                         */
/* *********************************************************************** */

/**************************************************************************/
/*!
    @brief    Scans for SSID/APs in the CC3000's range

    @note     This command isn't available when the CC3000 is configured
              in 'CC3000_TINY_DRIVER' mode

    @returns  False if an error occured!
*/
/**************************************************************************/
#if ! defined(CC3000_TINY_DRIVER) || defined(CC3000_SECURE)
bool Adafruit_CC3000::scanSSIDs(uint32_t time)
{
  const unsigned long intervalTime[16] = { 2000, 2000, 2000, 2000,  2000,
    2000, 2000, 2000, 2000, 2000, 2000, 2000, 2000, 2000, 2000, 2000 };

  if (!_initialised)
  {
    return false;
  }

#ifndef CC3000_TINY_DRIVER
  // We can abort a scan with a time of 0
  if (time)
  {
    if (CC3KPrinter != 0) {
      CC3KPrinter->println(F(CC3000_MSG_SCAN_STARTED));
    }
  }
#endif

  CHECK_SUCCESS(
      wlan_ioctl_set_scan_params(time, 20, 100, 5, 0x7FF, -120, 0, 300,
          (unsigned long * ) &intervalTime),
          CC3000_MSG_FAIL_SSID_PARAMS, false);

  return true;
}
#endif

/* *********************************************************************** */
/*                                                                         */
/* CONSTRUCTORS                                                            */
/*                                                                         */
/* *********************************************************************** */

/**************************************************************************/
/*!
    @brief  Instantiates a new CC3000 class
*/
/**************************************************************************/
Adafruit_CC3000::Adafruit_CC3000(uint8_t csPin, uint8_t irqPin, uint8_t vbatPin, uint8_t SPIspeed)
{
  _initialised = false;
  g_csPin = csPin;
  g_irqPin = irqPin;
  g_vbatPin = vbatPin;
  g_IRQnum = 0xFF;
  g_SPIspeed = SPIspeed;

  ulCC3000DHCP          = 0;
  ulCC3000Connected     = 0;
#ifndef CC3000_TINY_DRIVER
  ulSocket              = 0;
  ulSmartConfigFinished = 0;
#endif

#ifndef CC3000_TINY_DRIVER
  #if defined(UDR0) || defined(UDR1) || defined(CORE_TEENSY) || ( defined (__arm__) && defined (__SAM3X8E__) )
  CC3KPrinter = &Serial;
  #else
  CC3KPrinter = 0;
  // no default serial port found
  #endif
#endif
}

/* *********************************************************************** */
/*                                                                         */
/* PUBLIC FUNCTIONS                                                        */
/*                                                                         */
/* *********************************************************************** */

/**************************************************************************/
/*!
    @brief  Setups the HW
    
    @args[in] patchReq  
              Set this to true if we are starting a firmware patch,
              otherwise false for normal operation
    @args[in] useSmartConfig
              Set this to true if you want to use the connection details
              that were stored on the device from the SmartConfig process,
              otherwise false to erase existing profiles and start a
              clean connection
*/
/**************************************************************************/
bool Adafruit_CC3000::begin(uint8_t patchReq, bool useSmartConfigData)
{
  if (_initialised) return true;

  #ifndef CORE_ADAX
  // determine irq #
  for (uint8_t i=0; i<sizeof(dreqinttable); i+=2) {
    if (g_irqPin == dreqinttable[i]) {
      g_IRQnum = dreqinttable[i+1];
    }
  }
#ifndef CC3000_TINY_DRIVER
  if (g_IRQnum == 0xFF) {
    if (CC3KPrinter != 0) {
      CC3KPrinter->println(F(CC3000_MSG_IRQ_NOT_INT_PIN));
    }
    return false;
  }
#endif
#else
  g_IRQnum = g_irqPin;
  // (almost) every single pin on Xmega supports interrupt
  #endif

  init_spi();
  
  DEBUGPRINT_F("init\n\r");
  WDT_RESET();
  wlan_init(CC3000_UsynchCallback,
#ifndef CC3000_NO_PATCH
            sendWLFWPatch, sendDriverPatch, sendBootLoaderPatch,
#endif
            ReadWlanInterruptPin,
            WlanInterruptEnable,
            WlanInterruptDisable,
            WriteWlanPin);
  DEBUGPRINT_F("start\n\r");

  WDT_RESET();
  wlan_start(patchReq);
  
  DEBUGPRINT_F("ioctl\n\r");
#ifndef CC3000_TINY_DRIVER
  // Check if we should erase previous stored connection details
  // (most likely written with data from the SmartConfig app)
  if (!useSmartConfigData)
  {
#endif
    // Manual connection only (no auto, profiles, etc.)
    wlan_ioctl_set_connection_policy(0, 0, 0);
    // Delete previous profiles from memory
    wlan_ioctl_del_profile(255);
#ifndef CC3000_TINY_DRIVER
  }
  else
  {
    // Auto Connect - the C3000 device tries to connect to any AP it detects during scanning:
    // wlan_ioctl_set_connection_policy(1, 0, 0)
    
    // Fast Connect - the CC3000 device tries to reconnect to the last AP connected to:
    // wlan_ioctl_set_connection_policy(0, 1, 0)

    // Use Profiles - the CC3000 device tries to connect to an AP from profiles:
    wlan_ioctl_set_connection_policy(0, 0, 1);
  }

#endif
  WDT_RESET();
  CHECK_SUCCESS(
    wlan_set_event_mask(HCI_EVNT_WLAN_UNSOL_INIT        |
                        //HCI_EVNT_WLAN_ASYNC_PING_REPORT |// we want ping reports
                        //HCI_EVNT_BSD_TCP_CLOSE_WAIT |
                        //HCI_EVNT_WLAN_TX_COMPLETE |
                        HCI_EVNT_WLAN_KEEPALIVE),
                        CC3000_MSG_FAIL_SET_EVNT_MASK_2, false);

  _initialised = true;

#ifndef CC3000_TINY_DRIVER
  // Wait for re-connection is we're using SmartConfig data
  if (useSmartConfigData)
  {
    // Wait for a connection
    uint32_t timeout = 0;
    while(!ulCC3000Connected)
    {
      WDT_RESET();
      cc3k_int_poll();
      if(timeout > WLAN_CONNECT_TIMEOUT)
      {
        if (CC3KPrinter != 0) {
          CC3KPrinter->println(F(CC3000_MSG_TIMEOUT_SMART_CFG));
        }
        return false;
      }
      timeout += 10;
      delay(10);
    }

    WDT_RESET();
    delay(1000);
    if (ulCC3000DHCP)
    {
      mdnsAdvertiser(1, (char *) _deviceName, strlen(_deviceName));
    }
  }
#endif
    
  return true;
}

/**************************************************************************/
/*!
    @brief  Prints a hexadecimal value in plain characters

    @param  data      Pointer to the byte data
    @param  numBytes  Data length in bytes
*/
/**************************************************************************/
#ifndef CC3000_TINY_DRIVER
void Adafruit_CC3000::printHex(const byte * data, const uint32_t numBytes)
{
  if (CC3KPrinter == 0) return;

  uint32_t szPos;
  for (szPos=0; szPos < numBytes; szPos++)
  {
    CC3KPrinter->print(F("0x"));
    // Append leading 0 for small values
    if (data[szPos] <= 0xF)
      CC3KPrinter->print(F("0"));
    CC3KPrinter->print(data[szPos], HEX);
    if ((numBytes > 1) && (szPos != numBytes - 1))
    {
      CC3KPrinter->print(' ');
    }
  }
  CC3KPrinter->println();
}
#endif

/**************************************************************************/
/*!
    @brief  Prints a hexadecimal value in plain characters, along with
            the char equivalents in the following format

            00 00 00 00 00 00  ......

    @param  data      Pointer to the byte data
    @param  numBytes  Data length in bytes
*/
/**************************************************************************/
#ifndef CC3000_TINY_DRIVER
void Adafruit_CC3000::printHexChar(const byte * data, const uint32_t numBytes)
{
  if (CC3KPrinter == 0) return;

  uint32_t szPos;
  for (szPos=0; szPos < numBytes; szPos++)
  {
    // Append leading 0 for small values
    if (data[szPos] <= 0xF)
      CC3KPrinter->print('0');
    CC3KPrinter->print(data[szPos], HEX);
    if ((numBytes > 1) && (szPos != numBytes - 1))
    {
      CC3KPrinter->print(' ');
    }
  }
  CC3KPrinter->print("  ");
  for (szPos=0; szPos < numBytes; szPos++)
  {
    if (data[szPos] <= 0x1F)
      CC3KPrinter->print('.');
    else
      CC3KPrinter->print(data[szPos]);
  }
  CC3KPrinter->println();
}
#endif

/**************************************************************************/
/*!
    @brief  Helper function to display an IP address with dots
*/
/**************************************************************************/

#ifndef CC3000_TINY_DRIVER
void Adafruit_CC3000::printIPdots(uint32_t ip) {
  if (CC3KPrinter == 0) return;
  CC3KPrinter->print((uint8_t)(ip));
  CC3KPrinter->print('.');
  CC3KPrinter->print((uint8_t)(ip >> 8));
  CC3KPrinter->print('.');
  CC3KPrinter->print((uint8_t)(ip >> 16));
  CC3KPrinter->print('.');
  CC3KPrinter->print((uint8_t)(ip >> 24));  
}
#endif

/**************************************************************************/
/*!
    @brief  Helper function to display an IP address with dots, printing
            the bytes in reverse order
*/
/**************************************************************************/
void Adafruit_CC3000::printIPdotsRev(uint32_t ip) {
  if (CC3KPrinter == 0) return;
  CC3KPrinter->print((uint8_t)(ip >> 24));
  CC3KPrinter->print('.');
  CC3KPrinter->print((uint8_t)(ip >> 16));
  CC3KPrinter->print('.');
  CC3KPrinter->print((uint8_t)(ip >> 8));
  CC3KPrinter->print('.');
  CC3KPrinter->print((uint8_t)(ip));  
}

/**************************************************************************/
/*!
    @brief  Helper function to convert four bytes to a U32 IP value
*/
/**************************************************************************/
#ifndef CC3000_TINY_DRIVER
uint32_t Adafruit_CC3000::IP2U32(uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
  uint32_t ip = a;
  ip <<= 8;
  ip |= b;
  ip <<= 8;
  ip |= c;
  ip <<= 8;
  ip |= d;

  return ip;
}
#endif

/**************************************************************************/
/*!
    @brief   Reboot CC3000 (stop then start)
*/
/**************************************************************************/
#ifndef CC3000_TINY_DRIVER
void Adafruit_CC3000::reboot(uint8_t patch)
{
  if (!_initialised)
  {
    return;
  }

  WDT_RESET();
  wlan_stop();
  WDT_RESET();
  delay(5000);
  WDT_RESET();
  wlan_start(patch);
  WDT_RESET();
}
#endif

/**************************************************************************/
/*!
    @brief   Stop CC3000
*/
/**************************************************************************/
void Adafruit_CC3000::stop(void)
{
  if (!_initialised)
  {
    return;
  }

  WDT_RESET();
  wlan_stop();
  WDT_RESET();
}

/**************************************************************************/
/*!
    @brief  Disconnects from the network

    @returns  False if an error occured!
*/
/**************************************************************************/
bool Adafruit_CC3000::disconnect(void)
{
  if (!_initialised)
  {
    return false;
  }

  long retVal = wlan_disconnect();

  return retVal != 0 ? false : true;
}

/**************************************************************************/
/*!
    @brief   Deletes all profiles stored in the CC3000

    @returns  False if an error occured!
*/
/**************************************************************************/
#ifndef CC3000_TINY_DRIVER
bool Adafruit_CC3000::deleteProfiles(void)
{
  if (!_initialised)
  {
    return false;
  }

  CHECK_SUCCESS(wlan_ioctl_set_connection_policy(0, 0, 0),
               CC3000_MSG_FAIL_PROFILES_CONN, false);
  CHECK_SUCCESS(wlan_ioctl_del_profile(255),
               CC3000_MSG_FAIL_DEL_PROFILES_5, false);

  return true;
}
#endif

/**************************************************************************/
/*!
    @brief    Reads the MAC address

    @param    address  Buffer to hold the 6 byte Mac Address

    @returns  False if an error occured!
*/
/**************************************************************************/
#ifndef CC3000_TINY_DRIVER
bool Adafruit_CC3000::getMacAddress(uint8_t address[6])
{
  if (!_initialised)
  {
    return false;
  }

  CHECK_SUCCESS(nvmem_read(NVMEM_MAC_FILEID, 6, 0, address),
                CC3000_MSG_FAIL_READ_MAC_ADDR, false);

  return true;
}
#endif

/**************************************************************************/
/*!
    @brief    Sets a new MAC address

    @param    address   Buffer pointing to the 6 byte Mac Address

    @returns  False if an error occured!
*/
/**************************************************************************/
#ifndef CC3000_TINY_DRIVER
bool Adafruit_CC3000::setMacAddress(uint8_t address[6])
{
  if (!_initialised)
  {
    return false;
  }

  if (address[0] == 0)
  {
    return false;
  }

  CHECK_SUCCESS(netapp_config_mac_adrress(address),
                CC3000_MSG_FAIL_SET_MAC_ADDR, false);

  wlan_stop();
  delay(200);
  wlan_start(0);

  return true;
}
#endif

/**************************************************************************/
/*!
    @brief   Reads the current IP address

    @returns  False if an error occured!
*/
/**************************************************************************/
bool Adafruit_CC3000::getIPAddress(uint32_t *retip, uint32_t *netmask, uint32_t *gateway, uint32_t *dhcpserv, uint32_t *dnsserv)
{
  if (!_initialised) return false;
  if (!ulCC3000Connected) return false;
  if (!ulCC3000DHCP) return false;

  tNetappIpconfigRetArgs ipconfig;
  netapp_ipconfig(&ipconfig);

  /* If byte 1 is 0 we don't have a valid address */
  if (ipconfig.aucIP[3] == 0) return false;

  memcpy(retip, ipconfig.aucIP, 4);
#ifndef CC3000_TINY_DRIVER
  memcpy(netmask, ipconfig.aucSubnetMask, 4);
  memcpy(gateway, ipconfig.aucDefaultGateway, 4);
  memcpy(dhcpserv, ipconfig.aucDHCPServer, 4);
  memcpy(dnsserv, ipconfig.aucDNSServer, 4);
#endif

  return true;
}

/**************************************************************************/
/*!
    @brief    Gets the two byte ID for the firmware patch version

    @note     This command isn't available when the CC3000 is configured
              in 'CC3000_TINY_DRIVER' mode

    @returns  False if an error occured!
*/
/**************************************************************************/
#ifndef CC3000_TINY_DRIVER
bool Adafruit_CC3000::getFirmwareVersion(uint8_t *major, uint8_t *minor)
{
  uint8_t fwpReturn[2];

  if (!_initialised)
  {
    return false;
  }

  CHECK_SUCCESS(nvmem_read_sp_version(fwpReturn),
                CC3000_MSG_FAIL_READ_FW_VER, false);

  *major = fwpReturn[0];
  *minor = fwpReturn[1];

  return true;
}
#endif

/**************************************************************************/
/*!
    @Brief   Prints out the current status flag of the CC3000

    @note     This command isn't available when the CC3000 is configured
              in 'CC3000_TINY_DRIVER' mode
*/
/**************************************************************************/
status_t Adafruit_CC3000::getStatus()
{
  if (!_initialised)
  {
    return STATUS_DISCONNECTED;
  }

  long results = wlan_ioctl_statusget();

  switch(results)
  {
    case 1:
      return STATUS_SCANNING;
      break;
    case 2:
      return STATUS_CONNECTING;
      break;
    case 3:
      return STATUS_CONNECTED;
      break;
    case 0:
    default:
      return STATUS_DISCONNECTED;
      break;
  }
}

/**************************************************************************/
/*!
    @brief    Calls listSSIDs and then displays the results of the SSID scan

              For the moment we only list these via CC3KPrinter->print since
              this can consume a lot of memory passing all the data
              back with a buffer

    @note     This command isn't available when the CC3000 is configured
              in 'CC3000_TINY_DRIVER' mode

    @returns  False if an error occured!
*/
/**************************************************************************/
#ifndef CC3000_TINY_DRIVER

ResultStruct_t SSIDScanResultBuff;


uint16_t Adafruit_CC3000::startSSIDscan() {
  uint16_t   index = 0;

  if (!_initialised)
  {
    return false;
  }

  // Setup a 4 second SSID scan
  WDT_RESET();
  if (!scanSSIDs(4000))
  {
    // Oops ... SSID scan failed
    return false;
  }

  // Wait for results
  WDT_RESET();
  delay(4500);

  CHECK_SUCCESS(wlan_ioctl_get_scan_results(0, (uint8_t* ) &SSIDScanResultBuff),
                CC3000_MSG_FAIL_SSID_SCAN, false);

  index = SSIDScanResultBuff.num_networks;
  return index;
}

void Adafruit_CC3000::stopSSIDscan(void) {

  // Stop scanning
  scanSSIDs(0);
}

uint8_t Adafruit_CC3000::getNextSSID(uint8_t *rssi, uint8_t *secMode, char *ssidname) {
    uint8_t valid = (SSIDScanResultBuff.rssiByte & (~0xFE));
    *rssi = (SSIDScanResultBuff.rssiByte >> 1);
    *secMode = (SSIDScanResultBuff.Sec_ssidLen & (~0xFC));
    uint8_t ssidLen = (SSIDScanResultBuff.Sec_ssidLen >> 2);
    strncpy(ssidname, (char *)SSIDScanResultBuff.ssid_name, ssidLen);
    ssidname[ssidLen] = 0;

    CHECK_SUCCESS(wlan_ioctl_get_scan_results(0, (uint8_t* ) &SSIDScanResultBuff),
                  CC3000_MSG_ERR_SSID_SCAN_RESULT, false);
    return valid;
}
#endif

/**************************************************************************/
/*!
    @brief    Starts the smart config connection process

    @note     This command isn't available when the CC3000 is configured
              in 'CC3000_TINY_DRIVER' mode

    @returns  False if an error occured!
*/
/**************************************************************************/
#ifndef CC3000_TINY_DRIVER
bool Adafruit_CC3000::startSmartConfig(bool enableAES)
{
  ulSmartConfigFinished = 0;
  ulCC3000Connected = 0;
  ulCC3000DHCP = 0;
  OkToDoShutDown=0;

  uint32_t   timeout = 0;

  if (!_initialised) {
    return false;
  }

  // Reset all the previous configurations
  CHECK_SUCCESS(wlan_ioctl_set_connection_policy(WIFI_DISABLE, WIFI_DISABLE, WIFI_DISABLE),
                CC3000_MSG_FAIL_SET_CONN_POLICY_11, false);
  
  // Delete existing profile data
  CHECK_SUCCESS(wlan_ioctl_del_profile(255),
                CC3000_MSG_FAIL_DEL_PROFILES_13, false);

  // CC3KPrinter->println("Disconnecting");
  // Wait until CC3000 is disconnected
  while (ulCC3000Connected == WIFI_STATUS_CONNECTED) {
    WDT_RESET();
    cc3k_int_poll();
    CHECK_SUCCESS(wlan_disconnect(),
                  CC3000_MSG_FAIL_DISCONNECT_AP, false);
    delay(10);
    hci_unsolicited_event_handler();
  }

  // Reset the CC3000
  wlan_stop();
  WDT_RESET();
  delay(1000);
  wlan_start(0);

  // create new entry for AES encryption key
  CHECK_SUCCESS(nvmem_create_entry(NVMEM_AES128_KEY_FILEID,16),
                CC3000_MSG_FAIL_NEW_NVMEM_ENTRY, false);
  
  // write AES key to NVMEM
  CHECK_SUCCESS(aes_write_key((unsigned char *)(&_smartConfigKey[0])),
                CC3000_MSG_FAIL_WRITE_AES_KEY, false);
  
  //CC3KPrinter->println("Set prefix");
  // Wait until CC3000 is disconnected
  CHECK_SUCCESS(wlan_smart_config_set_prefix((char *)&_cc3000_prefix),
                CC3000_MSG_FAIL_SMART_CFG_PREFIX, false);

  //CC3KPrinter->println("Start config");
  // Start the SmartConfig start process
  CHECK_SUCCESS(wlan_smart_config_start(0),
                CC3000_MSG_FAIL_START_SMART_CFG, false);

  // Wait for smart config process complete (event in CC3000_UsynchCallback)
  while (ulSmartConfigFinished == 0)
  {
    WDT_RESET();
    cc3k_int_poll();
    // waiting here for event SIMPLE_CONFIG_DONE
    timeout+=10;
    if (timeout > 60000)   // ~60s
    {
      return false;
    }
    delay(10); // ms
    //  CC3KPrinter->print('.');
  }

  CC3KPrinter->println(F("Got smart config data"));
  if (enableAES) {
    CHECK_SUCCESS(wlan_smart_config_process(),
                 CC3000_MSG_FAIL_SMART_CFG_PROC, false);
  }
  
  // ******************************************************
  // Decrypt configuration information and add profile
  // ToDo: This is causing stack overflow ... investigate
  // CHECK_SUCCESS(wlan_smart_config_process(),
  //             "Smart config failed", false);
  // ******************************************************

  // Connect automatically to the AP specified in smart config settings
  CHECK_SUCCESS(wlan_ioctl_set_connection_policy(WIFI_DISABLE, WIFI_DISABLE, WIFI_ENABLE),
                CC3000_MSG_FAIL_SET_CONN_POLICY_19, false);
  
  // Reset the CC3000
  wlan_stop();
  WDT_RESET();
  delay(1000);
  wlan_start(0);
  
  // Mask out all non-required events
  CHECK_SUCCESS(wlan_set_event_mask(HCI_EVNT_WLAN_KEEPALIVE |
                HCI_EVNT_WLAN_UNSOL_INIT
                //HCI_EVNT_WLAN_ASYNC_PING_REPORT |
                //HCI_EVNT_WLAN_TX_COMPLETE
                ),
                CC3000_MSG_FAIL_SET_EVNT_MASK_20, false);

  // Wait for a connection
  timeout = 0;
  while(!ulCC3000Connected)
  {
    WDT_RESET();
    cc3k_int_poll();
    if(timeout > WLAN_CONNECT_TIMEOUT) // ~20s
    {
      if (CC3KPrinter != 0) {
        CC3KPrinter->println(F(CC3000_MSG_TIMEOUT_CONNECT));
      }
      return false;
    }
    timeout += 10;
    delay(10);
  }

  WDT_RESET();
  delay(1000);
  if (ulCC3000DHCP)
  {
    mdnsAdvertiser(1, (char *) _deviceName, strlen(_deviceName));
  }

  return true;
}

#endif
  
/**************************************************************************/
/*!
    Connect to an unsecured SSID/AP(security)

    @param  ssid      The named of the AP to connect to (max 32 chars)
    @param  ssidLen   The size of the ssid name

    @returns  False if an error occured!
*/
/**************************************************************************/
#if !defined(CC3000_TINY_DRIVER) || !defined(CC3000_SECURE)
bool Adafruit_CC3000::connectOpen(const char *ssid)
{
  if (!_initialised) {
    return false;
  }

//#ifndef CC3000_TINY_DRIVER
    CHECK_SUCCESS(wlan_ioctl_set_connection_policy(0, 0, 0),
                 CC3000_MSG_FAIL_SET_CONN_POLICY_22, false);
    WDT_RESET();
    delay(500);
    CHECK_SUCCESS(wlan_connect(WLAN_SEC_UNSEC,
					(const char*)ssid, strlen(ssid),
					0 ,NULL,0),
					CC3000_MSG_FAIL_CONNECT_SSID_23, false);
/*
#else
    WDT_RESET();
    wlan_connect(ssid, strlen(ssidLen));
#endif
*/
  return true;
}
#endif

//*****************************************************************************
//
//! CC3000_UsynchCallback
//!
//! @param  lEventType Event type
//! @param  data
//! @param  length
//!
//! @return none
//!
//! @brief  The function handles asynchronous events that come from CC3000
//!         device and operates a led for indicate
//
//*****************************************************************************
void CC3000_UsynchCallback(long lEventType, char * data, unsigned char length)
{
#ifndef CC3000_TINY_DRIVER
  if (lEventType == HCI_EVNT_WLAN_ASYNC_SIMPLE_CONFIG_DONE)
  {
    ulSmartConfigFinished = 1;
    ucStopSmartConfig     = 1;
  }
#endif

  if (lEventType == HCI_EVNT_WLAN_UNSOL_CONNECT)
  {
    ulCC3000Connected = 1;
  }

  if (lEventType == HCI_EVNT_WLAN_UNSOL_DISCONNECT)
  {
    ulCC3000Connected = 0;
    ulCC3000DHCP      = 0;
    ulCC3000DHCP_configured = 0;
  }
  
  if (lEventType == HCI_EVNT_WLAN_UNSOL_DHCP)
  {
    ulCC3000DHCP = 1;
  }

#ifndef CC3000_TINY_DRIVER
  if (lEventType == HCI_EVENT_CC3000_CAN_SHUT_DOWN)
  {
    OkToDoShutDown = 1;
  }

  if (lEventType == HCI_EVNT_WLAN_ASYNC_PING_REPORT)
  {
    //PRINT_F("CC3000: Ping report\n\r");
    pingReportnum++;
    memcpy(&pingReport, data, length);
  }
#endif

  if (lEventType == HCI_EVNT_BSD_TCP_CLOSE_WAIT) {
    uint8_t socketnum;
    socketnum = data[0];
    //PRINT_F("TCP Close wait #"); printDec(socketnum);
    if (socketnum < MAX_SOCKETS)
      closed_sockets[socketnum] = true;
  }
}

/**************************************************************************/
/*!
    Connect to an SSID/AP(security)

    @note     This command isn't available when the CC3000 is configured
              in 'CC3000_TINY_DRIVER' mode

    @returns  False if an error occured!
*/
/**************************************************************************/
#if ! defined(CC3000_TINY_DRIVER) || defined(CC3000_SECURE)
bool Adafruit_CC3000::connectSecure(const char *ssid, const char *key, int32_t secMode)
{
  if (!_initialised) {
    return false;
  }
  
#ifndef CC3000_TINY_DRIVER
  if ( (secMode < 0) || (secMode > 3)) {
    if (CC3KPrinter != 0) CC3KPrinter->println(F(CC3000_MSG_FAIL_SET_SECURITY_MODE));
    return false;
  }

  if (strlen(ssid) > MAXSSID) {
    if (CC3KPrinter != 0) {
#ifdef CC3000_MESSAGES_VERBOSE
      CC3KPrinter->print(F(CC3000_MSG_ERR_SSID_LENGTH));
      CC3KPrinter->println(MAXSSID);
#else
      CC3KPrinter->println(F(CC3000_MSG_ERR_SSID_LENGTH));
#endif
    }
    return false;
  }

  if (strlen(key) > MAXLENGTHKEY) {
    if (CC3KPrinter != 0) {
#ifdef CC3000_MESSAGES_VERBOSE
      CC3KPrinter->print(F(CC3000_MSG_ERR_KEY_LENGTH));
      CC3KPrinter->println(MAXLENGTHKEY);
#else
      CC3KPrinter->println(F(CC3000_MSG_ERR_KEY_LENGTH));
#endif
    }
    return false;
  }
#endif

  CHECK_SUCCESS(wlan_ioctl_set_connection_policy(0, 0, 0),
                CC3000_MSG_FAIL_SET_CONN_POLICY_27, false);
  WDT_RESET();
  delay(500);
  CHECK_SUCCESS(wlan_connect(secMode, (char *)ssid, strlen(ssid),
                             NULL,
                             (unsigned char *)key, strlen(key)),
                             CC3000_MSG_FAIL_CONNECT_SSID_28, false);

  /* Wait for 'HCI_EVNT_WLAN_UNSOL_CONNECT' in CC3000_UsynchCallback */

  return true;
}
#endif

// Connect with timeout
bool Adafruit_CC3000::connectToAP(const char *ssid, const char *key, uint8_t secmode) {
  if (!_initialised) {
    return false;
  }

  int16_t timer;

  do {
    WDT_RESET();
    cc3k_int_poll();
    /* MEME: not sure why this is absolutely required but the cc3k freaks
       if you dont. maybe bootup delay? */
    // Setup a 4 second SSID scan
    WDT_RESET();
    scanSSIDs(4000);
    // Wait for results
    WDT_RESET();
    delay(4500);
    WDT_RESET();
    scanSSIDs(0);
    
    /* Attempt to connect to an access point */
    if (CC3KPrinter != 0) {
      CC3KPrinter->print(F(CC3000_MSG_CONNECTING));
      CC3KPrinter->print(ssid);
      CC3KPrinter->print(F("..."));
    }
#ifndef CC3000_SECURE
    if ((secmode == 0) || (strlen(key) == 0)) {
      /* Connect to an unsecured network */
      if (! connectOpen(ssid)) {
        if (CC3KPrinter != 0) CC3KPrinter->println(F(CC3000_MSG_FAIL_CONNECT_OPEN));
        continue;
      }
    } else {
      /* NOTE: Secure connections are not available in 'Tiny' mode! */
#endif
#if ! defined(CC3000_TINY_DRIVER) || defined(CC3000_SECURE)
      /* Connect to a secure network using WPA2, etc */
      if (! connectSecure(ssid, key, secmode)) {
        if (CC3KPrinter != 0) CC3KPrinter->println(F(CC3000_MSG_FAIL_CONNECT_SECURE));
        continue;
      }
#endif
#ifndef CC3000_SECURE
    }
#endif
	  
    timer = WLAN_CONNECT_TIMEOUT;

    /* Wait around a bit for the async connected signal to arrive or timeout */
    if (CC3KPrinter != 0) CC3KPrinter->print(F(CC3000_MSG_WAITING_CONNECT));
    while ((timer > 0) && !checkConnected())
    {
      cc3k_int_poll();
      delay(10);
      timer -= 10;
    }
    if (timer <= 0) {
      if (CC3KPrinter != 0) CC3KPrinter->println(F(CC3000_MSG_TIMEOUT));
    }
  } while (!checkConnected());

  return true;
}


#ifndef CC3000_TINY_DRIVER
uint16_t Adafruit_CC3000::ping(uint32_t ip, uint8_t attempts, uint16_t timeout, uint8_t size) {
  if (!_initialised) return 0;
  if (!ulCC3000Connected) return 0;
  if (!ulCC3000DHCP) return 0;

  uint32_t revIP = (ip >> 24) | ((ip >> 8) & 0xFF00) | ((ip & 0xFF00) << 8) | (ip << 24);

  pingReportnum = 0;
  pingReport.packets_received = 0;

  //if (CC3KPrinter != 0) {
  //  CC3KPrinter->print(F("Pinging ")); printIPdots(revIP); CC3KPrinter->print(" ");
  //  CC3KPrinter->print(attempts); CC3KPrinter->println(F(" times"));
  //}
  
  netapp_ping_send(&revIP, attempts, size, timeout);
  delay(timeout*attempts*2);
  //if (CC3KPrinter != 0) CC3KPrinter->println(F("Req report"));
  //netapp_ping_report();
  //if (CC3KPrinter != 0) { CC3KPrinter->print(F("Reports: ")); CC3KPrinter->println(pingReportnum); }

  if (pingReportnum) {
    /*
    if (CC3KPrinter != 0) {
      CC3KPrinter->print(F("Sent: ")); CC3KPrinter->println(pingReport.packets_sent);
      CC3KPrinter->print(F("Recv: ")); CC3KPrinter->println(pingReport.packets_received);
      CC3KPrinter->print(F("MinT: ")); CC3KPrinter->println(pingReport.min_round_time);
      CC3KPrinter->print(F("MaxT: ")); CC3KPrinter->println(pingReport.max_round_time);
      CC3KPrinter->print(F("AvgT: ")); CC3KPrinter->println(pingReport.avg_round_time);
    }
    //*/
    return pingReport.packets_received;
  } else {
    return 0;
  }
}

#endif

#ifndef CC3000_TINY_DRIVER
uint16_t Adafruit_CC3000::getHostByName(char *hostname, uint32_t *ip) {
  if (!_initialised) return 0;
  if (!ulCC3000Connected) return 0;
  if (!ulCC3000DHCP) return 0;

  int16_t r = gethostbyname(hostname, strlen(hostname), ip);
  //if (CC3KPrinter != 0) { CC3KPrinter->print("Errno: "); CC3KPrinter->println(r); }
  return r;
}
#endif

/**************************************************************************/
/*!
    Checks if the device is connected or not

    @returns  True if connected
*/
/**************************************************************************/
bool Adafruit_CC3000::checkConnected(void)
{
  return ulCC3000Connected ? true : false;
}

/**************************************************************************/
/*!
    Checks if the DHCP process is complete or not

    @returns  True if DHCP process is complete (IP address assigned)
*/
/**************************************************************************/
bool Adafruit_CC3000::checkDHCP(void)
{
  return ulCC3000DHCP ? true : false;
}

/**************************************************************************/
/*!
    Checks if the smart config process is finished or not

    @returns  True if smart config is finished
*/
/**************************************************************************/
#ifndef CC3000_TINY_DRIVER
bool Adafruit_CC3000::checkSmartConfigFinished(void)
{
  return ulSmartConfigFinished ? true : false;
}
#endif

#ifndef CC3000_TINY_DRIVER
/**************************************************************************/
/*!
    Gets the IP config settings (if connected)

    @returns  True if smart config is finished
*/
/**************************************************************************/
bool Adafruit_CC3000::getIPConfig(tNetappIpconfigRetArgs *ipConfig)
{
  if (!_initialised)      return false;
  if (!ulCC3000Connected) return false;
  if (!ulCC3000DHCP)      return false;
  
  netapp_ipconfig(ipConfig);
  return true;
}
#endif


/**************************************************************************/
/*!
    @brief  Quick socket test to pull contents from the web
*/
/**************************************************************************/
Adafruit_CC3000_Client Adafruit_CC3000::connectTCP(uint32_t destIP, uint16_t destPort)
{
  sockaddr      socketAddress;
  int32_t       tcp_socket;

  // Create the socket(s)
  //if (CC3KPrinter != 0) CC3KPrinter->print(F("Creating socket ... "));
  tcp_socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (-1 == tcp_socket)
  {
    if (CC3KPrinter != 0) CC3KPrinter->println(F(CC3000_MSG_FAIL_OPEN_SOCKET_TCP));
    return Adafruit_CC3000_Client();
  }
  //CC3KPrinter->print(F("DONE (socket ")); CC3KPrinter->print(tcp_socket); CC3KPrinter->println(F(")"));

  // Try to open the socket
  memset(&socketAddress, 0x00, sizeof(socketAddress));
  socketAddress.sa_family = AF_INET;
  socketAddress.sa_data[0] = (destPort & 0xFF00) >> 8;  // Set the Port Number
  socketAddress.sa_data[1] = (destPort & 0x00FF);
  socketAddress.sa_data[2] = destIP >> 24;
  socketAddress.sa_data[3] = destIP >> 16;
  socketAddress.sa_data[4] = destIP >> 8;
  socketAddress.sa_data[5] = destIP;

  if (CC3KPrinter != 0) {
    CC3KPrinter->print(F(CC3000_MSG_CONNECT_TO));
    printIPdotsRev(destIP);
    CC3KPrinter->print(':');
    CC3KPrinter->println(destPort);
  }

  //printHex((byte *)&socketAddress, sizeof(socketAddress));
  //if (CC3KPrinter != 0) CC3KPrinter->print(F("Connecting socket ... "));
  if (-1 == connect(tcp_socket, &socketAddress, sizeof(socketAddress)))
  {
    if (CC3KPrinter != 0) CC3KPrinter->println(F(CC3000_MSG_ERR_CONN_TCP));
    closesocket(tcp_socket);
    return Adafruit_CC3000_Client();
  }
  //if (CC3KPrinter != 0) CC3KPrinter->println(F("DONE"));
  return Adafruit_CC3000_Client(tcp_socket);
}


#ifndef CC3000_TINY_DRIVER
Adafruit_CC3000_Client Adafruit_CC3000::connectUDP(uint32_t destIP, uint16_t destPort)
{
  sockaddr      socketAddress;
  int32_t       udp_socket;

  // Create the socket(s)
  // socket   = SOCK_STREAM, SOCK_DGRAM, or SOCK_RAW 
  // protocol = IPPROTO_TCP, IPPROTO_UDP or IPPROTO_RAW
  //if (CC3KPrinter != 0) CC3KPrinter->print(F("Creating socket... "));
  udp_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (-1 == udp_socket)
  {
    CC3KPrinter->println(F(CC3000_MSG_FAIL_OPEN_SOCKET_UDP));
    return Adafruit_CC3000_Client();
  }
  //if (CC3KPrinter != 0) { CC3KPrinter->print(F("DONE (socket ")); CC3KPrinter->print(udp_socket); CC3KPrinter->println(F(")")); }

  // Try to open the socket
  memset(&socketAddress, 0x00, sizeof(socketAddress));
  socketAddress.sa_family = AF_INET;
  socketAddress.sa_data[0] = (destPort & 0xFF00) >> 8;  // Set the Port Number
  socketAddress.sa_data[1] = (destPort & 0x00FF);
  socketAddress.sa_data[2] = destIP >> 24;
  socketAddress.sa_data[3] = destIP >> 16;
  socketAddress.sa_data[4] = destIP >> 8;
  socketAddress.sa_data[5] = destIP;

  if (CC3KPrinter != 0) {
    CC3KPrinter->print(F(CC3000_MSG_CONNECT_TO));
    printIPdotsRev(destIP);
    CC3KPrinter->print(':');
    CC3KPrinter->println(destPort);
  }

  //printHex((byte *)&socketAddress, sizeof(socketAddress));
  if (-1 == connect(udp_socket, &socketAddress, sizeof(socketAddress)))
  {
    if (CC3KPrinter != 0) CC3KPrinter->println(F(CC3000_MSG_ERR_CONN_UDP));
    closesocket(udp_socket);
    return Adafruit_CC3000_Client();
  }

  return Adafruit_CC3000_Client(udp_socket);
}

#endif

/**********************************************************************/
Adafruit_CC3000_Client::Adafruit_CC3000_Client(void) {
  _socket = -1;
}

Adafruit_CC3000_Client::Adafruit_CC3000_Client(uint16_t s) {
  _socket = s; 
  bufsiz = 0;
  _rx_buf_idx = 0;
}

Adafruit_CC3000_Client::Adafruit_CC3000_Client(const Adafruit_CC3000_Client& copy) {
  // Copy all the members to construct this client.
  _socket = copy._socket;
  bufsiz = copy.bufsiz;
  _rx_buf_idx = copy._rx_buf_idx;
  memcpy(_rx_buf, copy._rx_buf, RXBUFFERSIZE);
}

void Adafruit_CC3000_Client::operator=(const Adafruit_CC3000_Client& other) {
  // Copy all the members to assign a new value to this client.
  _socket = other._socket;
  bufsiz = other.bufsiz;
  _rx_buf_idx = other._rx_buf_idx;
  memcpy(_rx_buf, other._rx_buf, RXBUFFERSIZE);
}

bool Adafruit_CC3000_Client::connected(void) { 
  if (_socket < 0) return false;

  if (! available() && closed_sockets[_socket] == true) {
    //if (CC3KPrinter != 0) CC3KPrinter->println("No more data, and closed!");
    closesocket(_socket);
    closed_sockets[_socket] = false;
    _socket = -1;
    return false;
  }

  else return true;  
}

int16_t Adafruit_CC3000_Client::write(const void *buf, uint16_t len, uint32_t flags)
{
  return send(_socket, buf, len, flags);
}


size_t Adafruit_CC3000_Client::write(uint8_t c)
{
  int32_t r;
  r = send(_socket, &c, 1, 0);
  if ( r < 0 ) return 0;
  return r;
}

size_t Adafruit_CC3000_Client::fastrprint(const __FlashStringHelper *ifsh)
{
  char _tx_buf[TXBUFFERSIZE];
  uint8_t idx = 0;

  const char PROGMEM *p = (const char PROGMEM *)ifsh;
  size_t n = 0;
  while (1) {
    unsigned char c = pgm_read_byte(p++);
    if (c == 0) break;
    _tx_buf[idx] = c;
    idx++;
    if (idx >= TXBUFFERSIZE) {
      // lets send it!
      n += send(_socket, _tx_buf, TXBUFFERSIZE, 0);
      idx = 0;
    }
  }
  if (idx > 0) {
    // Send any remaining data in the transmit buffer.
    n += send(_socket, _tx_buf, idx, 0);
  }

  return n;
}

#ifndef CC3000_TINY_DRIVER
size_t Adafruit_CC3000_Client::fastrprintln(const __FlashStringHelper *ifsh)
{
  size_t r = 0;
  r = fastrprint(ifsh);
  r+= fastrprint(F("\n\r"));
  return r;
}

size_t Adafruit_CC3000_Client::fastrprintln(const char *str)
{
  size_t r = 0;
  size_t len = strlen(str);
  if (len > 0) {
    if ((r = write(str, len, 0)) <= 0) return 0;
  }
  if ((r += write("\n\r", 2, 0)) <= 0) return 0;  // meme fix
  return r;
}
#endif

size_t Adafruit_CC3000_Client::fastrprint(const char *str)
{
  size_t len = strlen(str);
  if (len > 0) {
    return write(str, len, 0);
  }
  else {
    return 0;
  }
}

#ifndef CC3000_TINY_DRIVER
int16_t Adafruit_CC3000_Client::read(void *buf, uint16_t len, uint32_t flags)
{
  return recv(_socket, buf, len, flags);

}
#endif

int32_t Adafruit_CC3000_Client::close(void) {
  int32_t x = closesocket(_socket);
  _socket = -1;
  return x;
}

uint8_t Adafruit_CC3000_Client::read(void) 
{
  while ((bufsiz <= 0) || (bufsiz == _rx_buf_idx)) {
    cc3k_int_poll();
    // buffer in some more data
    bufsiz = recv(_socket, _rx_buf, sizeof(_rx_buf), 0);
    if (bufsiz == -57) {
      close();
      return 0;
    }
    //if (CC3KPrinter != 0) { CC3KPrinter->println("Read "); CC3KPrinter->print(bufsiz); CC3KPrinter->println(" bytes"); }
    _rx_buf_idx = 0;
  }
  uint8_t ret = _rx_buf[_rx_buf_idx];
  _rx_buf_idx++;
  //if (CC3KPrinter != 0) { CC3KPrinter->print("("); CC3KPrinter->write(ret); CC3KPrinter->print(")"); }
  return ret;
}

uint8_t Adafruit_CC3000_Client::available(void) {
  // not open!
  if (_socket < 0) return 0;

  if ((bufsiz > 0) // we have some data in the internal buffer
      && (_rx_buf_idx < bufsiz)) {  // we havent already spit it all out
    return (bufsiz - _rx_buf_idx);
  }

  // do a select() call on this socket
  timeval timeout;
  fd_set fd_read;

  memset(&fd_read, 0, sizeof(fd_read));
  FD_SET(_socket, &fd_read);

  timeout.tv_sec = 0;
  timeout.tv_usec = 5000; // 5 millisec

  int16_t s = select(_socket+1, &fd_read, NULL, NULL, &timeout);
  //if (CC3KPrinter != 0) } CC3KPrinter->print(F("Select: ")); CC3KPrinter->println(s); }
  if (s == 1) return 1;  // some data is available to read
  else return 0;  // no data is available
}

void Adafruit_CC3000::setPrinter(Print* p) {
  CC3KPrinter = p;
}
