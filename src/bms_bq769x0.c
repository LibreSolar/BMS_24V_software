/* Libre Solar Battery Management System firmware
 * Copyright (c) 2016-2019 Martin Jäger (www.libre.solar)
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "pcb.h"

#if CONFIG_BMS_BQ76920 || CONFIG_BMS_BQ76930 || CONFIG_BMS_BQ76940

#include "bms.h"
#include "bq769x0_registers.h"
#include "bq769x0_interface.h"
#include "helper.h"

#include "config.h"

#include <math.h>       // log for thermistor calculation
#include <stdlib.h>     // for abs() function
#include <stdio.h>
#include <time.h>
#include <string.h>

// static (private) variables
//----------------------------------------------------------------------------

extern int adc_gain;    // factory-calibrated, read out from chip (uV/LSB)
extern int adc_offset;  // factory-calibrated, read out from chip (mV)

//----------------------------------------------------------------------------

/**
 * Checks if temperatures are within the limits, otherwise disables CHG/DSG FET
 *
 * This function is necessary as bq769x0 doesn't support temperature protection
 */
void bms_check_cell_temp(BmsConfig *conf, BmsStatus *status);

//----------------------------------------------------------------------------

void bms_init()
{
    bq769x0_init();
}

void bms_update(BmsConfig *conf, BmsStatus *status)
{
    bms_read_voltages(status);
    bms_read_current(conf, status);
    bms_read_temperatures(conf, status);
    bms_check_cell_temp(conf, status);      // bq769x0 doesn't support temperature settings
    bms_apply_balancing(conf, status);
}

void bms_set_error_flag(BmsStatus *status, uint32_t flag, bool value)
{
    // check if error flag changed
    if ((status->error_flags & (1UL << flag)) != ((uint32_t)value << flag)) {
        if (value) {
            status->error_flags |= (1UL << flag);
            //bms_chg_switch(conf, status, false);
        }
        else {
            status->error_flags &= ~(1UL << flag);
            //bms_chg_switch(conf, status, true);
        }
        #if BMS_DEBUG
        printf("Error flag %u changed to: %d\n", flag, value);
        #endif
    }
}

void bms_check_cell_temp(BmsConfig *conf, BmsStatus *status)
{
    bool chg_overtemp = status->bat_temp_max > conf->chg_ot_limit -
        ((status->error_flags & (1UL << BMS_ERR_CHG_OVERTEMP)) ? conf->t_limit_hyst : 0);

    bool chg_undertemp = status->bat_temp_min < conf->chg_ut_limit +
        ((status->error_flags & (1UL << BMS_ERR_CHG_UNDERTEMP)) ? conf->t_limit_hyst : 0);

    bool dis_overtemp = status->bat_temp_max > conf->dis_ot_limit -
        ((status->error_flags & (1UL << BMS_ERR_DIS_OVERTEMP)) ? conf->t_limit_hyst : 0);

    bool dis_undertemp = status->bat_temp_min < conf->dis_ut_limit +
        ((status->error_flags & (1UL << BMS_ERR_DIS_OVERTEMP)) ? conf->t_limit_hyst : 0);

    if (chg_overtemp != (bool)(status->error_flags & (1UL << BMS_ERR_CHG_OVERTEMP))) {
        bms_set_error_flag(status, BMS_ERR_CHG_OVERTEMP, chg_overtemp);
        bms_chg_switch(conf, status, !chg_overtemp);
    }

    if (chg_undertemp != (bool)(status->error_flags & (1UL << BMS_ERR_CHG_UNDERTEMP))) {
        bms_set_error_flag(status, BMS_ERR_CHG_UNDERTEMP, chg_undertemp);
        bms_chg_switch(conf, status, !chg_undertemp);
    }

    if (dis_overtemp != (bool)(status->error_flags & (1UL << BMS_ERR_DIS_OVERTEMP))) {
        bms_set_error_flag(status, BMS_ERR_DIS_OVERTEMP, dis_overtemp);
        bms_dis_switch(conf, status, !dis_overtemp);
    }

    if (dis_undertemp != (bool)(status->error_flags & (1UL << BMS_ERR_DIS_UNDERTEMP))) {
        bms_set_error_flag(status, BMS_ERR_DIS_UNDERTEMP, dis_undertemp);
        bms_dis_switch(conf, status, !dis_undertemp);
    }
}

