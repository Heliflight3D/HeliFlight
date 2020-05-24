/*
 * This file is part of Cleanflight and Betaflight.
 *
 * Cleanflight and Betaflight are free software. You can redistribute
 * this software and/or modify this software under the terms of the
 * GNU General Public License as published by the Free Software
 * Foundation, either version 3 of the License, or (at your option)
 * any later version.
 *
 * Cleanflight and Betaflight are distributed in the hope that they
 * will be useful, but WITHOUT ANY WARRANTY; without even the implied
 * warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this software.
 *
 * If not, see <http://www.gnu.org/licenses/>.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

#include "platform.h"

#if defined(USE_ESC_SENSOR)

#include "build/debug.h"

#include "common/time.h"

#include "config/feature.h"
#include "pg/pg.h"
#include "pg/pg_ids.h"
#include "pg/motor.h"

#include "common/maths.h"
#include "common/utils.h"

#include "drivers/timer.h"
#include "drivers/motor.h"
#include "drivers/dshot.h"
#include "drivers/dshot_dpwm.h"
#include "drivers/serial.h"
#include "drivers/serial_uart.h"

#include "esc_sensor.h"

#include "config/config.h"

#include "flight/mixer.h"

#include "io/serial.h"

/*
KISS ESC TELEMETRY PROTOCOL
---------------------------

One transmission will have 10 times 8-bit bytes sent with 115200 baud and 3.6V.

Byte 0: Temperature
Byte 1: Voltage high byte
Byte 2: Voltage low byte
Byte 3: Current high byte
Byte 4: Current low byte
Byte 5: Consumption high byte
Byte 6: Consumption low byte
Byte 7: Rpm high byte
Byte 8: Rpm low byte
Byte 9: 8-bit CRC

*/

PG_REGISTER_WITH_RESET_TEMPLATE(escSensorConfig_t, escSensorConfig, PG_ESC_SENSOR_CONFIG, 0);

PG_RESET_TEMPLATE(escSensorConfig_t, escSensorConfig,
        .halfDuplex = 0,
        .escSensorProtocol = 0
);

/*
DEBUG INFORMATION
-----------------

set debug_mode = DEBUG_ESC_SENSOR in cli

*/

enum {
    DEBUG_ESC_MOTOR_INDEX = 0,
    DEBUG_ESC_NUM_TIMEOUTS = 1,
    DEBUG_ESC_NUM_CRC_ERRORS = 2,
    DEBUG_ESC_DATA_AGE = 3,
};

typedef enum {
    ESC_SENSOR_FRAME_PENDING = 0,
    ESC_SENSOR_FRAME_COMPLETE = 1,
    ESC_SENSOR_FRAME_FAILED = 2
} escTlmFrameState_t;

typedef enum {
    ESC_SENSOR_TRIGGER_STARTUP = 0,
    ESC_SENSOR_TRIGGER_READY = 1,
    ESC_SENSOR_TRIGGER_PENDING = 2
} escSensorTriggerState_t;

#define ESC_SENSOR_BAUDRATE 115200
#define ESC_BOOTTIME 5000               // 5 seconds
#define ESC_REQUEST_TIMEOUT 100         // 100 ms (data transfer takes only 900us)

#define TELEMETRY_FRAME_SIZE 10
static uint8_t telemetryBuffer[TELEMETRY_FRAME_SIZE] = { 0, };

static volatile uint8_t *buffer;
static volatile uint8_t bufferSize = 0;
static volatile uint8_t bufferPosition = 0;

static serialPort_t *escSensorPort = NULL;

static escSensorData_t escSensorData[MAX_SUPPORTED_MOTORS];

static escSensorTriggerState_t escSensorTriggerState = ESC_SENSOR_TRIGGER_STARTUP;
static uint32_t escTriggerTimestamp;
static uint8_t escSensorMotor = 0;      // motor index

static escSensorData_t combinedEscSensorData;
static bool combinedDataNeedsUpdate = true;

static uint16_t totalTimeoutCount = 0;
static uint16_t totalCrcErrorCount = 0;

void startEscDataRead(uint8_t *frameBuffer, uint8_t frameLength)
{
    buffer = frameBuffer;
    bufferPosition = 0;
    bufferSize = frameLength;
}

uint8_t getNumberEscBytesRead(void)
{
    return bufferPosition;
}

static bool isFrameComplete(void)
{
    return bufferPosition == bufferSize;
}

