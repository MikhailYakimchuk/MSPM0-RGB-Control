
#include "ti_msp_dl_config.h"
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#define RGB_RED_PIN    DL_GPIO_PIN_26
#define RGB_GREEN_PIN  DL_GPIO_PIN_13
#define RGB_BLUE_PIN   DL_GPIO_PIN_27
#define RGB_ALL_PINS   (RGB_RED_PIN | RGB_GREEN_PIN | RGB_BLUE_PIN)

#define ADC_LIGHT_CHANNEL  DL_ADC12_INPUT_CHAN_4
#define ADC_TEMP_CHANNEL   DL_ADC12_INPUT_CHAN_9

#define ADC_ERROR 0xFFFFu
#define CLI_BUFFER_SIZE 48u

typedef struct {
    uint8_t red;
    uint8_t green;
    uint8_t blue;
} RGB;

typedef enum {
    MODE_LIGHT = 1,
    MODE_TEMPERATURE = 2
} LedMode;

static LedMode current_mode = MODE_LIGHT;
static bool rgb_control_active = false;

static RGB current_rgb = {0, 0, 0};

static uint16_t light_raw = 0;
static int32_t temperature_c = 25;

static bool light_valid = false;
static bool temperature_valid = false;

static void uart_write(const char *text)
{
    while (*text != '\0') {
        DL_UART_Main_transmitDataBlocking(UART0, (uint8_t)*text);
        text++;
    }
}

static void uart_write_number(uint32_t value)
{
    char buffer[11];
    unsigned int index = 0;

    do {
        buffer[index++] = (char)('0' + (value % 10u));
        value /= 10u;
    } while (value != 0u);

    while (index > 0u) {
        index--;
        DL_UART_Main_transmitDataBlocking(UART0, (uint8_t)buffer[index]);
    }
}

static void uart_write_signed(int32_t value)
{
    if (value < 0) {
        uart_write("-");
        uart_write_number((uint32_t)(-value));
    } else {
        uart_write_number((uint32_t)value);
    }
}

static void rgb_set(uint8_t red, uint8_t green, uint8_t blue)
{
    current_rgb.red = red;
    current_rgb.green = green;
    current_rgb.blue = blue;
}

static void rgb_pwm_step(void)
{
    static uint8_t phase = 0;
    uint32_t on_pins = 0;

    if (phase < current_rgb.red) {
        on_pins |= RGB_RED_PIN;
    }

    if (phase < current_rgb.green) {
        on_pins |= RGB_GREEN_PIN;
    }

    if (phase < current_rgb.blue) {
        on_pins |= RGB_BLUE_PIN;
    }

    DL_GPIO_setPins(GPIOA, on_pins);
    DL_GPIO_clearPins(GPIOA, RGB_ALL_PINS & ~on_pins);

    phase++;
    delay_cycles(120);
}

static uint16_t adc_read(uint32_t channel)
{
    uint32_t timeout = 50000u;
    uint16_t result;

    DL_ADC12_stopConversion(ADC0);
    DL_ADC12_disableConversions(ADC0);

    DL_ADC12_configConversionMem(
        ADC0,
        DL_ADC12_MEM_IDX_0,
        channel,
        DL_ADC12_REFERENCE_VOLTAGE_VDDA,
        DL_ADC12_SAMPLE_TIMER_SOURCE_SCOMP0,
        DL_ADC12_AVERAGING_MODE_DISABLED,
        DL_ADC12_BURN_OUT_SOURCE_DISABLED,
        DL_ADC12_TRIGGER_MODE_TRIGGER_NEXT,
        DL_ADC12_WINDOWS_COMP_MODE_DISABLED
    );

    DL_ADC12_clearInterruptStatus(
        ADC0, DL_ADC12_INTERRUPT_MEM0_RESULT_LOADED
    );

    DL_ADC12_enableConversions(ADC0);
    DL_ADC12_startConversion(ADC0);

    while ((DL_ADC12_getRawInterruptStatus(
                ADC0, DL_ADC12_INTERRUPT_MEM0_RESULT_LOADED
            ) == 0u) && (timeout > 0u)) {
        timeout--;
    }

    if (timeout == 0u) {
        DL_ADC12_stopConversion(ADC0);
        DL_ADC12_disableConversions(ADC0);
        return ADC_ERROR;
    }

    result = DL_ADC12_getMemResult(ADC0, DL_ADC12_MEM_IDX_0);

    DL_ADC12_stopConversion(ADC0);
    DL_ADC12_disableConversions(ADC0);

    return result;
}