void bms_shutdown()
{
    // puts BMS IC into SHIP mode (i.e. switched off)
    bq769x0_write_byte(SYS_CTRL1, 0x0);
    bq769x0_write_byte(SYS_CTRL1, 0x1);
    bq769x0_write_byte(SYS_CTRL1, 0x2);
}

bool bms_chg_switch(BmsConfig *conf, BmsStatus *status, bool enable)
{
    if (enable) {
        if (!bms_chg_error(status))
        {
            int sys_ctrl2;
            sys_ctrl2 = bq769x0_read_byte(SYS_CTRL2);
            bq769x0_write_byte(SYS_CTRL2, sys_ctrl2 | 0b00000001);  // switch CHG on
            #if BMS_DEBUG
            printf("Enabling CHG FET\n");
            #endif
            return true;
        }
        else {
            return false;
        }
    }
    else {
        int sys_ctrl2;
        sys_ctrl2 = bq769x0_read_byte(SYS_CTRL2);
        bq769x0_write_byte(SYS_CTRL2, sys_ctrl2 & ~0b00000001);  // switch CHG off
        #if BMS_DEBUG
        printf("Disabling CHG FET\n");
        #endif
        return true;
    }
}

bool bms_dis_switch(BmsConfig *conf, BmsStatus *status, bool enable)
{
    if (enable) {
        if (!bms_dis_error(status))
        {
            int sys_ctrl2;
            sys_ctrl2 = bq769x0_read_byte(SYS_CTRL2);
            bq769x0_write_byte(SYS_CTRL2, sys_ctrl2 | 0b00000010);  // switch DSG on
            #if BMS_DEBUG
            printf("Enabling DIS FET\n");
            #endif
            return true;
        }
        else {
            return false;
        }
    }
    else {
        int sys_ctrl2;
        sys_ctrl2 = bq769x0_read_byte(SYS_CTRL2);
        bq769x0_write_byte(SYS_CTRL2, sys_ctrl2 & ~0b00000010);  // switch DSG off
        #if BMS_DEBUG
        printf("Disabling DIS FET\n");
        #endif
        return true;
    }
}

void bms_apply_balancing(BmsConfig *conf, BmsStatus *status)
{
    long idleSeconds = uptime() - status->no_idle_timestamp;
    int numberOfSections = NUM_CELLS_MAX/5;

    // check for _timer.read_ms() overflow
    if (idleSeconds < 0) {
        status->no_idle_timestamp = 0;
        idleSeconds = uptime();
    }

    // check if balancing allowed
    if (idleSeconds >= conf->bal_idle_delay &&
        status->cell_voltage_max > conf->bal_cell_voltage_min &&
        (status->cell_voltage_max - status->cell_voltage_min) > conf->bal_cell_voltage_diff)
    {
        //printf("Balancing enabled!");
        status->balancing_status = 0;  // current status will be set in following loop

        //regCELLBAL_t cellbal;
        int balancingFlags;
        int balancingFlagsTarget;

        for (int section = 0; section < numberOfSections; section++)
        {
            // find cells which should be balanced and sort them by voltage descending
            int cellList[5];
            int cellCounter = 0;
            for (int i = 0; i < 5; i++)
            {
                if ((status->cell_voltages[section*5 + i] - status->cell_voltage_min) >
                    conf->bal_cell_voltage_diff)
                {
                    int j = cellCounter;
                    while (j > 0 && status->cell_voltages[section*5 + cellList[j - 1]] <
                        status->cell_voltages[section*5 + i])
                    {
                        cellList[j] = cellList[j - 1];
                        j--;
                    }
                    cellList[j] = i;
                    cellCounter++;
                }
            }

            balancingFlags = 0;
            for (int i = 0; i < cellCounter; i++)
            {
                // try to enable balancing of current cell
                balancingFlagsTarget = balancingFlags | (1 << cellList[i]);

                // check if attempting to balance adjacent cells
                bool adjacentCellCollision =
                    ((balancingFlagsTarget << 1) & balancingFlags) ||
                    ((balancingFlags << 1) & balancingFlagsTarget);

                if (adjacentCellCollision == false) {
                    balancingFlags = balancingFlagsTarget;
                }
            }

            #if BMS_DEBUG
            //printf("Setting CELLBAL%d register to: %s\n", section+1, byte2char(balancingFlags));
            #endif

            status->balancing_status |= balancingFlags << section*5;

            // set balancing register for this section
            bq769x0_write_byte(CELLBAL1+section, balancingFlags);

        } // section loop
    }
    else if (status->balancing_status > 0)
    {
        // clear all CELLBAL registers
        for (int section = 0; section < numberOfSections; section++)
        {
            #if BMS_DEBUG
            printf("Clearing Register CELLBAL%d\n", section+1);
            #endif

            bq769x0_write_byte(CELLBAL1+section, 0x0);
        }

        status->balancing_status = 0;
    }
}

