/***********************************************************************************************//**
 * \file mtb_bmi160.c
 *
 * Description: This file contains the functions for interacting with the
 *              motion sensor.
 *
 ***************************************************************************************************
 * \copyright
 * Copyright 2018-2022 Cypress Semiconductor Corporation (an Infineon company) or
 * an affiliate of Cypress Semiconductor Corporation
 *
 * SPDX-License-Identifier: Apache-2.0
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
 **************************************************************************************************/

#include "mtb_bmi160.h"
#include "cyhal_i2c.h"
#include "cyhal_system.h"

#if defined(__cplusplus)
extern "C"
{
#endif

#define I2C_TIMEOUT         10 // 10 msec
#define BMI160_ERROR(x)     \
    (CY_RSLT_CREATE(CY_RSLT_TYPE_ERROR, CY_RSLT_MODULE_BOARD_HARDWARE_BMI160, x))
#define I2C_WRITE_BUFFER_LENGTH   32
#define SOFT_RESET_DELAY_US       300

static cyhal_i2c_t* _bmi160_i2c = NULL;
static cyhal_spi_t* _bmi160_spi = NULL;
static cyhal_gpio_t _bmi160_spi_ssel = NC;

//--------------------------------------------------------------------------------------------------
// _bmi160_i2c_write_bytes
//--------------------------------------------------------------------------------------------------
static int8_t _bmi160_i2c_write_bytes(uint8_t dev_addr, uint8_t reg_addr, uint8_t* data,
                                      uint16_t len)
{
    CY_ASSERT((len + 1) < I2C_WRITE_BUFFER_LENGTH);
    uint8_t buf[I2C_WRITE_BUFFER_LENGTH];
    buf[0] = reg_addr;
    for (uint16_t i=0; i < len; i++)
    {
        buf[i+1] = data[i];
    }

    cy_rslt_t result = cyhal_i2c_master_write(_bmi160_i2c, dev_addr, buf, len+1, I2C_TIMEOUT, true);

    return (CY_RSLT_SUCCESS == result)
        ? BMI160_OK
        : BMI160_E_COM_FAIL;
}


//--------------------------------------------------------------------------------------------------
// _bmi160_i2c_read_bytes
//--------------------------------------------------------------------------------------------------
static int8_t _bmi160_i2c_read_bytes(uint8_t dev_addr, uint8_t reg_addr, uint8_t* data,
                                     uint16_t len)
{
    cy_rslt_t result = cyhal_i2c_master_write(_bmi160_i2c, dev_addr, &reg_addr, 1, I2C_TIMEOUT,
                                              false);

    if (CY_RSLT_SUCCESS == result)
    {
        result = cyhal_i2c_master_read(_bmi160_i2c, dev_addr, data, len, I2C_TIMEOUT, true);
    }

    return (CY_RSLT_SUCCESS == result)
        ? BMI160_OK
        : BMI160_E_COM_FAIL;
}


//--------------------------------------------------------------------------------------------------
// _bmi160_spi_write_bytes
//--------------------------------------------------------------------------------------------------
static int8_t _bmi160_spi_write_bytes(uint8_t dev_addr, uint8_t reg_addr, uint8_t* data,
                                      uint16_t len)
{
    CY_UNUSED_PARAMETER(dev_addr);
    cy_rslt_t result = CY_RSLT_SUCCESS;

    cyhal_gpio_write(_bmi160_spi_ssel, 0);
    result |= cyhal_spi_send(_bmi160_spi, reg_addr);

    for (uint16_t i = 0; i < len; i++)
    {
        result |= cyhal_spi_send(_bmi160_spi, data[i]);
    }
    cyhal_gpio_write(_bmi160_spi_ssel, 1);

    return (CY_RSLT_SUCCESS == result)
        ? BMI160_OK
        : BMI160_E_COM_FAIL;
}


//--------------------------------------------------------------------------------------------------
// _bmi160_spi_read_bytes
//--------------------------------------------------------------------------------------------------
static int8_t _bmi160_spi_read_bytes(uint8_t dev_addr, uint8_t reg_addr, uint8_t* data,
                                     uint16_t len)
{
    CY_UNUSED_PARAMETER(dev_addr);
    cy_rslt_t result = CY_RSLT_SUCCESS;
    uint8_t value = reg_addr | 0x80;

    cyhal_gpio_write(_bmi160_spi_ssel, 0);
    result |= cyhal_spi_send(_bmi160_spi, value);

    for (uint16_t i = 0; i < len; i++)
    {
        uint32_t val;
        result |= cyhal_spi_recv(_bmi160_spi, &val);
        data[i] = (uint8_t)val;
    }
    cyhal_gpio_write(_bmi160_spi_ssel, 1);

    return (CY_RSLT_SUCCESS == result)
        ? BMI160_OK
        : BMI160_E_COM_FAIL;
}


//--------------------------------------------------------------------------------------------------
// delay_wrapper
//--------------------------------------------------------------------------------------------------
static void delay_wrapper(uint32_t ms)
{
    (void)cyhal_system_delay_ms(ms);
}


//--------------------------------------------------------------------------------------------------
// _mtb_bmi160_pins_equal
//--------------------------------------------------------------------------------------------------
static inline bool _mtb_bmi160_pins_equal(_mtb_bmi160_interrupt_pin_t ref_pin, cyhal_gpio_t pin)
{
    #if (CYHAL_API_VERSION >= 2)
    return (ref_pin.pin == pin);
    #else
    return (ref_pin == pin);
    #endif
}


//--------------------------------------------------------------------------------------------------
// _mtb_bmi160_set_pin
//--------------------------------------------------------------------------------------------------
static inline void _mtb_bmi160_set_pin(_mtb_bmi160_interrupt_pin_t* ref_pin, cyhal_gpio_t pin)
{
    #if (CYHAL_API_VERSION >= 2)
    ref_pin->pin = pin;
    #else
    *ref_pin = pin;
    #endif
}


//--------------------------------------------------------------------------------------------------
// _mtb_bmi160_free_pin
//--------------------------------------------------------------------------------------------------
static inline void _mtb_bmi160_free_pin(_mtb_bmi160_interrupt_pin_t ref_pin)
{
    #if (CYHAL_API_VERSION >= 2)
    cyhal_gpio_free(ref_pin.pin);
    #else
    cyhal_gpio_free(ref_pin);
    #endif
}


//--------------------------------------------------------------------------------------------------
// _mtb_bmi160_config_int
//--------------------------------------------------------------------------------------------------
static cy_rslt_t _mtb_bmi160_config_int(_mtb_bmi160_interrupt_pin_t* intpin, cyhal_gpio_t pin,
                                        bool init, uint8_t intr_priority, cyhal_gpio_event_t event,
                                        cyhal_gpio_event_callback_t callback, void* callback_arg)
{
    cy_rslt_t result = CY_RSLT_SUCCESS;

    if (NULL == callback)
    {
        cyhal_gpio_free(pin);
        _mtb_bmi160_set_pin(intpin, NC);
    }
    else
    {
        if (init)
        {
            result = cyhal_gpio_init(pin, CYHAL_GPIO_DIR_INPUT, CYHAL_GPIO_DRIVE_NONE, 0);
        }
        if (CY_RSLT_SUCCESS == result)
        {
            _mtb_bmi160_set_pin(intpin, pin);
            #if (CYHAL_API_VERSION >= 2)
            intpin->callback = callback;
            intpin->callback_arg = callback_arg;
            cyhal_gpio_register_callback(pin, intpin);
            #else
            cyhal_gpio_register_callback(pin, callback, callback_arg);
            #endif
            cyhal_gpio_enable_event(pin, event, intr_priority, 1);
        }
    }

    return result;
}


//--------------------------------------------------------------------------------------------------
// mtb_bmi160_init_i2c
//--------------------------------------------------------------------------------------------------
cy_rslt_t mtb_bmi160_init_i2c(mtb_bmi160_t* obj, cyhal_i2c_t* inst, mtb_bmi160_address_t address)
{
    CY_ASSERT(inst != NULL);
    _bmi160_i2c = inst;

    // Configure the BMI160 structure
    obj->sensor.id        = address;
    obj->sensor.intf      = BMI160_I2C_INTF;
    obj->sensor.read      = (bmi160_read_fptr_t)_bmi160_i2c_read_bytes;
    obj->sensor.write     = (bmi160_write_fptr_t)_bmi160_i2c_write_bytes;
    obj->sensor.delay_ms  = delay_wrapper;
    _mtb_bmi160_set_pin(&(obj->intpin1), NC);
    _mtb_bmi160_set_pin(&(obj->intpin2), NC);

    // Initialize BNI160 sensor
    int8_t status = bmi160_init(&(obj->sensor));

    return (BMI160_OK == status) // BMI160 initialization successful
        ? mtb_bmi160_config_default(obj)
        : BMI160_ERROR(status);
}


//--------------------------------------------------------------------------------------------------
// mtb_bmi160_init_spi
//--------------------------------------------------------------------------------------------------
cy_rslt_t mtb_bmi160_init_spi(mtb_bmi160_t* obj, cyhal_spi_t* inst, cyhal_gpio_t spi_ss)
{
    CY_ASSERT(inst != NULL);
    CY_ASSERT(NC != spi_ss);
    _bmi160_spi = inst;
    _bmi160_spi_ssel = spi_ss;

    /* Configure the BMI160 structure */
    obj->sensor.id        = 0;
    obj->sensor.intf      = BMI160_SPI_INTF;
    obj->sensor.read      = (bmi160_read_fptr_t)_bmi160_spi_read_bytes;
    obj->sensor.write     = (bmi160_write_fptr_t)_bmi160_spi_write_bytes;
    obj->sensor.delay_ms  = delay_wrapper;
    _mtb_bmi160_set_pin(&(obj->intpin1), NC);
    _mtb_bmi160_set_pin(&(obj->intpin2), NC);

    // Initialize BNI160 sensor
    int8_t status = bmi160_init(&(obj->sensor));
    cyhal_system_delay_us(SOFT_RESET_DELAY_US); // per datasheet, delay needed after reset to reboot

    return (BMI160_OK == status) // BMI160 initialization successful
        ? mtb_bmi160_config_default(obj)
        : BMI160_ERROR(status);
}


//--------------------------------------------------------------------------------------------------
// mtb_bmi160_config_default
//--------------------------------------------------------------------------------------------------
cy_rslt_t mtb_bmi160_config_default(mtb_bmi160_t* obj)
{
    // Select the Output data rate, range of accelerometer sensor
    obj->sensor.accel_cfg.odr   = BMI160_ACCEL_ODR_1600HZ;
    obj->sensor.accel_cfg.range = BMI160_ACCEL_RANGE_2G;
    obj->sensor.accel_cfg.bw    = BMI160_ACCEL_BW_NORMAL_AVG4;

    // Select the power mode of accelerometer sensor
    obj->sensor.accel_cfg.power = BMI160_ACCEL_NORMAL_MODE;

    // Select the Output data rate, range of gyroscope sensor
    obj->sensor.gyro_cfg.odr   = BMI160_GYRO_ODR_3200HZ;
    obj->sensor.gyro_cfg.range = BMI160_GYRO_RANGE_2000_DPS;
    obj->sensor.gyro_cfg.bw    = BMI160_GYRO_BW_NORMAL_MODE;

    // Select the power mode of gyroscope sensor
    obj->sensor.gyro_cfg.power = BMI160_GYRO_NORMAL_MODE;

    // Set the sensor configuration
    int8_t status = bmi160_set_sens_conf(&(obj->sensor));

    return (BMI160_OK == status)
        ? CY_RSLT_SUCCESS
        : BMI160_ERROR(status);
}


//--------------------------------------------------------------------------------------------------
// mtb_bmi160_read
//--------------------------------------------------------------------------------------------------
cy_rslt_t mtb_bmi160_read(mtb_bmi160_t* obj, mtb_bmi160_data_t* sensor_data)
{
    // To read both Accel and Gyro data along with time
    int8_t status = bmi160_get_sensor_data((BMI160_ACCEL_SEL | BMI160_GYRO_SEL | BMI160_TIME_SEL),
                                           &(sensor_data->accel), &(sensor_data->gyro),
                                           &(obj->sensor));

    return (BMI160_OK == status)
        ? CY_RSLT_SUCCESS
        : BMI160_ERROR(status);
}


//--------------------------------------------------------------------------------------------------
// mtb_bmi160_get
//--------------------------------------------------------------------------------------------------
struct bmi160_dev* mtb_bmi160_get(mtb_bmi160_t* obj)
{
    return &(obj->sensor);
}


//--------------------------------------------------------------------------------------------------
// mtb_bmi160_selftest
//--------------------------------------------------------------------------------------------------
cy_rslt_t mtb_bmi160_selftest(mtb_bmi160_t* obj)
{
    int8_t status = bmi160_perform_self_test(BMI160_ACCEL_SEL, &(obj->sensor));
    cyhal_system_delay_us(SOFT_RESET_DELAY_US); // per datasheet, delay needed after reset to reboot

    if (status == BMI160_OK)
    {
        status = bmi160_perform_self_test(BMI160_GYRO_SEL, &(obj->sensor));
        cyhal_system_delay_us(SOFT_RESET_DELAY_US); // delay needed after another reset
    }

    return (BMI160_OK == status)
        ? CY_RSLT_SUCCESS
        : BMI160_ERROR(status);
}


//--------------------------------------------------------------------------------------------------
// mtb_bmi160_config_int
//--------------------------------------------------------------------------------------------------
cy_rslt_t mtb_bmi160_config_int(mtb_bmi160_t* obj, struct bmi160_int_settg* intsettings,
                                cyhal_gpio_t pin, uint8_t intr_priority, cyhal_gpio_event_t event,
                                cyhal_gpio_event_callback_t callback, void* callback_arg)
{
    cy_rslt_t result;

    if (_mtb_bmi160_pins_equal(obj->intpin1, pin))
    {
        result = _mtb_bmi160_config_int(&(obj->intpin1), pin, false, intr_priority, event, callback,
                                        callback_arg);
    }
    else if (_mtb_bmi160_pins_equal(obj->intpin2, pin))
    {
        result = _mtb_bmi160_config_int(&(obj->intpin2), pin, false, intr_priority, event, callback,
                                        callback_arg);
    }
    else if (_mtb_bmi160_pins_equal(obj->intpin1, NC))
    {
        result = _mtb_bmi160_config_int(&(obj->intpin1), pin, true, intr_priority, event, callback,
                                        callback_arg);
    }
    else if (_mtb_bmi160_pins_equal(obj->intpin2, NC))
    {
        result = _mtb_bmi160_config_int(&(obj->intpin2), pin, true, intr_priority, event, callback,
                                        callback_arg);
    }
    else
    {
        result = MTB_BMI160_RSLT_ERR_INSUFFICIENT_INT_PINS;
    }

    if (result == CY_RSLT_SUCCESS)
    {
        int8_t status = bmi160_set_int_config(intsettings, &(obj->sensor));
        if (status != BMI160_OK)
        {
            result = BMI160_ERROR(status);
        }
    }

    return result;
}


//--------------------------------------------------------------------------------------------------
// mtb_bmi160_free
//--------------------------------------------------------------------------------------------------
void mtb_bmi160_free(mtb_bmi160_t* obj)
{
    if (!_mtb_bmi160_pins_equal(obj->intpin1, NC))
    {
        _mtb_bmi160_free_pin(obj->intpin1);
    }

    if (!_mtb_bmi160_pins_equal(obj->intpin2, NC))
    {
        _mtb_bmi160_free_pin(obj->intpin2);
    }

    _bmi160_i2c = NULL;
}


#if defined(__cplusplus)
}
#endif