static uint16_t read_light(void)
{
    return adc_read(ADC_LIGHT_CHANNEL);
}

static int32_t read_temperature(void)
{
    uint16_t raw = adc_read(ADC_TEMP_CHANNEL);

    if (raw == ADC_ERROR || raw < 50u || raw > 4040u) {
        return -1000;
    }
    uint32_t resistance =
        (10000u * (uint32_t)raw) / (4095u - raw);

    return 25 + ((int32_t)resistance - 10000) / 64;
}

static void update_sensors(void)
{
    uint16_t value = read_light();

    light_valid = (value != ADC_ERROR);

    if (light_valid) {
        light_raw = value;
    }

    int32_t temp = read_temperature();

    temperature_valid = (temp != -1000);

    if (temperature_valid) {
        temperature_c = temp;
    }
}

static void mode_light(void)
{
    if (!light_valid) {
        rgb_set(255, 0, 255);
        return;
    }
    if (light_raw < 2048u) {
        uint8_t green = (uint8_t)(light_raw / 8u);
        rgb_set(0, green, (uint8_t)(255u - green));
    } else {
        uint8_t red = (uint8_t)((light_raw - 2048u) / 8u);
        rgb_set(red, (uint8_t)(255u - red), 0);
    }
}

static void mode_temperature(void)
{
    if (!temperature_valid) {
        rgb_set(255, 0, 255);
        return;
    }

    if (temperature_c < 20) {
        rgb_set(0, 0, 255);
    } else if (temperature_c < 30) {
        rgb_set(0, 255, 0);
    } else {
        rgb_set(255, 0, 0);
    }
}

static void apply_mode(void)
{
    if (rgb_control_active) {
        return;
    }

    switch (current_mode) {
        case MODE_LIGHT:
            mode_light();
            break;

        case MODE_TEMPERATURE:
            mode_temperature();
            break;

        default:
            current_mode = MODE_LIGHT;
            mode_light();
            break;
    }
}

static void print_sensors(void)
{
    uart_write("Light ADC: ");

    if (light_valid) {
        uart_write_number(light_raw);
        uart_write(" / 4095\r\n");
    } else {
        uart_write("ERROR\r\n");
    }

    uart_write("Temperature approx: ");

    if (temperature_valid) {
        uart_write_signed(temperature_c);
        uart_write(" C\r\n");
    } else {
        uart_write("ERROR\r\n");
    }
}

static void print_status(void)
{
    if (rgb_control_active) {
        uart_write("Control: RGB (UART)\r\n");
    } else {
        uart_write("Mode: ");
        uart_write_number((uint32_t)current_mode);

        if (current_mode == MODE_LIGHT) {
            uart_write(" (light)\r\n");
        } else {
            uart_write(" (temperature)\r\n");
        }
    }

    uart_write("RGB: ");
    uart_write_number(current_rgb.red);
    uart_write(" ");
    uart_write_number(current_rgb.green);
    uart_write(" ");
    uart_write_number(current_rgb.blue);
    uart_write("\r\n");

    print_sensors();
}

static void print_help(void)
{
    uart_write("\r\nCommands:\r\n");
    uart_write("  help          - show commands\r\n");
    uart_write("  status        - show mode and RGB\r\n");
    uart_write("  sensors       - show sensor values\r\n");
    uart_write("  mode 1        - light mode\r\n");
    uart_write("  mode 2        - temperature mode\r\n");
    uart_write("  rgb R G B     - set RGB via UART (0-255)\r\n");
}