float bms_apply_dis_scp(BmsConfig *conf)
{
    regPROTECT1_t protect1;

    // only RSNS = 1 considered
    protect1.bits.RSNS = 1;

    protect1.bits.SCD_THRESH = 0;
    for (int i = ARRAY_SIZE(SCD_threshold_setting) - 1; i > 0; i--) {
        if (conf->dis_sc_limit * conf->shunt_res_mOhm >= SCD_threshold_setting[i]) {
            protect1.bits.SCD_THRESH = i;
            break;
        }
    }

    protect1.bits.SCD_DELAY = 0;
    for (int i = ARRAY_SIZE(SCD_delay_setting) - 1; i > 0; i--) {
        if (conf->dis_sc_delay_us >= SCD_delay_setting[i]) {
            protect1.bits.SCD_DELAY = i;
            break;
        }
    }

    bq769x0_write_byte(PROTECT1, protect1.regByte);

    // returns the actual current threshold value
    return (long)SCD_threshold_setting[protect1.bits.SCD_THRESH] * 1000 /
        conf->shunt_res_mOhm;
}

float bms_apply_chg_ocp(BmsConfig *conf)
{
    // ToDo: Software protection for charge overcurrent
    return 0;
}

float bms_apply_dis_ocp(BmsConfig *conf)
{
    regPROTECT2_t protect2;

    // Remark: RSNS must be set to 1 in PROTECT1 register

    protect2.bits.OCD_THRESH = 0;
    for (int i = ARRAY_SIZE(OCD_threshold_setting) - 1; i > 0; i--) {
        if (conf->dis_oc_limit * conf->shunt_res_mOhm >= OCD_threshold_setting[i]) {
            protect2.bits.OCD_THRESH = i;
            break;
        }
    }

    protect2.bits.OCD_DELAY = 0;
    for (int i = ARRAY_SIZE(OCD_delay_setting) - 1; i > 0; i--) {
        if (conf->dis_oc_delay_ms >= OCD_delay_setting[i]) {
            protect2.bits.OCD_DELAY = i;
            break;
        }
    }

    bq769x0_write_byte(PROTECT2, protect2.regByte);

    // returns the actual current threshold value
    return (long)OCD_threshold_setting[protect2.bits.OCD_THRESH] * 1000 /
        conf->shunt_res_mOhm;
}