bool isEscSensorActive(void)
{
    return escSensorPort != NULL;
}

// HF3D:  Added to provide RPM data to rpm filter and spoolup/governor logic when DSHOT RPM or RPM Sensor isn't available.
bool isEscSensorValid(uint8_t motorNumber)
{
    // First check escSensorPort != NULL
    if (escSensorPort == NULL) {
        return 0;
    }
    
    if ( escSensorConfig()->escSensorProtocol == ESC_SENSOR_PROTOCOL_KISS ) {
        // KISS:  Check if dataAge < ESC_BATTERY_AGE_MAX
        if (motorNumber < getMotorCount()) {
            return (escSensorData[motorNumber].dataAge <= ESC_BATTERY_AGE_MAX);
        } else if (motorNumber == ESC_SENSOR_COMBINED) {
            return (combinedEscSensorData.dataAge <= ESC_BATTERY_AGE_MAX);
        } else {
            return 0;
        }
    
    } else if ( escSensorConfig()->escSensorProtocol == ESC_SENSOR_PROTOCOL_HOBBYWINGV4 ) {
        // HWV4:  dataAge < 100 while disarmed  or  dataAge < 11 while armed
        //   Realistically we get these every 40 while disarmed and 4 while motor is spinning (rpm>0)
        uint8_t thisDataAge = 0;
        uint16_t thisRPM = 0;
        
        if (motorNumber < getMotorCount()) {
            thisDataAge = escSensorData[motorNumber].dataAge;
            thisRPM = escSensorData[motorNumber].rpm;
        } else if (motorNumber == ESC_SENSOR_COMBINED) {
            thisDataAge = combinedEscSensorData.dataAge;
            thisRPM = combinedEscSensorData.rpm;
        } else {
            return 0;
        }

        // If last RPM value was > 0 (motor spinning) then we should be receiving telemetry every 50ms
        // HF3D TODO:  This will break if scheduler is changed to increase polling rate of escSensorProcess task beyond 100Hz.
        if (thisRPM > 0 && thisDataAge < 11) {
            return 1;
        } else if (thisRPM == 0 && thisDataAge < 100) {
            return 1;
        }
    
    }
    
    return 0;
}

uint16_t getEscSensorRPM(uint8_t motorNumber)
{
    // We check to see if ESC sensor data is valid elsewhere, and set RPM to zero if not.  
    //   So this will return RPM = 0 if ESC sensor telemetry is not active.
    if (motorNumber < getMotorCount()) {
        return escSensorData[motorNumber].rpm;
    } else {
        return 0;
    }
}
// ------- End of code to provide RPM data to rpm filter and spoolup/governor logic


escSensorData_t *getEscSensorData(uint8_t motorNumber)
{
    if (!featureIsEnabled(FEATURE_ESC_SENSOR)) {
        return NULL;
    }
    
    if ( escSensorConfig()->escSensorProtocol == ESC_SENSOR_PROTOCOL_KISS ) {
        // KISS ESC Telemetry
        if (motorNumber < getMotorCount()) {
            return &escSensorData[motorNumber];
        } else if (motorNumber == ESC_SENSOR_COMBINED) {
            if (combinedDataNeedsUpdate) {
                combinedEscSensorData.dataAge = 0;
                combinedEscSensorData.temperature = 0;
                combinedEscSensorData.voltage = 0;
                combinedEscSensorData.current = 0;
                combinedEscSensorData.consumption = 0;
                combinedEscSensorData.rpm = 0;

                for (int i = 0; i < getMotorCount(); i = i + 1) {
                    combinedEscSensorData.dataAge = MAX(combinedEscSensorData.dataAge, escSensorData[i].dataAge);
                    combinedEscSensorData.temperature = MAX(combinedEscSensorData.temperature, escSensorData[i].temperature);
                    combinedEscSensorData.voltage += escSensorData[i].voltage;
                    combinedEscSensorData.current += escSensorData[i].current;
                    combinedEscSensorData.consumption += escSensorData[i].consumption;
                    combinedEscSensorData.rpm += escSensorData[i].rpm;
                }

                combinedEscSensorData.voltage = combinedEscSensorData.voltage / getMotorCount();
                combinedEscSensorData.rpm = combinedEscSensorData.rpm / getMotorCount();

                combinedDataNeedsUpdate = false;

                DEBUG_SET(DEBUG_ESC_SENSOR, DEBUG_ESC_DATA_AGE, combinedEscSensorData.dataAge);
            }

            return &combinedEscSensorData;
        } else {
            return NULL;
        }

    } else if ( escSensorConfig()->escSensorProtocol == ESC_SENSOR_PROTOCOL_HOBBYWINGV4 ) {
        // Hobbywing V4 Telemetry
        
        if (motorNumber < getMotorCount()) {
            return &escSensorData[0];
        } else if (motorNumber == ESC_SENSOR_COMBINED) {
            DEBUG_SET(DEBUG_ESC_SENSOR, DEBUG_ESC_DATA_AGE, escSensorData[0].dataAge);
            return &escSensorData[0];
        } else {
            return NULL;
        }
    } else {
        return NULL;
    }
        
}

