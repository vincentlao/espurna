/*

POW MODULE
Support for Sonoff POW HLW8012-based power monitor

Copyright (C) 2016-2017 by Xose Pérez <xose dot perez at gmail dot com>

*/

#if ENABLE_POW

#include <HLW8012.h>
#include <Hash.h>
#include <ArduinoJson.h>

#define POW_USE_INTERRUPTS 1

HLW8012 hlw8012;

// -----------------------------------------------------------------------------
// POW
// -----------------------------------------------------------------------------

// When using interrupts we have to call the library entry point
// whenever an interrupt is triggered
void hlw8012_cf1_interrupt() {
    hlw8012.cf1_interrupt();
}

void hlw8012_cf_interrupt() {
    hlw8012.cf_interrupt();
}

void powAttachInterrupts() {
    attachInterrupt(POW_CF1_PIN, hlw8012_cf1_interrupt, CHANGE);
    attachInterrupt(POW_CF_PIN, hlw8012_cf_interrupt, CHANGE);
    DEBUG_MSG("[POW] Enabled\n");
}

void powDettachInterrupts() {
    detachInterrupt(POW_CF1_PIN);
    detachInterrupt(POW_CF_PIN);
    DEBUG_MSG("[POW] Disabled\n");
}

// -----------------------------------------------------------------------------

void powSaveCalibration() {
    setSetting("powPowerMult", hlw8012.getPowerMultiplier());
    setSetting("powCurrentMult", hlw8012.getCurrentMultiplier());
    setSetting("powVoltageMult", hlw8012.getVoltageMultiplier());
}

void powRetrieveCalibration() {
    double value;
    value = getSetting("powPowerMult", 0).toFloat();
    if (value > 0) hlw8012.setPowerMultiplier((int) value);
    value = getSetting("powCurrentMult", 0).toFloat();
    if (value > 0) hlw8012.setCurrentMultiplier((int) value);
    value = getSetting("powVoltageMult", 0).toFloat();
    if (value > 0) hlw8012.setVoltageMultiplier((int) value);
}

void powSetExpectedActivePower(unsigned int power) {
    if (power > 0) {
        hlw8012.expectedActivePower(power);
        powSaveCalibration();
    }
}

void powSetExpectedCurrent(double current) {
    if (current > 0) {
        hlw8012.expectedCurrent(current);
        powSaveCalibration();
    }
}

void powSetExpectedVoltage(unsigned int voltage) {
    if (voltage > 0) {
        hlw8012.expectedVoltage(voltage);
        powSaveCalibration();
    }
}

void powReset() {
    hlw8012.resetMultipliers();
    powSaveCalibration();
}

// -----------------------------------------------------------------------------

unsigned int getActivePower() {
    return hlw8012.getActivePower();
}

unsigned int getApparentPower() {
    return hlw8012.getApparentPower();
}

unsigned int getReactivePower() {
    return hlw8012.getReactivePower();
}

double getCurrent() {
    return hlw8012.getCurrent();
}

unsigned int getVoltage() {
    return hlw8012.getVoltage();
}

unsigned int getPowerFactor() {
    return (int) (100 * hlw8012.getPowerFactor());
}

// -----------------------------------------------------------------------------