int bms_apply_cell_uvp(BmsConfig *conf)
{
    regPROTECT3_t protect3;
    int uv_trip = 0;

    protect3.regByte = bq769x0_read_byte(PROTECT3);

    uv_trip = ((((long)(conf->cell_uv_limit * 1000) - adc_offset) * 1000 / adc_gain) >> 4) & 0x00FF;
    uv_trip += 1;   // always round up for lower cell voltage
    bq769x0_write_byte(UV_TRIP, uv_trip);

    protect3.bits.UV_DELAY = 0;
    for (int i = ARRAY_SIZE(UV_delay_setting) - 1; i > 0; i--) {
        if (conf->cell_uv_delay_ms >= UV_delay_setting[i]) {
            protect3.bits.UV_DELAY = i;
            break;
        }
    }

    bq769x0_write_byte(PROTECT3, protect3.regByte);

    // returns the actual current threshold value
    return ((long)1 << 12 | uv_trip << 4) * adc_gain / 1000 + adc_offset;
}

//----------------------------------------------------------------------------

int bms_apply_cell_ovp(BmsConfig *conf)
{
    regPROTECT3_t protect3;
    int ov_trip = 0;

    protect3.regByte = bq769x0_read_byte(PROTECT3);

    ov_trip = ((((long)(conf->cell_ov_limit * 1000) - adc_offset) * 1000 / adc_gain) >> 4) & 0x00FF;
    bq769x0_write_byte(OV_TRIP, ov_trip);

    protect3.bits.OV_DELAY = 0;
    for (int i = ARRAY_SIZE(OV_delay_setting) - 1; i > 0; i--) {
        if (conf->cell_ov_delay_ms >= OV_delay_setting[i]) {
            protect3.bits.OV_DELAY = i;
            break;
        }
    }

    bq769x0_write_byte(PROTECT3, protect3.regByte);

    // returns the actual current threshold value
    return ((long)1 << 13 | ov_trip << 4) * adc_gain / 1000 + adc_offset;
}

int bms_apply_temp_limits(BmsConfig *bms)
{
    // bq769x0 don't support temperature limits --> has to be solved in software
    return 0;
}

//----------------------------------------------------------------------------

void bms_read_temperatures(BmsConfig *conf, BmsStatus *status)
{
    float tmp = 0;
    int adc_raw = 0;
    int vtsx = 0;
    unsigned long rts = 0;

    // calculate R_thermistor according to bq769x0 datasheet
    adc_raw = (bq769x0_read_byte(TS1_HI_BYTE) & 0b00111111) << 8 | bq769x0_read_byte(TS1_LO_BYTE);
    vtsx = adc_raw * 0.382; // mV
    rts = 10000.0 * vtsx / (3300.0 - vtsx); // Ohm

    // Temperature calculation using Beta equation
    // - According to bq769x0 datasheet, only 10k thermistors should be used
    // - 25°C reference temperature for Beta equation assumed
    tmp = 1.0/(1.0/(273.15+25) + 1.0/conf->thermistor_beta*log(rts/10000.0)); // K
    status->bat_temps[0] = tmp - 273.15;
    status->bat_temp_min = status->bat_temps[0];
    status->bat_temp_max = status->bat_temps[0];
    int num_temps = 1;
    float sum_temps = status->bat_temps[0];

    if (NUM_THERMISTORS_MAX >= 2) {     // bq76930 or bq76940
        adc_raw = (bq769x0_read_byte(TS2_HI_BYTE) & 0b00111111) << 8 | bq769x0_read_byte(TS2_LO_BYTE);
        vtsx = adc_raw * 0.382; // mV
        rts = 10000.0 * vtsx / (3300.0 - vtsx); // Ohm
        tmp = 1.0/(1.0/(273.15+25) + 1.0/conf->thermistor_beta*log(rts/10000.0)); // K
        status->bat_temps[1] = tmp - 273.15;

        if (status->bat_temps[1] < status->bat_temp_min) {
            status->bat_temp_min = status->bat_temps[1];
        }
        if (status->bat_temps[1] > status->bat_temp_max) {
            status->bat_temp_max = status->bat_temps[1];
        }
        num_temps++;
        sum_temps += status->bat_temps[1];
    }

    if (NUM_THERMISTORS_MAX == 3) {     // bq76940
        adc_raw = (bq769x0_read_byte(TS3_HI_BYTE) & 0b00111111) << 8 | bq769x0_read_byte(TS3_LO_BYTE);
        vtsx = adc_raw * 0.382; // mV
        rts = 10000.0 * vtsx / (3300.0 - vtsx); // Ohm
        tmp = 1.0/(1.0/(273.15+25) + 1.0/conf->thermistor_beta*log(rts/10000.0)); // K
        status->bat_temps[2] = tmp - 273.15;

        if (status->bat_temps[2] < status->bat_temp_min) {
            status->bat_temp_min = status->bat_temps[2];
        }
        if (status->bat_temps[2] > status->bat_temp_max) {
            status->bat_temp_max = status->bat_temps[2];
        }
        num_temps++;
        sum_temps += status->bat_temps[2];
    }
    status->bat_temp_avg = sum_temps / num_temps;
}