// Receive ISR callback
static void escSensorDataReceive(uint16_t c, void *data)
{
    UNUSED(data);

    // KISS ESC sends some data during startup, ignore this for now (maybe future use)
    // startup data could be firmware version and serialnumber

    if (isFrameComplete()) {
        return;
    }

    buffer[bufferPosition++] = (uint8_t)c;
}

bool escSensorInit(void)
{
    const serialPortConfig_t *portConfig = findSerialPortConfig(FUNCTION_ESC_SENSOR);
    if (!portConfig) {
        return false;
    }

    if ( escSensorConfig()->escSensorProtocol == ESC_SENSOR_PROTOCOL_KISS ) {
        portOptions_e options = SERIAL_NOT_INVERTED  | (escSensorConfig()->halfDuplex ? SERIAL_BIDIR : 0);

        // Initialize serial port
        escSensorPort = openSerialPort(portConfig->identifier, FUNCTION_ESC_SENSOR, escSensorDataReceive, NULL, ESC_SENSOR_BAUDRATE, MODE_RX, options);

        for (int i = 0; i < MAX_SUPPORTED_MOTORS; i = i + 1) {
            escSensorData[i].dataAge = ESC_DATA_INVALID;
        }
    
    } else if ( escSensorConfig()->escSensorProtocol == ESC_SENSOR_PROTOCOL_HOBBYWINGV4 ) {
        /*
         * Hobbywing V4 protocol
         *
         * 19200 baud
         * not inverted
         * no parity
         * 8 Bit
         * 1 Stop Bit
         * Big Endian
         * RX Direction
        */
        // leave halfDuplex = 0 (off) unless you're connecting up to a UART TX pin for some strange reason (normal wiring = RX pin)
        portOptions_e options = (SERIAL_STOPBITS_1 | SERIAL_PARITY_NO | SERIAL_NOT_INVERTED)  | (escSensorConfig()->halfDuplex ? SERIAL_BIDIR : 0);

        // Initialize serial port with no callback.  We will just process the buffer.
        escSensorPort = openSerialPort(portConfig->identifier, FUNCTION_ESC_SENSOR, NULL, NULL, 19200, MODE_RX, options);

        escSensorData[0].dataAge = ESC_DATA_INVALID;
    }
    
    return escSensorPort != NULL;
}

static uint8_t updateCrc8(uint8_t crc, uint8_t crc_seed)
{
    uint8_t crc_u = crc;
    crc_u ^= crc_seed;

    for (int i=0; i<8; i++) {
        crc_u = ( crc_u & 0x80 ) ? 0x7 ^ ( crc_u << 1 ) : ( crc_u << 1 );
    }

    return (crc_u);
}

uint8_t calculateCrc8(const uint8_t *Buf, const uint8_t BufLen)
{
    uint8_t crc = 0;
    for (int i = 0; i < BufLen; i++) {
        crc = updateCrc8(Buf[i], crc);
    }

    return crc;
}

