/* J2534 PassThru API - SAE J2534-1 standard definitions */
#ifndef J2534_H
#define J2534_H

#include <stdint.h>

/* J2534 exports are __stdcall on Windows, plain cdecl on Linux. */
#ifdef _WIN32
  #define J2534_API   __stdcall
  #ifdef MVCI_BUILDING_DLL
    #define J2534_EXPORT __declspec(dllexport)
  #else
    #define J2534_EXPORT
  #endif
#else
  #define J2534_API
  #define J2534_EXPORT __attribute__((visibility("default")))
#endif

/* Protocol IDs */
#define J1850VPW        0x01
#define J1850PWM        0x02
#define ISO9141         0x03
#define ISO14230        0x04
#define CAN             0x05
#define ISO15765        0x06
#define SCI_A_ENGINE    0x07
#define SCI_A_TRANS     0x08
#define SCI_B_ENGINE    0x09
#define SCI_B_TRANS     0x0A

/* Connect flags */
#define CAN_29BIT_ID        0x0100
#define ISO9141_NO_CHECKSUM 0x0200
#define CAN_ID_BOTH         0x0800
#define ISO9141_K_LINE_ONLY 0x1000

/* Ioctl IDs */
#define GET_CONFIG          0x01
#define SET_CONFIG          0x02
#define READ_VBATT          0x03
#define FIVE_BAUD_INIT      0x04
#define FAST_INIT           0x05
#define CLEAR_TX_BUFFER     0x07
#define CLEAR_RX_BUFFER     0x08
#define CLEAR_PERIODIC_MSGS 0x09
#define CLEAR_MSG_FILTERS   0x0A
#define CLEAR_FUNCT_MSG_LOOKUP_TABLE 0x0B
#define ADD_TO_FUNCT_MSG_LOOKUP_TABLE 0x0C
#define DELETE_FROM_FUNCT_MSG_LOOKUP_TABLE 0x0D
#define READ_PROG_VOLTAGE   0x0E

/* Config parameter IDs */
#define DATA_RATE           0x01
#define LOOPBACK            0x03
#define NODE_ADDRESS        0x04
#define NETWORK_LINE        0x05
#define P1_MIN              0x06
#define P1_MAX              0x07
#define P2_MIN              0x08
#define P2_MAX              0x09
#define P3_MIN              0x0A
#define P3_MAX              0x0B
#define P4_MIN              0x0C
#define P4_MAX              0x0D
#define W1                  0x0E
#define W2                  0x0F
#define W3                  0x10
#define W4                  0x11
#define W5                  0x12
#define TIDLE               0x13
#define TINIL               0x14
#define TWUP                0x15
#define PARITY              0x16
#define BIT_SAMPLE_POINT    0x17
#define SYNC_JUMP_WIDTH     0x18
#define T1_MAX              0x19
#define T2_MAX              0x1A
#define T4_MAX              0x1C
#define T5_MAX              0x1D
#define ISO15765_BS         0x1E
#define ISO15765_STMIN      0x1F
#define DATA_BITS           0x20
#define FIVE_BAUD_MOD       0x21
#define BS_TX               0x22
#define STMIN_TX            0x23
#define T3_MAX              0x24
#define ISO15765_WFT_MAX    0x25

/* Filter types */
#define PASS_FILTER         0x01
#define BLOCK_FILTER        0x02
#define FLOW_CONTROL_FILTER 0x03

/* RxStatus flags */
#define TX_MSG_TYPE         0x0001
#define START_OF_MESSAGE    0x0002
#define RX_BREAK            0x0004
#define TX_INDICATION       0x0008
#define ISO15765_PADDING_ERROR 0x0010
#define ISO15765_ADDR_TYPE  0x0080

/* TxFlags */
#define ISO15765_FRAME_PAD  0x0040
#define WAIT_P3_MIN_ONLY    0x0200