void bms_read_current(BmsConfig *conf, BmsStatus *status)
{
    int adc_raw = 0;
    regSYS_STAT_t sys_stat;
    sys_stat.regByte = bq769x0_read_byte(SYS_STAT);

    // check if new current reading available
    if (sys_stat.bits.CC_READY == 1)
    {
        //printf("reading CC register...\n");
        adc_raw = (bq769x0_read_byte(CC_HI_BYTE) << 8) | bq769x0_read_byte(CC_LO_BYTE);
        int32_t pack_current_mA = (int16_t) adc_raw * 8.44 / conf->shunt_res_mOhm;

        status->coulomb_counter_mAs += pack_current_mA / 4;  // is read every 250 ms
        status->soc = status->coulomb_counter_mAs / (conf->nominal_capacity_Ah * 3.6e4F); // %

        // reduce resolution for actual current value
        if (pack_current_mA > -10 && pack_current_mA < 10) {
            pack_current_mA = 0;
        }

        status->pack_current = pack_current_mA / 1000.0;

        // reset no_idle_timestamp
        if (fabs(status->pack_current) > conf->bal_idle_current) {
            status->no_idle_timestamp = uptime();
        }

        // no error occured which caused alert
        if (!(sys_stat.regByte & 0b00111111)) {
            bq769x0_alert_flag_reset();
        }

        bq769x0_write_byte(SYS_STAT, 0b10000000);  // Clear CC ready flag
    }
}

void bms_read_voltages(BmsStatus *status)
{
    int adc_raw = 0;
    int conn_cells = 0;
    float sum_voltages = 0;
    float v_max = 0, v_min = 10;

    for (int i = 0; i < NUM_CELLS_MAX; i++) {
        adc_raw = bq769x0_read_word(VC1_HI_BYTE + i*2) & 0x3FFF;
        status->cell_voltages[i] = (adc_raw * adc_gain * 1e-3F + adc_offset) * 1e-3F;

        if (status->cell_voltages[i] > 0.5F) {
            conn_cells++;
            sum_voltages += status->cell_voltages[i];
        }
        if (status->cell_voltages[i] > v_max) {
            v_max = status->cell_voltages[i];
        }
        if (status->cell_voltages[i] < v_min && status->cell_voltages[i] > 0.5F) {
            v_min = status->cell_voltages[i];
        }
    }
    status->connected_cells = conn_cells;
    status->cell_voltage_avg = sum_voltages / conn_cells;
    status->cell_voltage_min = v_min;
    status->cell_voltage_max = v_max;

    // read battery pack voltage
    adc_raw = bq769x0_read_word(BAT_HI_BYTE);
    status->pack_voltage = (4.0 * adc_gain * adc_raw * 1e-3F + status->connected_cells * adc_offset) * 1e-3F;
}