static uint8_t decodeEscFrame(void)
{
    if (!isFrameComplete()) {
        return ESC_SENSOR_FRAME_PENDING;
    }

    // Get CRC8 checksum
    uint16_t chksum = calculateCrc8(telemetryBuffer, TELEMETRY_FRAME_SIZE - 1);
    uint16_t tlmsum = telemetryBuffer[TELEMETRY_FRAME_SIZE - 1];     // last byte contains CRC value
    uint8_t frameStatus;
    if (chksum == tlmsum) {
        escSensorData[escSensorMotor].dataAge = 0;
        escSensorData[escSensorMotor].temperature = telemetryBuffer[0];
        escSensorData[escSensorMotor].voltage = telemetryBuffer[1] << 8 | telemetryBuffer[2];
        escSensorData[escSensorMotor].current = telemetryBuffer[3] << 8 | telemetryBuffer[4];
        escSensorData[escSensorMotor].consumption = telemetryBuffer[5] << 8 | telemetryBuffer[6];
        escSensorData[escSensorMotor].rpm = telemetryBuffer[7] << 8 | telemetryBuffer[8];

        combinedDataNeedsUpdate = true;

        frameStatus = ESC_SENSOR_FRAME_COMPLETE;

        if (escSensorMotor < 4) {
            DEBUG_SET(DEBUG_ESC_SENSOR_RPM, escSensorMotor, calcEscRpm(escSensorData[escSensorMotor].rpm) / 10); // output actual rpm/10 to fit in 16bit signed.
            DEBUG_SET(DEBUG_ESC_SENSOR_TMP, escSensorMotor, escSensorData[escSensorMotor].temperature);
        }
    } else {
        frameStatus = ESC_SENSOR_FRAME_FAILED;
    }

    return frameStatus;
}

static void increaseDataAge(void)
{
    if (escSensorData[escSensorMotor].dataAge < ESC_DATA_INVALID) {
        escSensorData[escSensorMotor].dataAge++;

        combinedDataNeedsUpdate = true;
    }
}

static void selectNextMotor(void)
{
    escSensorMotor++;
    if (escSensorMotor == getMotorCount()) {
        escSensorMotor = 0;
    }
}

// XXX Review ESC sensor under refactored motor handling

uint8_t telemetryData[18] = {0};            // Stores Hobbywing V4 telemetry data during read
uint8_t bytesRead = 0;
uint8_t skipPackets = 0;

bool processHWv4TelemetryStream(uint8_t dataByte) 
{
    // Hobbywing V4 ESC Telemetry Protocol Data parser

    if (skipPackets > 0) {
        // Ignore the data in these ?non-telemetry? packets while throttle = 0
        skipPackets--;
    } else if (bytesRead == 0 && dataByte == (uint8_t) 0x9B) {
        // Start of a potentially valid read
        bytesRead = 1;
    } else if (bytesRead == 1 && dataByte == (uint8_t) 0x9B) {
        // We received two 0x9B in a row at the start of a read.. invalid packet.
        // For the first byte of the packet counter to be 0x9B you would need 10,158,080 data packets.
        //   That's 84,650 seconds at 120Hz tranmission rate.  So not very likely to occur accidentally.
        bytesRead = 0;
        skipPackets = 11;
    } else if (bytesRead > 0) {
        // Store each portion of what looks to be a valid data packet
        telemetryData[bytesRead-1] = dataByte;
        bytesRead++;
        if (bytesRead == 19) {
            bytesRead = 0;
            return 1;  // Successfully finished reading a telemetry packet... to the best of our ability to distinguish one.  Return true.
            DEBUG_SET(DEBUG_ESC_SENSOR, DEBUG_ESC_MOTOR_INDEX, escSensorMotor + 1);
        }
    }
    return 0;      // No complete telemetry packet ready yet
}

float calcTempHW(uint16_t tempRaw)
{
    // HF3D TODO:  Only Hobbywing and God knows if this is right.
    uint16_t tempFunc[26][2] =
        {{0, 1},
         {14, 2},
         {28, 3},
         {58, 5},
         {106, 8},
         {158, 11},
         {234, 15},
         {296, 18},
         {362, 21},
         {408, 23},
         {505, 27},
         {583, 30},
         {664, 33},
         {720, 35},
         {807, 38},
         {897, 41},
         {1021, 45},
         {1150, 49},
         {1315, 54},
         {1855, 70},
         {1978, 74},
         {2239, 82},
         {2387, 87},
         {2472, 90},
         {2656, 97},
         {2705, 99}};
    if (tempRaw > 3828)
        return 0;
    if (tempRaw < 1123)
        return 100;
    tempRaw = 3828 - tempRaw;
    uint8_t i = 0;
    while (i < 26 && tempRaw >= tempFunc[i][0])
    {
        i++;
    }
    return tempFunc[i - 1][1] + (tempFunc[i][1] - tempFunc[i - 1][1]) * (float)(tempRaw - tempFunc[i - 1][0]) / (tempFunc[i][0] - tempFunc[i - 1][0]);
}

