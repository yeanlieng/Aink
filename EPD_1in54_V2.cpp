/*****************************************************************************
* | File      	:   EPD_1in54_V2.c
* | Author      :   Waveshare team
* | Function    :   1.54inch e-paper V2
******************************************************************************/
#include "EPD_1in54_V2.h"

#define EPD_BUSY_TIMING_DEBUG 1
#define EPD_BUSY_TIMEOUT_MS   35000
#define EPD_BUSY_BLOCK_DELAY_MS 10

static uint32_t epd_busy_read_count = 0;
static bool epd_busy_wait_active = false;
static const char *epd_busy_wait_label = "busy";
static uint32_t epd_busy_wait_start = 0;
static uint32_t epd_busy_wait_elapsed = 0;

unsigned char WF_Full_1IN54[159] =
{
0x80,	0x48,	0x40,	0x0,	0x0,	0x0,	0x0,	0x0,	0x0,	0x0,	0x0,	0x0,
0x40,	0x48,	0x80,	0x0,	0x0,	0x0,	0x0,	0x0,	0x0,	0x0,	0x0,	0x0,
0x80,	0x48,	0x40,	0x0,	0x0,	0x0,	0x0,	0x0,	0x0,	0x0,	0x0,	0x0,
0x40,	0x48,	0x80,	0x0,	0x0,	0x0,	0x0,	0x0,	0x0,	0x0,	0x0,	0x0,
0x0,	0x0,	0x0,	0x0,	0x0,	0x0,	0x0,	0x0,	0x0,	0x0,	0x0,	0x0,
0xA,	0x0,	0x0,	0x0,	0x0,	0x0,	0x0,
0x8,	0x1,	0x0,	0x8,	0x1,	0x0,	0x2,
0xA,	0x0,	0x0,	0x0,	0x0,	0x0,	0x0,
0x0,	0x0,	0x0,	0x0,	0x0,	0x0,	0x0,
0x0,	0x0,	0x0,	0x0,	0x0,	0x0,	0x0,
0x0,	0x0,	0x0,	0x0,	0x0,	0x0,	0x0,
0x0,	0x0,	0x0,	0x0,	0x0,	0x0,	0x0,
0x0,	0x0,	0x0,	0x0,	0x0,	0x0,	0x0,
0x0,	0x0,	0x0,	0x0,	0x0,	0x0,	0x0,
0x0,	0x0,	0x0,	0x0,	0x0,	0x0,	0x0,
0x0,	0x0,	0x0,	0x0,	0x0,	0x0,	0x0,
0x0,	0x0,	0x0,	0x0,	0x0,	0x0,	0x0,
0x22,	0x22,	0x22,	0x22,	0x22,	0x22,	0x0,	0x0,	0x0,
0x22,	0x17,	0x41,	0x0,	0x32,	0x20
};

unsigned char WF_PARTIAL_1IN54_0[159] =
{
0x0,0x40,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,
0x80,0x80,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,
0x40,0x40,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,
0x0,0x80,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,
0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,
0xF,0x0,0x0,0x0,0x0,0x0,0x0,
0x1,0x1,0x0,0x0,0x0,0x0,0x0,
0x0,0x0,0x0,0x0,0x0,0x0,0x0,
0x0,0x0,0x0,0x0,0x0,0x0,0x0,
0x0,0x0,0x0,0x0,0x0,0x0,0x0,
0x0,0x0,0x0,0x0,0x0,0x0,0x0,
0x0,0x0,0x0,0x0,0x0,0x0,0x0,
0x0,0x0,0x0,0x0,0x0,0x0,0x0,
0x0,0x0,0x0,0x0,0x0,0x0,0x0,
0x0,0x0,0x0,0x0,0x0,0x0,0x0,
0x0,0x0,0x0,0x0,0x0,0x0,0x0,
0x0,0x0,0x0,0x0,0x0,0x0,0x0,
0x22,0x22,0x22,0x22,0x22,0x22,0x0,0x0,0x0,
0x02,0x17,0x41,0xB0,0x32,0x28,
};