void bms_update_error_flags(BmsConfig *conf, BmsStatus *status)
{
    regSYS_STAT_t sys_stat;
    sys_stat.regByte = bq769x0_read_byte(SYS_STAT);

    uint32_t error_flags_temp = 0;
    if (sys_stat.bits.UV)      error_flags_temp |= 1U << BMS_ERR_CELL_UNDERVOLTAGE;
    if (sys_stat.bits.OV)      error_flags_temp |= 1U << BMS_ERR_CELL_OVERVOLTAGE;
    if (sys_stat.bits.SCD)     error_flags_temp |= 1U << BMS_ERR_SHORT_CIRCUIT;
    if (sys_stat.bits.OCD)     error_flags_temp |= 1U << BMS_ERR_DIS_OVERCURRENT;

    if (status->pack_current > conf->chg_oc_limit) {
        // ToDo: consider conf->chg_oc_delay
        error_flags_temp |= 1U << BMS_ERR_CHG_OVERCURRENT;
    }

    if (status->bat_temp_max > conf->chg_ot_limit ||
        (status->error_flags & (1UL << BMS_ERR_CHG_OVERTEMP)))
    {
        error_flags_temp |= 1U << BMS_ERR_CHG_OVERTEMP;
    }

    if (status->bat_temp_min < conf->chg_ut_limit ||
        (status->error_flags & (1UL << BMS_ERR_CHG_UNDERTEMP)))
    {
        error_flags_temp |= 1U << BMS_ERR_CHG_UNDERTEMP;
    }

    if (status->bat_temp_max > conf->dis_ot_limit ||
        (status->error_flags & (1UL << BMS_ERR_DIS_OVERTEMP)))
    {
        error_flags_temp |= 1U << BMS_ERR_DIS_OVERTEMP;
    }

    if (status->bat_temp_min < conf->dis_ut_limit ||
        (status->error_flags & (1UL << BMS_ERR_DIS_UNDERTEMP)))
    {
        error_flags_temp |= 1U << BMS_ERR_DIS_UNDERTEMP;
    }
}

void bms_handle_errors(BmsConfig *conf, BmsStatus *status)
{
    static uint16_t error_status = 0;
    static uint32_t sec_since_error = 0;

    // ToDo: Handle also temperature and chg errors (incl. temp hysteresis)

    if (!bq769x0_alert_flag() && error_status == 0) {
        return;
    }
    else {

        regSYS_STAT_t sys_stat;
        sys_stat.regByte = bq769x0_read_byte(SYS_STAT);

        // first check, if only a new CC reading is available
        if (sys_stat.bits.CC_READY == 1) {
            //printf("Interrupt: CC ready");
            bms_read_current(conf, status);  // automatically clears CC ready flag
        }

        // Serious error occured
        if (sys_stat.regByte & 0b00111111)
        {
            if (bq769x0_alert_flag() == true) {
                sec_since_error = 0;
            }
            error_status = sys_stat.regByte;

            unsigned int sec_since_interrupt = uptime() - bq769x0_alert_timestamp();

            if (abs((long)(sec_since_interrupt - sec_since_error)) > 2) {
                sec_since_error = sec_since_interrupt;
            }

            // called only once per second
            if (sec_since_interrupt >= sec_since_error)
            {
                if (sys_stat.regByte & 0b00100000) { // XR error
                    // datasheet recommendation: try to clear after waiting a few seconds
                    if (sec_since_error % 3 == 0) {
                        #if BMS_DEBUG
                        printf("Attempting to clear XR error");
                        #endif
                        bq769x0_write_byte(SYS_STAT, 0b00100000);
                        bms_chg_switch(conf, status, true);
                        bms_dis_switch(conf, status, true);
                    }
                }
                if (sys_stat.regByte & 0b00010000) { // Alert error
                    if (sec_since_error % 10 == 0) {
                        #if BMS_DEBUG
                        printf("Attempting to clear Alert error");
                        #endif
                        bq769x0_write_byte(SYS_STAT, 0b00010000);
                        bms_chg_switch(conf, status, true);
                        bms_dis_switch(conf, status, true);
                    }
                }
                if (sys_stat.regByte & 0b00001000) { // UV error
                    bms_read_voltages(status);
                    if (status->cell_voltage_min > conf->cell_uv_limit) {
                        #if BMS_DEBUG
                        printf("Attempting to clear UV error");
                        #endif
                        bq769x0_write_byte(SYS_STAT, 0b00001000);
                        bms_dis_switch(conf, status, true);
                    }
                }
                if (sys_stat.regByte & 0b00000100) { // OV error
                    bms_read_voltages(status);
                    if (status->cell_voltage_max < conf->cell_ov_limit) {
                        #if BMS_DEBUG
                        printf("Attempting to clear OV error");
                        #endif
                        bq769x0_write_byte(SYS_STAT, 0b00000100);
                        bms_chg_switch(conf, status, true);
                    }
                }
                if (sys_stat.regByte & 0b00000010) { // SCD
                    if (sec_since_error % 60 == 0) {
                        #if BMS_DEBUG
                        printf("Attempting to clear SCD error");
                        #endif
                        bq769x0_write_byte(SYS_STAT, 0b00000010);
                        bms_dis_switch(conf, status, true);
                    }
                }
                if (sys_stat.regByte & 0b00000001) { // OCD
                    if (sec_since_error % 60 == 0) {
                        #if BMS_DEBUG
                        printf("Attempting to clear OCD error");
                        #endif
                        bq769x0_write_byte(SYS_STAT, 0b00000001);
                        bms_dis_switch(conf, status, true);
                    }
                }
                sec_since_error++;
            }
        }
        else {
            error_status = 0;
        }
    }
}