float calcCurrHW(uint16_t currentRaw)
{
    if (currentRaw > 28)
    {
        return (float)(currentRaw - 28) / 610;
    }
    else
        return 0;
}

void escSensorProcess(timeUs_t currentTimeUs)
{
    // Variables for Hobbywing V4 telemetry
    static timeMs_t lastProcessTimeMs = 0;
    static float consumption = 0.0f;
    
    // Executed from tasks.c at 100Hz with Low priority
    const timeMs_t currentTimeMs = currentTimeUs / 1000;

    if (!escSensorPort || !motorIsEnabled()) {
        // Motors are enabled in init.c as soon as everything else is initialized.
        return;
        DEBUG_SET(DEBUG_ESC_SENSOR, DEBUG_ESC_MOTOR_INDEX, 5);        
    }
    
    DEBUG_SET(DEBUG_ESC_SENSOR, DEBUG_ESC_MOTOR_INDEX, 0); 

    if ( escSensorConfig()->escSensorProtocol == ESC_SENSOR_PROTOCOL_KISS ) {
        // KISS ESC Telemetry Protocol
        switch (escSensorTriggerState) {
            case ESC_SENSOR_TRIGGER_STARTUP:
                // Wait period of time before requesting telemetry (let the system boot first)
                if (currentTimeMs >= ESC_BOOTTIME) {
                    escSensorTriggerState = ESC_SENSOR_TRIGGER_READY;
                }

                break;
            case ESC_SENSOR_TRIGGER_READY:
                escTriggerTimestamp = currentTimeMs;

                startEscDataRead(telemetryBuffer, TELEMETRY_FRAME_SIZE);
                motorDmaOutput_t * const motor = getMotorDmaOutput(escSensorMotor);
                motor->protocolControl.requestTelemetry = true;
                escSensorTriggerState = ESC_SENSOR_TRIGGER_PENDING;

                DEBUG_SET(DEBUG_ESC_SENSOR, DEBUG_ESC_MOTOR_INDEX, escSensorMotor + 1);

                break;
            case ESC_SENSOR_TRIGGER_PENDING:
                if (currentTimeMs < escTriggerTimestamp + ESC_REQUEST_TIMEOUT) {
                    uint8_t state = decodeEscFrame();
                    switch (state) {
                        case ESC_SENSOR_FRAME_COMPLETE:
                            selectNextMotor();
                            escSensorTriggerState = ESC_SENSOR_TRIGGER_READY;

                            break;
                        case ESC_SENSOR_FRAME_FAILED:
                            increaseDataAge();

                            selectNextMotor();
                            escSensorTriggerState = ESC_SENSOR_TRIGGER_READY;

                            DEBUG_SET(DEBUG_ESC_SENSOR, DEBUG_ESC_NUM_CRC_ERRORS, ++totalCrcErrorCount);
                            break;
                        case ESC_SENSOR_FRAME_PENDING:
                            break;
                    }
                } else {
                    // Move on to next ESC, we'll come back to this one
                    increaseDataAge();

                    selectNextMotor();
                    escSensorTriggerState = ESC_SENSOR_TRIGGER_READY;

                    DEBUG_SET(DEBUG_ESC_SENSOR, DEBUG_ESC_NUM_TIMEOUTS, ++totalTimeoutCount);
                }

                break;
        }
        
    } else if ( escSensorConfig()->escSensorProtocol == ESC_SENSOR_PROTOCOL_HOBBYWINGV4 ) {
        // Hobbywing V4 ESC Telemetry Protocol
        //  Only supports motor 0 for now
        escSensorMotor = 0;

        // Increment data aging so we'll know if we don't get a valid data packet on a ESC sensor read
        escSensorData[escSensorMotor].dataAge++;
        
        // check for any available ESC telemetry bytes in the buffer
        while (serialRxBytesWaiting(escSensorPort)) {

            // and process them one by one to build a telemetryData packet
            if (processHWv4TelemetryStream(serialRead(escSensorPort))) {

                // Thanks goes to:  https://github.com/dgatf/msrc/blob/73444dfd2952e17d36a1d73dfa55a57445cc43aa/README.md
                
                // If this evaluated true then we have a potentially valid Telemetry data frame waiting for us.  Process it.
                // uint32_t packetNumber = (uint32_t)data[0] << 16 | (uint16_t)data[1] << 8 | data[2];
                // HF3D TODO:  Debug log this data, including packet number?  Might be useful to see if we're getting the right data if we up the telemetry process speed in the tasks scheduler.
                //uint16_t thr = (uint16_t)telemetryData[3] << 8 | telemetryData[4]; // 0-1024
                //uint16_t pwm = (uint16_t)telemetryData[5] << 8 | telemetryData[6]; // 0-1024
                float rpm = (uint32_t)telemetryData[7] << 16 | (uint16_t)telemetryData[8] << 8 | telemetryData[9];
                float voltage = (float)((uint16_t)telemetryData[10] << 8 | telemetryData[11]) / 113;
                float current = calcCurrHW((uint16_t)telemetryData[12] << 8 | telemetryData[13]);
                float tempFET = calcTempHW((uint16_t)telemetryData[14] << 8 | telemetryData[15]);
                //float tempBEC = calcTempHW((uint16_t)telemetryData[16] << 8 | telemetryData[17]);

                // Now store these values into our telemetry data array... with averaging??
                //   If we don't do averaging we might as well just throw away all the results except for the last one, lol.
                    // uint8_t dataAge;
                    // int8_t temperature;  // C degrees
                    // int16_t voltage;     // 0.01V
                    // int32_t current;     // 0.01A
                    // int32_t consumption; // mAh
                    // int16_t rpm;         // 100 erpm
                // RPM: 5594.00 Volt: 13.08 Temp1: 33.72 Temp2: 34.35
                escSensorData[escSensorMotor].dataAge = 0;
                escSensorData[escSensorMotor].temperature = tempFET;
                escSensorData[escSensorMotor].voltage = voltage * 100;
                escSensorData[escSensorMotor].current = current * 100;
                escSensorData[escSensorMotor].rpm = rpm / 100;

                // HF3D TODO:  Add a debug_ESC parameter for Hobbywing (Packet #, RPM, FET Temp, BEC Temp)
                // HF3D TODO:  Hopefully we're bringing ESC Voltage and Current into the logs permanently anyway.... and probably should bring ESC Temp in permanently too.
                if (escSensorMotor < 4) {
                    DEBUG_SET(DEBUG_ESC_SENSOR_RPM, escSensorMotor, calcEscRpm(escSensorData[escSensorMotor].rpm) / 10); // output actual rpm/10 to fit in 16bit signed.
                    DEBUG_SET(DEBUG_ESC_SENSOR_TMP, escSensorMotor, escSensorData[escSensorMotor].temperature);
                }
                
                DEBUG_SET(DEBUG_ESC_SENSOR, DEBUG_ESC_NUM_CRC_ERRORS, ++totalCrcErrorCount);

            }
            
            DEBUG_SET(DEBUG_ESC_SENSOR, DEBUG_ESC_NUM_TIMEOUTS, ++totalTimeoutCount);
            
        }
        
        DEBUG_SET(DEBUG_ESC_SENSOR, DEBUG_ESC_DATA_AGE, escSensorData[escSensorMotor].dataAge);
        
        // Accumulate consumption (mAh) as a float since we're updating at 100Hz... even 100A for 10ms is only 0.28 mAh.
        //  Calculate it using the last valid current reading we received
        consumption += (currentTimeMs - lastProcessTimeMs) * (float) escSensorData[escSensorMotor].current * 10.0f / (1000.0f * 3600.0f);
        escSensorData[escSensorMotor].consumption = (int32_t) consumption;
        lastProcessTimeMs = currentTimeMs;
        
    }  // End of HobbyWing V4 Telemetry

    // Check every motor for invalid dataAge to see if we need to zero out our values
    //   Better to do this here at slower loop rate than in the getRPM function that runs at rpmFilter rate!
    for (int i = 0; i < getMotorCount(); i++) {
        if (!isEscSensorValid(i)) {
            // Reset all the data
            escSensorData[i].voltage = 0;
            escSensorData[i].current = 0;
            escSensorData[i].consumption = 0;
            escSensorData[i].rpm = 0;
            combinedEscSensorData.voltage = 0;
            combinedEscSensorData.current = 0;
            combinedEscSensorData.consumption = 0;
            combinedEscSensorData.rpm = 0;
        }        
    }
}

int calcEscRpm(int erpm)
{
    return (erpm * 100) / (motorConfig()->motorPoleCount / 2);
}
#endif