static void EPD_1IN54_V2_Reset(void)
{
    DEV_Digital_Write(EPD_RST_PIN, 1);
    DEV_Delay_ms(20);
    DEV_Digital_Write(EPD_RST_PIN, 0);
    DEV_Delay_ms(2);
    DEV_Digital_Write(EPD_RST_PIN, 1);
    DEV_Delay_ms(20);
}

static void EPD_1IN54_V2_SendCommand(UBYTE Reg)
{
    DEV_Digital_Write(EPD_DC_PIN, 0);
    DEV_Digital_Write(EPD_CS_PIN, 0);
    DEV_SPI_WriteByte(Reg);
    DEV_Digital_Write(EPD_CS_PIN, 1);
}

static void EPD_1IN54_V2_SendData(UBYTE Data)
{
    DEV_Digital_Write(EPD_DC_PIN, 1);
    DEV_Digital_Write(EPD_CS_PIN, 0);
    DEV_SPI_WriteByte(Data);
    DEV_Digital_Write(EPD_CS_PIN, 1);
}

static void EPD_1IN54_V2_SendDataBuffer(UBYTE *Data, UDOUBLE Len)
{
    DEV_Digital_Write(EPD_DC_PIN, 1);
    DEV_Digital_Write(EPD_CS_PIN, 0);
    DEV_SPI_Write_nByte(Data, Len);
    DEV_Digital_Write(EPD_CS_PIN, 1);
}

bool EPD_1IN54_V2_IsBusy(void)
{
    return DEV_Digital_Read(EPD_BUSY_PIN) == 1;
}

void EPD_1IN54_V2_BeginBusyWait(const char *label)
{
    epd_busy_wait_label = label ? label : "busy";
    epd_busy_wait_start = millis();
    epd_busy_wait_elapsed = 0;
    epd_busy_wait_active = true;

#if EPD_BUSY_TIMING_DEBUG
    Serial.printf("[EPD] BUSY wait: %s (start=%d)...\r\n",
        epd_busy_wait_label, DEV_Digital_Read(EPD_BUSY_PIN));
#endif
}

bool EPD_1IN54_V2_PollBusyWait(void)
{
    if (!epd_busy_wait_active) {
        return false;
    }

    if (EPD_1IN54_V2_IsBusy()) {
        epd_busy_wait_elapsed = millis() - epd_busy_wait_start;
        if (epd_busy_wait_elapsed >= EPD_BUSY_TIMEOUT_MS) {
#if EPD_BUSY_TIMING_DEBUG
            Serial.printf("[EPD] BUSY TIMEOUT %-20s stuck HIGH after %lums\r\n",
                epd_busy_wait_label, (unsigned long)epd_busy_wait_elapsed);
#endif
            epd_busy_wait_active = false;
            return false;
        }
        return true;
    }

    epd_busy_wait_elapsed = millis() - epd_busy_wait_start;
    epd_busy_wait_active = false;

#if EPD_BUSY_TIMING_DEBUG
    Serial.printf("[EPD] BUSY done  #%-3lu %-20s %lums\r\n",
        ++epd_busy_read_count, epd_busy_wait_label, (unsigned long)epd_busy_wait_elapsed);
#endif

    return false;
}

bool EPD_1IN54_V2_BusyWaitActive(void)
{
    return epd_busy_wait_active;
}

static uint32_t EPD_1IN54_V2_WaitBusyWithDelay(uint32_t delayMs)
{
    while (EPD_1IN54_V2_PollBusyWait()) {
        DEV_Delay_ms(delayMs);
    }

    return epd_busy_wait_elapsed;
}

static uint32_t EPD_1IN54_V2_ReadBusy(const char *label)
{
    EPD_1IN54_V2_BeginBusyWait(label);
    return EPD_1IN54_V2_WaitBusyWithDelay(1);
}

static void EPD_1IN54_V2_TurnOnDisplayBegin(void)
{
    EPD_1IN54_V2_SendCommand(0x22);
    EPD_1IN54_V2_SendData(0xc7);
    EPD_1IN54_V2_SendCommand(0x20);
    EPD_1IN54_V2_BeginBusyWait("display-full");
}