#if BMS_DEBUG

static const char *byte2bitstr(uint8_t b)
{
    static char str[9];
    str[0] = '\0';
    for (int z = 128; z > 0; z >>= 1) {
        strcat(str, ((b & z) == z) ? "1" : "0");
    }
    return str;
}

void bms_print_registers()
{
    printf("0x00 SYS_STAT:  %s\n", byte2bitstr(bq769x0_read_byte(SYS_STAT)));
    printf("0x01 CELLBAL1:  %s\n", byte2bitstr(bq769x0_read_byte(CELLBAL1)));
    printf("0x04 SYS_CTRL1: %s\n", byte2bitstr(bq769x0_read_byte(SYS_CTRL1)));
    printf("0x05 SYS_CTRL2: %s\n", byte2bitstr(bq769x0_read_byte(SYS_CTRL2)));
    printf("0x06 PROTECT1:  %s\n", byte2bitstr(bq769x0_read_byte(PROTECT1)));
    printf("0x07 PROTECT2:  %s\n", byte2bitstr(bq769x0_read_byte(PROTECT2)));
    printf("0x08 PROTECT3:  %s\n", byte2bitstr(bq769x0_read_byte(PROTECT3)));
    printf("0x09 OV_TRIP:   %s\n", byte2bitstr(bq769x0_read_byte(OV_TRIP)));
    printf("0x0A UV_TRIP:   %s\n", byte2bitstr(bq769x0_read_byte(UV_TRIP)));
    printf("0x0B CC_CFG:    %s\n", byte2bitstr(bq769x0_read_byte(CC_CFG)));
    printf("0x32 CC_HI:     %s\n", byte2bitstr(bq769x0_read_byte(CC_HI_BYTE)));
    printf("0x33 CC_LO:     %s\n", byte2bitstr(bq769x0_read_byte(CC_LO_BYTE)));
    /*
    printf("0x50 ADCGAIN1:  %s\n", byte2bitstr(bq769x0_read_byte(ADCGAIN1)));
    printf("0x51 ADCOFFSET: %s\n", byte2bitstr(bq769x0_read_byte(ADCOFFSET)));
    printf("0x59 ADCGAIN2:  %s\n", byte2bitstr(bq769x0_read_byte(ADCGAIN2)));
    */
}

#endif

#endif // defined BQ769x0