static bool parse_number(const char **text, uint16_t *value)
{
    const char *p = *text;
    uint32_t number = 0;
    bool found_digit = false;

    while (*p == ' ') {
        p++;
    }

    while (*p >= '0' && *p <= '9') {
        found_digit = true;
        number = number * 10u + (uint32_t)(*p - '0');

        if (number > 255u) {
            return false;
        }

        p++;
    }

    if (!found_digit || (*p != ' ' && *p != '\0')) {
        return false;
    }

    *value = (uint16_t)number;
    *text = p;

    return true;
}

static bool only_spaces_left(const char *text)
{
    while (*text == ' ') {
        text++;
    }

    return *text == '\0';
}

static void cli_execute(const char *command)
{
    if (strcmp(command, "help") == 0) {
        print_help();
        return;
    }

    if (strcmp(command, "status") == 0) {
        print_status();
        return;
    }

    if (strcmp(command, "sensors") == 0) {
        print_sensors();
        return;
    }

    if (strncmp(command, "mode ", 5) == 0) {
        const char *args = command + 5;
        uint16_t mode;

        if (!parse_number(&args, &mode) ||
            !only_spaces_left(args) ||
            mode < 1u || mode > 2u) {
            uart_write("ERROR: use mode 1 or 2\r\n");
            return;
        }

        current_mode = (LedMode)mode;
        rgb_control_active = false;
        apply_mode();

        uart_write("OK: mode ");
        uart_write_number(mode);
        uart_write("\r\n");
        return;
    }

    if (strncmp(command, "rgb ", 4) == 0) {
        const char *args = command + 4;
        uint16_t r, g, b;

        if (!parse_number(&args, &r) ||
            !parse_number(&args, &g) ||
            !parse_number(&args, &b) ||
            !only_spaces_left(args)) {
            uart_write("ERROR: use rgb R G B (0-255)\r\n");
            return;
        }

        rgb_control_active = true;
        rgb_set((uint8_t)r, (uint8_t)g, (uint8_t)b);

        uart_write("OK: RGB updated\r\n");
        return;
    }

    uart_write("ERROR: unknown command. Type help\r\n");
}

static void uart_poll(void)
{
    static char buffer[CLI_BUFFER_SIZE];
    static unsigned int length = 0;
    static bool overflow = false;

    while (!DL_UART_Main_isRXFIFOEmpty(UART0)) {
        char character = (char)DL_UART_Main_receiveData(UART0);

        if (character == '\r' || character == '\n') {
            if (length != 0u || overflow) {
                uart_write("\r\n");

                if (overflow) {
                    uart_write("ERROR: command too long\r\n");
                } else {
                    buffer[length] = '\0';
                    cli_execute(buffer);
                }

                length = 0;
                overflow = false;
                uart_write("> ");
            }

            continue;
        }

        if (character == '\b' || character == 127) {
            if (length > 0u) {
                length--;
                uart_write("\b \b");
            }
            continue;
        }

        if (character >= 32 && character <= 126) {
            if (length < CLI_BUFFER_SIZE - 1u) {
                buffer[length++] = character;
                DL_UART_Main_transmitDataBlocking(
                    UART0, (uint8_t)character
                );
            } else {
                overflow = true;
            }
        }
    }
}