static void EPD_1IN54_V2_TurnOnDisplay(void)
{
    EPD_1IN54_V2_TurnOnDisplayBegin();
    EPD_1IN54_V2_WaitBusyWithDelay(EPD_BUSY_BLOCK_DELAY_MS);
}

static void EPD_1IN54_V2_TurnOnDisplayPartBegin(void)
{
    EPD_1IN54_V2_SendCommand(0x22);
    EPD_1IN54_V2_SendData(0xcF);
    EPD_1IN54_V2_SendCommand(0x20);
    EPD_1IN54_V2_BeginBusyWait("display-partial");
}

static void EPD_1IN54_V2_TurnOnDisplayPart(void)
{
    EPD_1IN54_V2_TurnOnDisplayPartBegin();
    EPD_1IN54_V2_WaitBusyWithDelay(EPD_BUSY_BLOCK_DELAY_MS);
}

static void EPD_1IN54_V2_Lut(UBYTE *lut)
{
    EPD_1IN54_V2_SendCommand(0x32);
    for (UBYTE i = 0; i < 153; i++)
        EPD_1IN54_V2_SendData(lut[i]);
    EPD_1IN54_V2_ReadBusy("lut");
}

static void EPD_1IN54_V2_SetLut(UBYTE *lut)
{
    EPD_1IN54_V2_Lut(lut);

    EPD_1IN54_V2_SendCommand(0x3f);
    EPD_1IN54_V2_SendData(lut[153]);

    EPD_1IN54_V2_SendCommand(0x03);
    EPD_1IN54_V2_SendData(lut[154]);

    EPD_1IN54_V2_SendCommand(0x04);
    EPD_1IN54_V2_SendData(lut[155]);
    EPD_1IN54_V2_SendData(lut[156]);
    EPD_1IN54_V2_SendData(lut[157]);

    EPD_1IN54_V2_SendCommand(0x2c);
    EPD_1IN54_V2_SendData(lut[158]);
}

static void EPD_1IN54_V2_SetWindows(UWORD Xstart, UWORD Ystart, UWORD Xend, UWORD Yend)
{
    EPD_1IN54_V2_SendCommand(0x44);
    EPD_1IN54_V2_SendData((Xstart >> 3) & 0xFF);
    EPD_1IN54_V2_SendData((Xend >> 3) & 0xFF);

    EPD_1IN54_V2_SendCommand(0x45);
    EPD_1IN54_V2_SendData(Ystart & 0xFF);
    EPD_1IN54_V2_SendData((Ystart >> 8) & 0xFF);
    EPD_1IN54_V2_SendData(Yend & 0xFF);
    EPD_1IN54_V2_SendData((Yend >> 8) & 0xFF);
}

static void EPD_1IN54_V2_SetCursor(UWORD Xstart, UWORD Ystart)
{
    EPD_1IN54_V2_SendCommand(0x4E);
    EPD_1IN54_V2_SendData(Xstart & 0xFF);

    EPD_1IN54_V2_SendCommand(0x4F);
    EPD_1IN54_V2_SendData(Ystart & 0xFF);
    EPD_1IN54_V2_SendData((Ystart >> 8) & 0xFF);
}

void EPD_1IN54_V2_Init(void)
{
    EPD_1IN54_V2_Reset();

    EPD_1IN54_V2_ReadBusy("init-reset");
    EPD_1IN54_V2_SendCommand(0x12);
    EPD_1IN54_V2_ReadBusy("init-swreset");

    EPD_1IN54_V2_SendCommand(0x01);
    EPD_1IN54_V2_SendData(0xC7);
    EPD_1IN54_V2_SendData(0x00);
    EPD_1IN54_V2_SendData(0x01);

  EPD_1IN54_V2_SendCommand(0x11);
  EPD_1IN54_V2_SendData(0x01);

    EPD_1IN54_V2_SetWindows(0, EPD_1IN54_V2_HEIGHT - 1, EPD_1IN54_V2_WIDTH - 1, 0);

    EPD_1IN54_V2_SendCommand(0x3C);
    EPD_1IN54_V2_SendData(0x01);

    EPD_1IN54_V2_SendCommand(0x18);
    EPD_1IN54_V2_SendData(0x80);

    EPD_1IN54_V2_SendCommand(0x22);
    EPD_1IN54_V2_SendData(0xB1);
    EPD_1IN54_V2_SendCommand(0x20);

    EPD_1IN54_V2_SetCursor(0, EPD_1IN54_V2_HEIGHT - 1);
    EPD_1IN54_V2_ReadBusy("init-load-temp");

    EPD_1IN54_V2_SetLut(WF_Full_1IN54);
}

