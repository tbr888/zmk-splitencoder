/*
 * Copyright (c) 2020 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/types.h>
#include <sys/util.h>
#include <init.h>
#include <device.h>

#include <logging/log.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#include <bluetooth/gatt.h>
#include <bluetooth/uuid.h>

#include <drivers/sensor.h>
#include <zmk/matrix.h>
#include <zmk/sensors.h>
#include <zmk/split/bluetooth/uuid.h>
#include <zmk/split/bluetooth/service.h>

#define POS_STATE_LEN 16

static uint8_t num_of_positions = ZMK_KEYMAP_LEN;
static uint8_t position_state[POS_STATE_LEN];

static ssize_t split_svc_pos_state(struct bt_conn *conn, const struct bt_gatt_attr *attrs,
                                   void *buf, uint16_t len, uint16_t offset) {
    return bt_gatt_attr_read(conn, attrs, buf, len, offset, &position_state,
                             sizeof(position_state));
}

static ssize_t split_svc_num_of_positions(struct bt_conn *conn, const struct bt_gatt_attr *attrs,
                                          void *buf, uint16_t len, uint16_t offset) {
    return bt_gatt_attr_read(conn, attrs, buf, len, offset, attrs->user_data, sizeof(uint8_t));
}

static void split_svc_pos_state_ccc(const struct bt_gatt_attr *attr, uint16_t value) {
    LOG_DBG("value %d", value);
}

BT_GATT_SERVICE_DEFINE(
    split_svc, BT_GATT_PRIMARY_SERVICE(BT_UUID_DECLARE_128(ZMK_SPLIT_BT_SERVICE_UUID)),
    BT_GATT_CHARACTERISTIC(BT_UUID_DECLARE_128(ZMK_SPLIT_BT_CHAR_POSITION_STATE_UUID),
                           BT_GATT_CHRC_READ | BT_GATT_CHRC_NOTIFY, BT_GATT_PERM_READ_ENCRYPT,
                           split_svc_pos_state, NULL, &position_state),
    BT_GATT_CCC(split_svc_pos_state_ccc, BT_GATT_PERM_READ_ENCRYPT | BT_GATT_PERM_WRITE_ENCRYPT),
    BT_GATT_DESCRIPTOR(BT_UUID_NUM_OF_DIGITALS, BT_GATT_PERM_READ, split_svc_num_of_positions, NULL,
                       &num_of_positions), );

K_THREAD_STACK_DEFINE(service_q_stack, CONFIG_ZMK_SPLIT_BLE_PERIPHERAL_STACK_SIZE);

struct k_work_q service_work_q;

K_MSGQ_DEFINE(position_state_msgq, sizeof(char[POS_STATE_LEN]),
              CONFIG_ZMK_SPLIT_BLE_PERIPHERAL_POSITION_QUEUE_SIZE, 4);

void send_position_state_callback(struct k_work *work) {
    uint8_t state[POS_STATE_LEN];

    while (k_msgq_get(&position_state_msgq, &state, K_NO_WAIT) == 0) {
        int err = bt_gatt_notify(NULL, &split_svc.attrs[1], &state, sizeof(state));
        if (err) {
            LOG_DBG("Error notifying %d", err);
        }
    }
};

K_WORK_DEFINE(service_position_notify_work, send_position_state_callback);

int send_position_state() {
    int err = k_msgq_put(&position_state_msgq, position_state, K_MSEC(100));
    if (err) {
        // retry...
        switch (err) {
        case -EAGAIN: {
            LOG_WRN("Position state message queue full, popping first message and queueing again");
            uint8_t discarded_state[POS_STATE_LEN];
            k_msgq_get(&position_state_msgq, &discarded_state, K_NO_WAIT);
            return send_position_state();
        }
        default:
            LOG_WRN("Failed to queue position state to send (%d)", err);
            return err;
        }
    }

    k_work_submit_to_queue(&service_work_q, &service_position_notify_work);

    return 0;
}

int zmk_split_bt_position_pressed(uint8_t position) {
    WRITE_BIT(position_state[position / 8], position % 8, true);
    return send_position_state();
}

int zmk_split_bt_position_released(uint8_t position) {
    WRITE_BIT(position_state[position / 8], position % 8, false);
    return send_position_state();
}

#if ZMK_KEYMAP_HAS_SENSORS
#define SENSOR_STATE_LEN 4 // XXX: what to set this too? how many encoders can there be??
static int sensor_state[SENSOR_STATE_LEN]; // 1, 0, or -1 smaller type???

int send_sensor_state() {
    LOG_INF("I would send this");
    int i;
    for (i = 0; i < SENSOR_STATE_LEN; i++) {
        LOG_INF("sensor: %d: %d", i, sensor_state[i]);
    }
}

int zmk_split_bt_sensor_triggered(uint8_t sensor_number, const struct device *sensor) {
    struct sensor_value value;
    int err = sensor_channel_get(sensor, SENSOR_CHAN_ROTATION, &value);
    if (err) {
        LOG_WRN("Failed to get sensor rotation value: %d", err);
        return err;
    }

    if (value.val1 != -1 && value.val1 != 1) {
        // Ignore values that only a fractional rotation part.
        return -ENOTSUP;
    }

    LOG_INF("I would send this %d, %d", sensor_number, value.val1, value.val2);
    sensor_state[sensor_number] = value.val1;
    return send_sensor_state();
}
#endif /* ZMK_KEYMAP_HAS_SENSORS */

int service_init(const struct device *_arg) {
    k_work_q_start(&service_work_q, service_q_stack, K_THREAD_STACK_SIZEOF(service_q_stack),
                   CONFIG_ZMK_SPLIT_BLE_PERIPHERAL_PRIORITY);

    return 0;
}

SYS_INIT(service_init, APPLICATION, CONFIG_ZMK_BLE_INIT_PRIORITY);