/* Error codes */
#define STATUS_NOERROR          0x00
#define ERR_NOT_SUPPORTED       0x01
#define ERR_INVALID_CHANNEL_ID  0x02
#define ERR_INVALID_PROTOCOL_ID 0x03
#define ERR_NULL_PARAMETER      0x04
#define ERR_INVALID_IOCTL_VALUE 0x05
#define ERR_INVALID_FLAGS       0x06
#define ERR_FAILED              0x07
#define ERR_DEVICE_NOT_CONNECTED 0x08
#define ERR_TIMEOUT             0x09
#define ERR_INVALID_MSG         0x0A
#define ERR_INVALID_TIME_INTERVAL 0x0B
#define ERR_EXCEEDED_LIMIT      0x0C
#define ERR_INVALID_MSG_ID      0x0D
#define ERR_DEVICE_IN_USE       0x0E
#define ERR_INVALID_IOCTL_ID    0x0F
#define ERR_BUFFER_EMPTY        0x10
#define ERR_BUFFER_FULL         0x11
#define ERR_BUFFER_OVERFLOW     0x12
#define ERR_PIN_INVALID         0x13
#define ERR_CHANNEL_IN_USE      0x14
#define ERR_MSG_PROTOCOL_ID     0x15
#define ERR_INVALID_FILTER_ID   0x16
#define ERR_NO_FLOW_CONTROL     0x17
#define ERR_NOT_UNIQUE          0x18
#define ERR_INVALID_BAUDRATE    0x19
#define ERR_INVALID_DEVICE_ID   0x1A

#define PASSTHRU_MSG_DATA_SIZE  4128

typedef struct {
    uint32_t ProtocolID;
    uint32_t RxStatus;
    uint32_t TxFlags;
    uint32_t Timestamp;
    uint32_t DataSize;
    uint32_t ExtraDataIndex;
    uint8_t  Data[PASSTHRU_MSG_DATA_SIZE];
} PASSTHRU_MSG;

typedef struct {
    uint32_t Parameter;
    uint32_t Value;
} SCONFIG;

typedef struct {
    uint32_t NumOfParams;
    SCONFIG *ConfigPtr;
} SCONFIG_LIST;

typedef struct {
    uint32_t NumOfBytes;
    uint8_t  *BytePtr;
} SBYTE_ARRAY;

/* J2534 API */
J2534_EXPORT long J2534_API PassThruOpen(void *pName, unsigned long *pDeviceID);
J2534_EXPORT long J2534_API PassThruClose(unsigned long DeviceID);
J2534_EXPORT long J2534_API PassThruConnect(unsigned long DeviceID, unsigned long ProtocolID,
                     unsigned long Flags, unsigned long BaudRate,
                     unsigned long *pChannelID);
J2534_EXPORT long J2534_API PassThruDisconnect(unsigned long ChannelID);
J2534_EXPORT long J2534_API PassThruReadMsgs(unsigned long ChannelID, PASSTHRU_MSG *pMsg,
                      unsigned long *pNumMsgs, unsigned long Timeout);
J2534_EXPORT long J2534_API PassThruWriteMsgs(unsigned long ChannelID, PASSTHRU_MSG *pMsg,
                       unsigned long *pNumMsgs, unsigned long TimeInterval);
J2534_EXPORT long J2534_API PassThruStartPeriodicMsg(unsigned long ChannelID, PASSTHRU_MSG *pMsg,
                               unsigned long *pMsgID, unsigned long TimeInterval);
J2534_EXPORT long J2534_API PassThruStopPeriodicMsg(unsigned long ChannelID, unsigned long MsgID);
J2534_EXPORT long J2534_API PassThruStartMsgFilter(unsigned long ChannelID, unsigned long FilterType,
                             PASSTHRU_MSG *pMaskMsg, PASSTHRU_MSG *pPatternMsg,
                             PASSTHRU_MSG *pFlowControlMsg, unsigned long *pFilterID);
J2534_EXPORT long J2534_API PassThruStopMsgFilter(unsigned long ChannelID, unsigned long FilterID);
J2534_EXPORT long J2534_API PassThruSetProgrammingVoltage(unsigned long DeviceID, unsigned long PinNumber,
                                    unsigned long Voltage);
J2534_EXPORT long J2534_API PassThruReadVersion(unsigned long DeviceID, char *pFirmwareVersion,
                          char *pDllVersion, char *pApiVersion);
J2534_EXPORT long J2534_API PassThruGetLastError(char *pErrorDescription);
J2534_EXPORT long J2534_API PassThruIoctl(unsigned long ChannelID, unsigned long IoctlID,
                   void *pInput, void *pOutput);

#endif /* J2534_H */