void EPD_1IN54_V2_Init_Partial(void)
{
    EPD_1IN54_V2_Reset();
    EPD_1IN54_V2_ReadBusy("partial-init-reset");

    EPD_1IN54_V2_SetLut(WF_PARTIAL_1IN54_0);
    EPD_1IN54_V2_SendCommand(0x37);
    EPD_1IN54_V2_SendData(0x00);
    EPD_1IN54_V2_SendData(0x00);
    EPD_1IN54_V2_SendData(0x00);
    EPD_1IN54_V2_SendData(0x00);
    EPD_1IN54_V2_SendData(0x00);
    EPD_1IN54_V2_SendData(0x40);
    EPD_1IN54_V2_SendData(0x00);
    EPD_1IN54_V2_SendData(0x00);
    EPD_1IN54_V2_SendData(0x00);

    EPD_1IN54_V2_SendCommand(0x3C);
    EPD_1IN54_V2_SendData(0x80);

    EPD_1IN54_V2_SendCommand(0x22);
    EPD_1IN54_V2_SendData(0xc0);
    EPD_1IN54_V2_SendCommand(0x20);
    EPD_1IN54_V2_ReadBusy("partial-init-power");
}

void EPD_1IN54_V2_Enter_Partial(void)
{
    EPD_1IN54_V2_SetLut(WF_PARTIAL_1IN54_0);
    EPD_1IN54_V2_SendCommand(0x37);
    EPD_1IN54_V2_SendData(0x00);
    EPD_1IN54_V2_SendData(0x00);
    EPD_1IN54_V2_SendData(0x00);
    EPD_1IN54_V2_SendData(0x00);
    EPD_1IN54_V2_SendData(0x00);
    EPD_1IN54_V2_SendData(0x40);
    EPD_1IN54_V2_SendData(0x00);
    EPD_1IN54_V2_SendData(0x00);
    EPD_1IN54_V2_SendData(0x00);

    EPD_1IN54_V2_SendCommand(0x3C);
    EPD_1IN54_V2_SendData(0x80);

    EPD_1IN54_V2_SendCommand(0x22);
    EPD_1IN54_V2_SendData(0xc0);
    EPD_1IN54_V2_SendCommand(0x20);
    EPD_1IN54_V2_ReadBusy("partial-enter-power");
}

void EPD_1IN54_V2_Clear(void)
{
    EPD_1IN54_V2_ClearAsync();
    EPD_1IN54_V2_WaitBusyWithDelay(EPD_BUSY_BLOCK_DELAY_MS);
}

void EPD_1IN54_V2_ClearAsync(void)
{
    UWORD Width, Height;
    Width = (EPD_1IN54_V2_WIDTH % 8 == 0) ? (EPD_1IN54_V2_WIDTH / 8) : (EPD_1IN54_V2_WIDTH / 8 + 1);
    Height = EPD_1IN54_V2_HEIGHT;

#if EPD_BUSY_TIMING_DEBUG
    Serial.println("[EPD] Clear: sending 0x24 buffer...");
#endif
    EPD_1IN54_V2_SendCommand(0x24);
    for (UWORD j = 0; j < Height; j++) {
        for (UWORD i = 0; i < Width; i++) {
            EPD_1IN54_V2_SendData(0xFF);
        }
    }

#if EPD_BUSY_TIMING_DEBUG
    Serial.println("[EPD] Clear: sending 0x26 buffer...");
#endif
    EPD_1IN54_V2_SendCommand(0x26);
    for (UWORD j = 0; j < Height; j++) {
        for (UWORD i = 0; i < Width; i++) {
            EPD_1IN54_V2_SendData(0xFF);
        }
    }

#if EPD_BUSY_TIMING_DEBUG
    Serial.println("[EPD] Clear: full refresh (~25s, async BUSY)...");
#endif
    EPD_1IN54_V2_TurnOnDisplayBegin();
}