void powSetup() {

    // Initialize HLW8012
    // void begin(unsigned char cf_pin, unsigned char cf1_pin, unsigned char sel_pin, unsigned char currentWhen = HIGH, bool use_interrupts = false, unsigned long pulse_timeout = PULSE_TIMEOUT);
    // * cf_pin, cf1_pin and sel_pin are GPIOs to the HLW8012 IC
    // * currentWhen is the value in sel_pin to select current sampling
    // * set use_interrupts to true to use interrupts to monitor pulse widths
    // * leave pulse_timeout to the default value, recommended when using interrupts
    #if POW_USE_INTERRUPTS
        hlw8012.begin(POW_CF_PIN, POW_CF1_PIN, POW_SEL_PIN, POW_SEL_CURRENT, true);
    #else
        hlw8012.begin(POW_CF_PIN, POW_CF1_PIN, POW_SEL_PIN, POW_SEL_CURRENT, false, 1000000);
    #endif

    // These values are used to calculate current, voltage and power factors as per datasheet formula
    // These are the nominal values for the Sonoff POW resistors:
    // * The CURRENT_RESISTOR is the 1milliOhm copper-manganese resistor in series with the main line
    // * The VOLTAGE_RESISTOR_UPSTREAM are the 5 470kOhm resistors in the voltage divider that feeds the V2P pin in the HLW8012
    // * The VOLTAGE_RESISTOR_DOWNSTREAM is the 1kOhm resistor in the voltage divider that feeds the V2P pin in the HLW8012
    hlw8012.setResistors(POW_CURRENT_R, POW_VOLTAGE_R_UP, POW_VOLTAGE_R_DOWN);

    // Retrieve calibration values
    powRetrieveCalibration();

    // Attach interrupts
    #if POW_USE_INTERRUPTS
        powAttachInterrupts();
    #endif

    apiRegister("/api/power", "power", [](char * buffer, size_t len) {
        snprintf(buffer, len, "%d", getActivePower());
    });
    apiRegister("/api/current", "current", [](char * buffer, size_t len) {
        dtostrf(getCurrent(), len-1, 2, buffer);
    });
    apiRegister("/api/voltage", "voltage", [](char * buffer, size_t len) {
        snprintf(buffer, len, "%d", getVoltage());
    });

}

void powLoop() {

    static unsigned long last_update = 0;
    static unsigned char report_count = POW_REPORT_EVERY;

    static unsigned long power_sum = 0;
    static double current_sum = 0;
    static unsigned long voltage_sum = 0;

    if ((millis() - last_update > POW_UPDATE_INTERVAL) || (last_update == 0 )){

        last_update = millis();

        unsigned int power = getActivePower();
        unsigned int voltage = getVoltage();
        double current = getCurrent();
        unsigned int apparent = getApparentPower();
        unsigned int factor = getPowerFactor();
        unsigned int reactive = getReactivePower();

        power_sum += power;
        current_sum += current;
        voltage_sum += voltage;

        DynamicJsonBuffer jsonBuffer;
        JsonObject& root = jsonBuffer.createObject();

        root["powVisible"] = 1;
        root["powActivePower"] = power;
        root["powCurrent"] = current;
        root["powVoltage"] = voltage;
        root["powApparentPower"] = apparent;
        root["powReactivePower"] = reactive;
        root["powPowerFactor"] = factor;

        String output;
        root.printTo(output);
        wsSend(output.c_str());

        if (--report_count == 0) {

            power = power_sum / POW_REPORT_EVERY;
            current = current_sum / POW_REPORT_EVERY;
            voltage = voltage_sum / POW_REPORT_EVERY;
            apparent = current * voltage;
            reactive = (apparent > power) ? sqrt(apparent * apparent - power * power) : 0;
            factor = (apparent > 0) ? 100 * power / apparent : 100;
            if (factor > 100) factor = 100;

            mqttSend(getSetting("powPowerTopic", POW_POWER_TOPIC).c_str(), String(power).c_str());
            mqttSend(getSetting("powCurrentTopic", POW_CURRENT_TOPIC).c_str(), String(current).c_str());
            mqttSend(getSetting("powVoltageTopic", POW_VOLTAGE_TOPIC).c_str(), String(voltage).c_str());
            mqttSend(getSetting("powAPowerTopic", POW_APOWER_TOPIC).c_str(), String(apparent).c_str());
            mqttSend(getSetting("powRPowerTopic", POW_RPOWER_TOPIC).c_str(), String(reactive).c_str());
            mqttSend(getSetting("powPFactorTopic", POW_PFACTOR_TOPIC).c_str(), String(factor).c_str());

            #if ENABLE_DOMOTICZ
                domoticzSend("dczPowIdx", power);
            #endif

            power_sum = current_sum = voltage_sum = 0;
            report_count = POW_REPORT_EVERY;

        }

        // Toggle between current and voltage monitoring
        #if POW_USE_INTERRUPTS == 0
            hlw8012.toggleMode();
        #endif

    }

}

#endif
