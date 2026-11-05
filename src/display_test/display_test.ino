// =============================================================================
// display_test — stand-alone check for the GLOOP ST7789 display wiring
//
// Uses NOTHING from GLOOP or DaisyDuino. Same board settings as gloop_fw.
//
// What you should see:
//   * The Seed's onboard LED blinks (once a second) → the sketch is running.
//   * The screen cycles RED → GREEN → BLUE → WHITE → stripes, ~1 s each.
//
// If the LED blinks but the screen stays blank, set USE_BITBANG to 1 and
// upload again. Bit-bang mode drives SCL/SDA as plain pins, so it doesn't use
// the SPI hardware at all:
//   * works with bit-bang, not with SPI → SPI set-up problem (tell Claude)
//   * blank in both                     → wiring / power / module problem
//
// Wiring (Seed D pins): SCL D8 · SDA D10 · DC D9 · CS D7 · RES D11
//                       VCC + BLK 3V3D (pin 38) · GND DGND (pin 40)
//                       D27 left unconnected (unused SPI MISO)
// =============================================================================
#include <Arduino.h>
#include <SPI.h>

#define USE_BITBANG 0 // 0 = SPI hardware, 1 = drive the pins directly

// Seed D pins (Arduino pin numbers = D numbers on the Seed)
static const uint8_t PIN_SCK  = 8;  // D8  → module SCL
static const uint8_t PIN_MOSI = 10; // D10 → module SDA
static const uint8_t PIN_DC   = 9;  // D9
static const uint8_t PIN_CS   = 7;  // D7
static const uint8_t PIN_RST  = 11; // D11
static const uint8_t PIN_MISO = 27; // D27, unconnected (keeps SPI off D9)

// Panel: 240 x 280 portrait, sits 20 rows into the controller's 320-row RAM
static const uint16_t W = 240, H = 280, ROW_OFFSET = 20;

// ---- Byte output ---------------------------------------------------------------
static void SendByte(uint8_t b)
{
#if USE_BITBANG
    for(int i = 7; i >= 0; i--) // SPI mode 0, MSB first
    {
        digitalWrite(PIN_MOSI, (b >> i) & 1);
        digitalWrite(PIN_SCK, HIGH);
        digitalWrite(PIN_SCK, LOW);
    }
#else
    SPI.transfer(b, SPI_TRANSMITONLY);
#endif
}

static void Command(uint8_t c)
{
    digitalWrite(PIN_CS, LOW);
    digitalWrite(PIN_DC, LOW);
    SendByte(c);
    digitalWrite(PIN_CS, HIGH);
}

static void Data(const uint8_t* d, size_t n)
{
    digitalWrite(PIN_CS, LOW);
    digitalWrite(PIN_DC, HIGH);
    for(size_t i = 0; i < n; i++)
        SendByte(d[i]);
    digitalWrite(PIN_CS, HIGH);
}

static void Data8(uint8_t v)
{
    Data(&v, 1);
}

static void Window(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1)
{
    y0 += ROW_OFFSET;
    y1 += ROW_OFFSET;
    uint8_t ca[4] = {uint8_t(x0 >> 8), uint8_t(x0), uint8_t(x1 >> 8), uint8_t(x1)};
    uint8_t ra[4] = {uint8_t(y0 >> 8), uint8_t(y0), uint8_t(y1 >> 8), uint8_t(y1)};
    Command(0x2A);
    Data(ca, 4);
    Command(0x2B);
    Data(ra, 4);
    Command(0x2C);
}

// Fill the screen; stripes = true draws 8 vertical colour bars
static void Fill(uint16_t colour, bool stripes = false)
{
    static const uint16_t kBars[8]
        = {0xFFFF, 0xFFE0, 0x07FF, 0x07E0, 0xF81F, 0xF800, 0x001F, 0x0000};
    Window(0, 0, W - 1, H - 1);
    digitalWrite(PIN_CS, LOW);
    digitalWrite(PIN_DC, HIGH);
    for(uint16_t y = 0; y < H; y++)
        for(uint16_t x = 0; x < W; x++)
        {
            uint16_t c = stripes ? kBars[x / (W / 8)] : colour;
            SendByte(c >> 8);
            SendByte(c & 0xFF);
        }
    digitalWrite(PIN_CS, HIGH);
}

void setup()
{
    pinMode(LED_BUILTIN, OUTPUT);
    pinMode(PIN_CS, OUTPUT);
    digitalWrite(PIN_CS, HIGH); // deselected until SPI is ready
    pinMode(PIN_RST, OUTPUT);

#if USE_BITBANG
    pinMode(PIN_SCK, OUTPUT);
    pinMode(PIN_MOSI, OUTPUT);
    digitalWrite(PIN_SCK, LOW);
#else
    SPI.setMOSI(PIN_MOSI);
    SPI.setSCLK(PIN_SCK);
    SPI.setMISO(PIN_MISO);
    SPI.begin();
    SPI.beginTransaction(SPISettings(8000000, MSBFIRST, SPI_MODE0)); // slow & safe
#endif
    pinMode(PIN_DC, OUTPUT); // after SPI.begin, so nothing can take it over

    // Hardware reset
    digitalWrite(PIN_RST, HIGH);
    delay(10);
    digitalWrite(PIN_RST, LOW);
    delay(20);
    digitalWrite(PIN_RST, HIGH);
    delay(150);

    Command(0x01); // software reset
    delay(150);
    Command(0x11); // sleep out
    delay(120);
    Command(0x3A); // 16-bit colour
    Data8(0x55);
    Command(0x36); // portrait
    Data8(0x00);
    Command(0x21); // inversion on (IPS)
    Command(0x13); // normal display
    Command(0x29); // display on
    delay(20);
}

void loop()
{
    static const uint16_t kColours[4] = {0xF800, 0x07E0, 0x001F, 0xFFFF}; // R G B W
    static int            step        = 0;

    digitalWrite(LED_BUILTIN, step % 2 ? HIGH : LOW); // heartbeat
    if(step % 5 == 4)
        Fill(0, true);
    else
        Fill(kColours[step % 5]);
    step++;
    delay(700);
}