static void hardware_init(void)
{
    SYSCFG_DL_init();

    DL_GPIO_reset(GPIOA);
    DL_UART_Main_reset(UART0);
    DL_ADC12_reset(ADC0);
    DL_OPA_reset(OPA0);

    DL_GPIO_enablePower(GPIOA);
    DL_UART_Main_enablePower(UART0);
    DL_ADC12_enablePower(ADC0);
    DL_OPA_enablePower(OPA0);

    delay_cycles(1000);

    DL_GPIO_initDigitalOutput(IOMUX_PINCM27); // PA26 red
    DL_GPIO_initDigitalOutput(IOMUX_PINCM14); // PA13 green
    DL_GPIO_initDigitalOutput(IOMUX_PINCM28); // PA27 blue

    DL_GPIO_clearPins(GPIOA, RGB_ALL_PINS);
    DL_GPIO_enableOutput(GPIOA, RGB_ALL_PINS);

    DL_GPIO_initPeripheralOutputFunction(
        IOMUX_PINCM9, IOMUX_PINCM9_PF_UART0_TX
    );

    DL_GPIO_initPeripheralInputFunction(
        IOMUX_PINCM10, IOMUX_PINCM10_PF_UART0_RX
    );

    DL_UART_Main_ClockConfig uart_clock = {
        .clockSel = DL_UART_MAIN_CLOCK_BUSCLK,
        .divideRatio = DL_UART_MAIN_CLOCK_DIVIDE_RATIO_1
    };

    DL_UART_Main_Config uart_config = {
        .mode = DL_UART_MAIN_MODE_NORMAL,
        .direction = DL_UART_MAIN_DIRECTION_TX_RX,
        .flowControl = DL_UART_MAIN_FLOW_CONTROL_NONE,
        .parity = DL_UART_MAIN_PARITY_NONE,
        .wordLength = DL_UART_MAIN_WORD_LENGTH_8_BITS,
        .stopBits = DL_UART_MAIN_STOP_BITS_ONE
    };

    DL_UART_Main_setClockConfig(UART0, &uart_clock);
    DL_UART_Main_init(UART0, &uart_config);

    DL_UART_Main_setOversampling(
        UART0, DL_UART_OVERSAMPLING_RATE_16X
    );
    DL_UART_Main_setBaudRateDivisor(UART0, 17, 23);
    DL_UART_Main_enable(UART0);

    DL_OPA_Config opa_config = {
        .choppingMode = DL_OPA_CHOPPING_MODE_DISABLE,
        .outputPinState = DL_OPA_OUTPUT_PIN_ENABLED,
        .pselChannel = DL_OPA_PSEL_IN0_POS,
        .nselChannel = DL_OPA_NSEL_IN0_NEG,
        .mselChannel = DL_OPA_MSEL_OPEN,
        .gain = DL_OPA_GAIN_N0_P1
    };

    DL_OPA_disableRailToRailInput(OPA0);
    DL_OPA_setGainBandwidth(OPA0, DL_OPA_GBW_HIGH);
    DL_OPA_init(OPA0, &opa_config);
    DL_OPA_enable(OPA0);

    DL_ADC12_ClockConfig adc_clock = {
        .clockSel = DL_ADC12_CLOCK_SYSOSC,
        .divideRatio = DL_ADC12_CLOCK_DIVIDE_4,
        .freqRange = DL_ADC12_CLOCK_FREQ_RANGE_24_TO_32
    };

    DL_ADC12_setClockConfig(ADC0, &adc_clock);

    DL_ADC12_initSingleSample(
        ADC0,
        DL_ADC12_REPEAT_MODE_DISABLED,
        DL_ADC12_SAMPLING_SOURCE_AUTO,
        DL_ADC12_TRIG_SRC_SOFTWARE,
        DL_ADC12_SAMP_CONV_RES_12_BIT,
        DL_ADC12_SAMP_CONV_DATA_FORMAT_UNSIGNED
    );

    DL_ADC12_setSampleTime0(ADC0, 26);
    DL_ADC12_setPowerDownMode(
        ADC0, DL_ADC12_POWER_DOWN_MODE_MANUAL
    );
}

int main(void)
{
    uint32_t sensor_counter = 0;

    hardware_init();

    delay_cycles(100000);
    update_sensors();
    apply_mode();

    uart_write("\r\nRGB Control ready\r\n");
    print_help();
    uart_write("\r\n> ");

    while (1) {
        rgb_pwm_step();
        uart_poll();

        sensor_counter++;

        if (sensor_counter >= 15000u) {
            sensor_counter = 0;
            update_sensors();
            apply_mode();
        }
    }
}