void EPD_1IN54_V2_Display(UBYTE *Image)
{
    EPD_1IN54_V2_DisplayAsync(Image);
    EPD_1IN54_V2_WaitBusyWithDelay(EPD_BUSY_BLOCK_DELAY_MS);
}

void EPD_1IN54_V2_DisplayAsync(UBYTE *Image)
{
    UWORD Width, Height;
    Width = (EPD_1IN54_V2_WIDTH % 8 == 0) ? (EPD_1IN54_V2_WIDTH / 8) : (EPD_1IN54_V2_WIDTH / 8 + 1);
    Height = EPD_1IN54_V2_HEIGHT;

    EPD_1IN54_V2_SetCursor(0, EPD_1IN54_V2_HEIGHT - 1);
    EPD_1IN54_V2_SendCommand(0x24);
    EPD_1IN54_V2_SendDataBuffer(Image, Width * Height);
    EPD_1IN54_V2_TurnOnDisplayBegin();
}

void EPD_1IN54_V2_DisplayPartBaseImage(UBYTE *Image)
{
    EPD_1IN54_V2_DisplayPartBaseImageAsync(Image);
    EPD_1IN54_V2_WaitBusyWithDelay(EPD_BUSY_BLOCK_DELAY_MS);
}

void EPD_1IN54_V2_DisplayPartBaseImageAsync(UBYTE *Image)
{
    UWORD Width, Height;
    Width = (EPD_1IN54_V2_WIDTH % 8 == 0) ? (EPD_1IN54_V2_WIDTH / 8) : (EPD_1IN54_V2_WIDTH / 8 + 1);
    Height = EPD_1IN54_V2_HEIGHT;

    EPD_1IN54_V2_SetCursor(0, EPD_1IN54_V2_HEIGHT - 1);
    EPD_1IN54_V2_SendCommand(0x24);
    EPD_1IN54_V2_SendDataBuffer(Image, Width * Height);
    EPD_1IN54_V2_SendCommand(0x26);
    EPD_1IN54_V2_SendDataBuffer(Image, Width * Height);
    EPD_1IN54_V2_TurnOnDisplayBegin();
}

void EPD_1IN54_V2_DisplayPart(UBYTE *Image)
{
    EPD_1IN54_V2_DisplayPartAsync(Image);
    EPD_1IN54_V2_WaitBusyWithDelay(EPD_BUSY_BLOCK_DELAY_MS);
}

void EPD_1IN54_V2_DisplayPartAsync(UBYTE *Image)
{
    UWORD Width, Height;
    Width = (EPD_1IN54_V2_WIDTH % 8 == 0) ? (EPD_1IN54_V2_WIDTH / 8) : (EPD_1IN54_V2_WIDTH / 8 + 1);
    Height = EPD_1IN54_V2_HEIGHT;

    EPD_1IN54_V2_SetCursor(0, EPD_1IN54_V2_HEIGHT - 1);
    EPD_1IN54_V2_SendCommand(0x24);
    EPD_1IN54_V2_SendDataBuffer(Image, Width * Height);
    EPD_1IN54_V2_TurnOnDisplayPartBegin();
}

void EPD_1IN54_V2_LoadPartOldImage(UBYTE *Image)
{
    UWORD Width, Height;
    Width = (EPD_1IN54_V2_WIDTH % 8 == 0) ? (EPD_1IN54_V2_WIDTH / 8) : (EPD_1IN54_V2_WIDTH / 8 + 1);
    Height = EPD_1IN54_V2_HEIGHT;

    EPD_1IN54_V2_SetCursor(0, EPD_1IN54_V2_HEIGHT - 1);
    EPD_1IN54_V2_SendCommand(0x26);
    EPD_1IN54_V2_SendDataBuffer(Image, Width * Height);
}

void EPD_1IN54_V2_Sleep(void)
{
    EPD_1IN54_V2_SendCommand(0x10);
    EPD_1IN54_V2_SendData(0x01);
    DEV_Delay_ms(100);
}
