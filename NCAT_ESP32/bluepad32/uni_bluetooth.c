/*
 * Copyright (C) 2017 BlueKitchen GmbH
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the copyright holders nor the names of
 *    contributors may be used to endorse or promote products derived
 *    from this software without specific prior written permission.
 * 4. Any redistribution, use, or modification is done solely for
 *    personal benefit and not for any commercial purpose or for
 *    monetary gain.
 *
 * THIS SOFTWARE IS PROVIDED BY BLUEKITCHEN GMBH AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL MATTHIAS
 * RINGWALD OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF
 * THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * Please inquire about commercial licensing options at
 * contact@bluekitchen-gmbh.com
 *
 */

/*
 * Copyright (C) 2019 Ricardo Quesada
 * Unijoysticle additions based on the following BlueKitchen's test/example
 * files:
 *   - hid_host_test.c
 *   - hid_device.c
 *   - gap_inquire.c
 *   - hid_device_test.c
 *   - gap_link_keys.c
 */

/* This file controls everything related to Bluetooth, like connections, state,
 * queries, etc. No Bluetooth logic should be placed outside this file. That
 * way, in theory, it should be possible to support USB devices by replacing
 * this file.
 */

#include "uni_bluetooth.h"

#include "../btstack/src/btstack.h"
#include "../btstack/src/btstack_config.h"
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sdkconfig.h"
#include "uni_config.h"
#include "uni_debug.h"
#include "uni_hid_device.h"
#include "uni_hid_parser.h"
#include "uni_platform.h"

#define INQUIRY_INTERVAL 5
#define MAX_ATTRIBUTE_VALUE_SIZE 512  // Apparently PS4 has a 470-bytes report
#define L2CAP_CHANNEL_MTU 128         // PS4 requires a 79-byte packet

// globals
// SDP
static uint8_t attribute_value[MAX_ATTRIBUTE_VALUE_SIZE];
static const unsigned int attribute_value_buffer_size = MAX_ATTRIBUTE_VALUE_SIZE;

// Used to implement connection timeout and reconnect timer
static btstack_timer_source_t connection_timer;

static btstack_packet_callback_registration_t BLUEPAD_hci_event_callback_registration; ///
static btstack_packet_callback_registration_t BLUEPAD_sm_event_callback_registration; ///
static hid_protocol_mode_t protocol_mode = HID_PROTOCOL_MODE_REPORT;

static void BLUEPAD_packet_handler(uint8_t packet_type, uint16_t channel, uint8_t* packet, uint16_t size); ///
static void handle_sdp_hid_query_result(uint8_t packet_type, uint16_t channel, uint8_t* packet, uint16_t size);
static void handle_sdp_pid_query_result(uint8_t packet_type, uint16_t channel, uint8_t* packet, uint16_t size);
static void BLUEPAD_handle_gatt_client_event(uint8_t packet_type, uint16_t channel, uint8_t* packet, uint16_t size); ///
static void continue_remote_names(void);
static bool adv_event_contains_hid_service(const uint8_t* packet);
static void hog_connect(bd_addr_t addr, bd_addr_type_t addr_type);
static void start_scan(void);
static void sdp_query_hid_descriptor(uni_hid_device_t* device);
static void sdp_query_product_id(uni_hid_device_t* device);
static void list_link_keys(void);

static void on_l2cap_channel_closed(uint16_t channel, uint8_t* packet, uint16_t size);
static void on_l2cap_channel_opened(uint16_t channel, uint8_t* packet, uint16_t size);
static void on_l2cap_incoming_connection(uint16_t channel, uint8_t* packet, uint16_t size);
static void on_l2cap_data_packet(uint16_t channel, uint8_t* packet, uint16_t sizel);
static void on_gap_inquiry_result(uint16_t channel, uint8_t* packet, uint16_t size);
static void on_gap_event_advertising_report(uint16_t channel, uint8_t* packet, uint16_t size);
static void on_hci_connection_complete(uint16_t channel, uint8_t* packet, uint16_t size);
static void on_hci_connection_request(uint16_t channel, uint8_t* packet, uint16_t size);
static void fsm_process(uni_hid_device_t* d);
static void l2cap_create_control_connection(uni_hid_device_t* d);
static void l2cap_create_interrupt_connection(uni_hid_device_t* d);

// HID results: HID descriptor, PSM interrupt, PSM control, etc.
static void handle_sdp_hid_query_result(uint8_t packet_type, uint16_t channel, uint8_t* packet, uint16_t size) {
    UNUSED(packet_type);
    UNUSED(channel);
    UNUSED(size);

    des_iterator_t attribute_list_it;
    des_iterator_t additional_des_it;
    uint8_t* des_element;
    uint8_t* element;
    uni_hid_device_t* device;

    device = uni_hid_device_get_sdp_device(NULL /*elapsed time*/);
    if (device == NULL) {
        loge("ERROR: handle_sdp_client_query_result. SDP device = NULL\n");
        return;
    }

    switch (hci_event_packet_get_type(packet)) {
        case SDP_EVENT_QUERY_ATTRIBUTE_VALUE:
            if (sdp_event_query_attribute_byte_get_attribute_length(packet) <= attribute_value_buffer_size) {
                attribute_value[sdp_event_query_attribute_byte_get_data_offset(packet)] =
                    sdp_event_query_attribute_byte_get_data(packet);
                if ((uint16_t)(sdp_event_query_attribute_byte_get_data_offset(packet) + 1) ==
                    sdp_event_query_attribute_byte_get_attribute_length(packet)) {
                    switch (sdp_event_query_attribute_byte_get_attribute_id(packet)) {
                        case BLUETOOTH_ATTRIBUTE_HID_DESCRIPTOR_LIST:
                            for (des_iterator_init(&attribute_list_it, attribute_value);
                                 des_iterator_has_more(&attribute_list_it); des_iterator_next(&attribute_list_it)) {
                                if (des_iterator_get_type(&attribute_list_it) != DE_DES)
                                    continue;
                                des_element = des_iterator_get_element(&attribute_list_it);
                                for (des_iterator_init(&additional_des_it, des_element);
                                     des_iterator_has_more(&additional_des_it); des_iterator_next(&additional_des_it)) {
                                    if (des_iterator_get_type(&additional_des_it) != DE_STRING)
                                        continue;
                                    element = des_iterator_get_element(&additional_des_it);
                                    const uint8_t* descriptor = de_get_string(element);
                                    int descriptor_len = de_get_data_size(element);
                                    logi("SDP HID Descriptor (%d):\n", descriptor_len);
                                    uni_hid_device_set_hid_descriptor(device, descriptor, descriptor_len);
                                    printf_hexdump(descriptor, descriptor_len);
                                }
                            }
                            break;
                        default:
                            break;
                    }
                }
            } else {
                loge(
                    "SDP attribute value buffer size exceeded: available %d, "
                    "required %d\n",
                    attribute_value_buffer_size, sdp_event_query_attribute_byte_get_attribute_length(packet));
            }
            break;
        case SDP_EVENT_QUERY_COMPLETE:
            uni_bt_conn_set_state(&device->conn, UNI_BT_CONN_STATE_SDP_HID_DESCRIPTOR_FETCHED);
            fsm_process(device);
            break;
        default:
            break;
    }
}

// Device ID results: Vendor ID, Product ID, Version, etc...
static void handle_sdp_pid_query_result(uint8_t packet_type, uint16_t channel, uint8_t* packet, uint16_t size) {
    UNUSED(packet_type);
    UNUSED(channel);
    UNUSED(size);

    uni_hid_device_t* device;
    uint16_t id16;

    device = uni_hid_device_get_sdp_device(NULL /*elapsed time*/);
    if (device == NULL) {
        loge("ERROR: handle_sdp_client_query_result. SDP device = NULL\n");
        return;
    }

    switch (hci_event_packet_get_type(packet)) {
        case SDP_EVENT_QUERY_ATTRIBUTE_VALUE:
            if (sdp_event_query_attribute_byte_get_attribute_length(packet) <= attribute_value_buffer_size) {
                attribute_value[sdp_event_query_attribute_byte_get_data_offset(packet)] =
                    sdp_event_query_attribute_byte_get_data(packet);
                if ((uint16_t)(sdp_event_query_attribute_byte_get_data_offset(packet) + 1) ==
                    sdp_event_query_attribute_byte_get_attribute_length(packet)) {
                    switch (sdp_event_query_attribute_byte_get_attribute_id(packet)) {
                        case BLUETOOTH_ATTRIBUTE_VENDOR_ID:
                            if (de_element_get_uint16(attribute_value, &id16))
                                uni_hid_device_set_vendor_id(device, id16);
                            else
                                loge("Error getting vendor id\n");
                            break;

                        case BLUETOOTH_ATTRIBUTE_PRODUCT_ID:
                            if (de_element_get_uint16(attribute_value, &id16))
                                uni_hid_device_set_product_id(device, id16);
                            else
                                loge("Error getting product id\n");
                            break;
                        default:
                            break;
                    }
                }
            } else {
                loge(
                    "SDP attribute value buffer size exceeded: available %d, required "
                    "%d\n",
                    attribute_value_buffer_size, sdp_event_query_attribute_byte_get_attribute_length(packet));
            }
            break;
        case SDP_EVENT_QUERY_COMPLETE:
            logi("Vendor ID: 0x%04x - Product ID: 0x%04x\n", uni_hid_device_get_vendor_id(device),
                 uni_hid_device_get_product_id(device));
            uni_hid_device_guess_controller_type_from_pid_vid(device);
            uni_hid_device_set_sdp_device(NULL);
            uni_bt_conn_set_state(&device->conn, UNI_BT_CONN_STATE_SDP_VENDOR_FETCHED);
            fsm_process(device);
            break;
    }
}

// BLE only
static void BLUEPAD_handle_gatt_client_event(uint8_t packet_type, uint16_t channel, uint8_t* packet, uint16_t size) { ///
    UNUSED(packet_type);
    UNUSED(channel);
    UNUSED(size);

    uint8_t status;

    if (hci_event_packet_get_type(packet) != HCI_EVENT_GATTSERVICE_META) {
        return;
    }

    switch (hci_event_gattservice_meta_get_subevent_code(packet)) {
        case GATTSERVICE_SUBEVENT_HID_SERVICE_CONNECTED:
            status = gattservice_subevent_hid_service_connected_get_status(packet);
            logi("GATTSERVICE_SUBEVENT_HID_SERVICE_CONNECTED, status=0x%02x\n", status);
            switch (status) {
                case ERROR_CODE_SUCCESS:
                    logi("HID service client connected, found %d services\n",
                         gattservice_subevent_hid_service_connected_get_num_instances(packet));

                    // store device as bonded
                    // XXX: todo
                    logi("Ready - please start typing or mousing..\n");
                    break;
                default:
                    loge("HID service client connection failed, err 0x%02x.\n", status);
                    break;
            }
            break;

        case GATTSERVICE_SUBEVENT_HID_REPORT:
            logi("GATTSERVICE_SUBEVENT_HID_REPORT\n");
            printf_hexdump(gattservice_subevent_hid_report_get_report(packet),
                           gattservice_subevent_hid_report_get_report_len(packet));
            break;

        default:
            logi("Unsupported gatt client event: 0x%02x\n", hci_event_gattservice_meta_get_subevent_code(packet));
            break;
    }
}

static void BLUEPAD_packet_handler(uint8_t packet_type, uint16_t channel, uint8_t* packet, uint16_t size) { ///
    static uint8_t bt_ready = 0;

    uint8_t event;
    bd_addr_t event_addr;
    uni_hid_device_t* device;
    uint8_t status;
    hci_con_handle_t con_handle;
    uint16_t hids_cid;

    // Ignore all packet events if BT is not ready, with the exception of the
    // "BT is ready" event.
    if ((!bt_ready) &&
        !((packet_type == HCI_EVENT_PACKET) && (hci_event_packet_get_type(packet) == BTSTACK_EVENT_STATE))) {
        // printf("Ignoring packet. BT not ready yet\n");
        return;
    }

    switch (packet_type) {
        case HCI_EVENT_PACKET:
            event = hci_event_packet_get_type(packet);
            switch (event) {
                case BTSTACK_EVENT_STATE:
                    if (btstack_event_state_get_state(packet) == HCI_STATE_WORKING) {
                        uni_get_platform()->on_init_complete();
                        bt_ready = 1;
                        gap_local_bd_addr(event_addr);
                        logi("BTstack up and running on %s.\n", bd_addr_to_str(event_addr));
                        list_link_keys();
                        start_scan();
                    }
                    break;

                // HCI EVENTS
                case HCI_EVENT_LE_META:  // BLE only
                    // wait for connection complete
                    // XXX: FIXME
                    if (hci_event_le_meta_get_subevent_code(packet) != HCI_SUBEVENT_LE_CONNECTION_COMPLETE)
                        break;
                    btstack_run_loop_remove_timer(&connection_timer);
                    hci_subevent_le_connection_complete_get_peer_address(packet, event_addr);
                    device = uni_hid_device_get_instance_for_address(event_addr);
                    if (!device) {
                        loge("Device not found for addr: %s\n", bd_addr_to_str(event_addr));
                        break;
                    }
                    con_handle = hci_subevent_le_connection_complete_get_connection_handle(packet);
                    // // request security
                    sm_request_pairing(con_handle);
                    uni_hid_device_set_connection_handle(device, con_handle);
                    break;
                case HCI_EVENT_ENCRYPTION_CHANGE:  // BLE only
                    con_handle = hci_event_encryption_change_get_connection_handle(packet);
                    device = uni_hid_device_get_instance_for_connection_handle(con_handle);
                    if (!device) {
                        loge("Device not found for connection handle: 0x%04x\n", con_handle);
                        break;
                    }
                    logi("Connection encrypted: %u\n", hci_event_encryption_change_get_encryption_enabled(packet));
                    if (hci_event_encryption_change_get_encryption_enabled(packet) == 0) {
                        logi("Encryption failed -> abort\n");
                        gap_disconnect(con_handle);
                        break;
                    }
                    // continue - query primary services
                    logi("Search for HID service.\n");
                    status = hids_client_connect(con_handle, BLUEPAD_handle_gatt_client_event, protocol_mode, &hids_cid); ///
                    if (status != ERROR_CODE_SUCCESS) {
                        printf("HID client connection failed, status 0x%02x\n", status);
                    }
                    device->hids_cid = hids_cid;
                    break;
                case HCI_EVENT_COMMAND_COMPLETE: {
                    uint16_t opcode = hci_event_command_complete_get_command_opcode(packet);
                    const uint8_t* param = hci_event_command_complete_get_return_parameters(packet);
                    status = param[0];
                    logi("--> HCI_EVENT_COMMAND_COMPLETE: opcode = 0x%04x - status=%d\n", opcode, status);
                    break;
                }
                case HCI_EVENT_AUTHENTICATION_COMPLETE_EVENT: {
                    status = hci_event_authentication_complete_get_status(packet);
                    uint16_t handle = hci_event_authentication_complete_get_connection_handle(packet);
                    logi(
                        "--> HCI_EVENT_AUTHENTICATION_COMPLETE_EVENT: status=%d, "
                        "handle=0x%04x\n",
                        status, handle);
                    break;
                }
                case HCI_EVENT_PIN_CODE_REQUEST: {
                    // gap_pin_code_response_binary does not copy the data, and data
                    // must be valid until the next hci_send_cmd is called.
                    static bd_addr_t pin_code;
                    bd_addr_t local_addr;
                    logi("--> HCI_EVENT_PIN_CODE_REQUEST\n");
                    // FIXME: Assumes incoming connection from Nintendo Wii using Sync.
                    //
                    // From: https://wiibrew.org/wiki/Wiimote#Bluetooth_Pairing:
                    //  If connecting by holding down the 1+2 buttons, the PIN is the
                    //  bluetooth address of the wiimote backwards, if connecting by
                    //  pressing the "sync" button on the back of the wiimote, then the
                    //  PIN is the bluetooth address of the host backwards.
                    hci_event_pin_code_request_get_bd_addr(packet, event_addr);
                    gap_local_bd_addr(local_addr);
                    reverse_bd_addr(local_addr, pin_code);
                    logi("Using PIN code: \n");
                    printf_hexdump(pin_code, sizeof(pin_code));
                    gap_pin_code_response_binary(event_addr, pin_code, sizeof(pin_code));
                    break;
                }
                case HCI_EVENT_USER_CONFIRMATION_REQUEST:
                    // inform about user confirmation request
                    logi("SSP User Confirmation Request with numeric value '%" PRIu32 "'\n",
                         little_endian_read_32(packet, 8));
                    logi("SSP User Confirmation Auto accept\n");
                    break;
                case HCI_EVENT_HID_META: {
                    logi("UNSUPPORTED ---> HCI_EVENT_HID_META <---\n");
                    uint8_t code = hci_event_hid_meta_get_subevent_code(packet);
                    logi("HCI HID META SUBEVENT: 0x%02x\n", code);
                    break;
                }
                case HCI_EVENT_INQUIRY_RESULT:
                    // logi("--> HCI_EVENT_INQUIRY_RESULT <--\n");
                    break;
                case HCI_EVENT_CONNECTION_REQUEST:
                    logi("--> HCI_EVENT_CONNECTION_REQUEST: link_type = %d <--\n",
                         hci_event_connection_request_get_link_type(packet));
                    on_hci_connection_request(channel, packet, size);
                    break;
                case HCI_EVENT_CONNECTION_COMPLETE:
                    logi("--> HCI_EVENT_CONNECTION_COMPLETE\n");
                    on_hci_connection_complete(channel, packet, size);
                    break;
                case HCI_EVENT_DISCONNECTION_COMPLETE:
                    logi("--> HCI_EVENT_DISCONNECTION_COMPLETE\n");
                    break;
                case HCI_EVENT_LINK_KEY_REQUEST:
                    logi("--> HCI_EVENT_LINK_KEY_REQUEST:\n");
                    break;
                case HCI_EVENT_ROLE_CHANGE:
                    logi("--> HCI_EVENT_ROLE_CHANGE\n");
                    break;
                case HCI_EVENT_SYNCHRONOUS_CONNECTION_COMPLETE:
                    logi("--> HCI_EVENT_SYNCHRONOUS_CONNECTION_COMPLETE\n");
                    break;
                case HCI_EVENT_INQUIRY_RESULT_WITH_RSSI:
                    // logi("--> HCI_EVENT_INQUIRY_RESULT_WITH_RSSI <--\n");
                    break;
                case HCI_EVENT_EXTENDED_INQUIRY_RESPONSE:
                    // logi("--> HCI_EVENT_EXTENDED_INQUIRY_RESPONSE <--\n");
                    break;
                case HCI_EVENT_REMOTE_NAME_REQUEST_COMPLETE:
                    logi("--> HCI_EVENT_REMOTE_NAME_REQUEST_COMPLETE\n");
                    reverse_bd_addr(&packet[3], event_addr);
                    device = uni_hid_device_get_instance_for_address(event_addr);
                    if (device != NULL) {
                        if (packet[2] == 0) {
                            logi("Name: '%s'\n", &packet[9]);
                            uni_hid_device_set_name(device, &packet[9], strlen((const char*)&packet[9]));
                            fsm_process(device);
                        } else {
                            logi("Failed to get name: page timeout\n");
                        }
                    }
                    continue_remote_names();
                    break;
                // L2CAP EVENTS
                case L2CAP_EVENT_CAN_SEND_NOW:
                {
                    logd("--> L2CAP_EVENT_CAN_SEND_NOW\n");
                    uint16_t local_cid = l2cap_event_can_send_now_get_local_cid(packet);
                    device = uni_hid_device_get_instance_for_cid(local_cid);
                    if (device == NULL) {
                        loge("--->>> CANNOT FIND DEVICE");
                    } else {
                        uni_hid_device_send_queued_reports(device);
                    }
                    break;
                }    
                case L2CAP_EVENT_INCOMING_CONNECTION:
                    logi("--> L2CAP_EVENT_INCOMING_CONNECTION\n");
                    on_l2cap_incoming_connection(channel, packet, size);
                    break;
                case L2CAP_EVENT_CHANNEL_OPENED:
                    on_l2cap_channel_opened(channel, packet, size);
                    break;
                case L2CAP_EVENT_CHANNEL_CLOSED:
                    on_l2cap_channel_closed(channel, packet, size);
                    break;

                // GAP EVENTS
                case GAP_EVENT_INQUIRY_RESULT:
                    // logi("--> GAP_EVENT_INQUIRY_RESULT\n");
                    on_gap_inquiry_result(channel, packet, size);
                    break;
                case GAP_EVENT_INQUIRY_COMPLETE:
                    logi("--> GAP_EVENT_INQUIRY_COMPLETE\n");
                    uni_hid_device_request_inquire();
                    continue_remote_names();
                    break;
                case GAP_EVENT_ADVERTISING_REPORT:  // BLE only
                    on_gap_event_advertising_report(channel, packet, size);
                    break;
                default:
                    break;
            }
            break;
        case L2CAP_DATA_PACKET:
            on_l2cap_data_packet(channel, packet, size);
            break;
        default:
            break;
    }
}

/* HCI packet handler
 *
 * text The SM packet handler receives Security Manager Events required for
 * pairing. It also receives events generated during Identity Resolving see
 * Listing SMPacketHandler.
 */
static void sm_packet_handler(uint8_t packet_type, uint16_t channel, uint8_t* packet, uint16_t size) {
    UNUSED(channel);
    UNUSED(size);

    if (packet_type != HCI_EVENT_PACKET)
        return;

    switch (hci_event_packet_get_type(packet)) {
        case SM_EVENT_JUST_WORKS_REQUEST:
            logi("Just works requested\n");
            sm_just_works_confirm(sm_event_just_works_request_get_handle(packet));
            break;
        case SM_EVENT_NUMERIC_COMPARISON_REQUEST:
            logi("Confirming numeric comparison: %" PRIu32 "\n",
                 sm_event_numeric_comparison_request_get_passkey(packet));
            sm_numeric_comparison_confirm(sm_event_passkey_display_number_get_handle(packet));
            break;
        case SM_EVENT_PASSKEY_DISPLAY_NUMBER:
            logi("Display Passkey: %" PRIu32 "\n", sm_event_passkey_display_number_get_passkey(packet));
            break;
        case SM_EVENT_PAIRING_COMPLETE:
            switch (sm_event_pairing_complete_get_status(packet)) {
                case ERROR_CODE_SUCCESS:
                    logi("Pairing complete, success\n");
                    break;
                case ERROR_CODE_CONNECTION_TIMEOUT:
                    logi("Pairing failed, timeout\n");
                    break;
                case ERROR_CODE_REMOTE_USER_TERMINATED_CONNECTION:
                    logi("Pairing faileed, disconnected\n");
                    break;
                case ERROR_CODE_AUTHENTICATION_FAILURE:
                    logi("Pairing failed, reason = %u\n", sm_event_pairing_complete_get_reason(packet));
                    break;
                default:
                    break;
            }
            break;
        default:
            break;
    }
}

static void on_hci_connection_request(uint16_t channel, uint8_t* packet, uint16_t size) {
    bd_addr_t event_addr;
    uint32_t cod;
    uni_hid_device_t* device;

    UNUSED(channel);
    UNUSED(size);

    hci_event_connection_request_get_bd_addr(packet, event_addr);
    cod = hci_event_connection_request_get_class_of_device(packet);

    device = uni_hid_device_get_instance_for_address(event_addr);
    if (device == NULL) {
        device = uni_hid_device_create(event_addr);
        if (device == NULL) {
            logi("Cannot create new device... no more slots available\n");
            return;
        }
    }
    uni_hid_device_set_cod(device, cod);
    uni_hid_device_set_incoming(device, 1);
    logi("on_hci_connection_request from: address = %s, cod=0x%04x\n", bd_addr_to_str(event_addr), cod);
}

static void on_hci_connection_complete(uint16_t channel, uint8_t* packet, uint16_t size) {
    bd_addr_t event_addr;
    uni_hid_device_t* d;
    hci_con_handle_t handle;
    uint8_t status;

    UNUSED(channel);
    UNUSED(size);

    hci_event_connection_complete_get_bd_addr(packet, event_addr);
    status = hci_event_connection_complete_get_status(packet);
    if (status) {
        logi("on_hci_connection_complete failed (0x%02x) for %s\n", status, bd_addr_to_str(event_addr));
        return;
    }

    d = uni_hid_device_get_instance_for_address(event_addr);
    if (d == NULL) {
        logi("on_hci_connection_complete: failed to get device for %s\n", bd_addr_to_str(event_addr));
        return;
    }

    handle = hci_event_connection_complete_get_connection_handle(packet);
    uni_hid_device_set_connection_handle(d, handle);

    // if (uni_hid_device_is_incoming(d)) {
    //   hci_send_cmd(&hci_authentication_requested, handle);
    // }

    int cod = d->cod;
    bool is_keyboard = ((cod & MASK_COD_MAJOR_PERIPHERAL) == MASK_COD_MAJOR_PERIPHERAL) &&
                       ((cod & MASK_COD_MINOR_MASK) & MASK_COD_MINOR_KEYBOARD);
    if (is_keyboard) {
        // gap_request_security_level(handle, LEVEL_1);
    }
}

static void on_gap_inquiry_result(uint16_t channel, uint8_t* packet, uint16_t size) {
    const int NAME_LEN_MAX = 240;
    bd_addr_t addr;
    uni_hid_device_t* device;
    uint8_t name_buffer[NAME_LEN_MAX];
    int name_len = -1;

    UNUSED(channel);
    UNUSED(size);

    gap_event_inquiry_result_get_bd_addr(packet, addr);
    uint8_t page_scan_repetition_mode = gap_event_inquiry_result_get_page_scan_repetition_mode(packet);
    uint16_t clock_offset = gap_event_inquiry_result_get_clock_offset(packet);
    uint32_t cod = gap_event_inquiry_result_get_class_of_device(packet);

    logi("Device found: %s ", bd_addr_to_str(addr));
    logi("with COD: 0x%06x, ", (unsigned int)cod);
    logi("pageScan %d, ", page_scan_repetition_mode);
    logi("clock offset 0x%04x", clock_offset);
    if (gap_event_inquiry_result_get_rssi_available(packet)) {
        logi(", rssi %d dBm", (int8_t)gap_event_inquiry_result_get_rssi(packet));
    }
    if (gap_event_inquiry_result_get_name_available(packet)) {
        name_len = btstack_min(NAME_LEN_MAX - 1, gap_event_inquiry_result_get_name_len(packet));
        memcpy(name_buffer, gap_event_inquiry_result_get_name(packet), name_len);
        name_buffer[name_len] = 0;
        logi(", name '%s'", name_buffer);
    }

    if (uni_hid_device_is_cod_supported(cod)) {
        device = uni_hid_device_get_instance_for_address(addr);
        if (device != NULL && !uni_hid_device_is_orphan(device)) {
            logi("... device already added (state=%d)\n", uni_bt_conn_get_state(&device->conn));
            uni_hid_device_dump_device(device);
            bool deleted = uni_hid_device_auto_delete(device);
            if (!deleted)
                return;
            // If it was not deleted, fallthrough, and let it create it again.
        }
        if (!device) {
            device = uni_hid_device_create(addr);
            if (device == NULL) {
                loge("\nError: no more available device slots\n");
                return;
            }
            uni_bt_conn_set_state(&device->conn, UNI_BT_CONN_STATE_DEVICE_DISCOVERED);
            uni_hid_device_set_cod(device, cod);
            device->conn.page_scan_repetition_mode = page_scan_repetition_mode;
            device->conn.clock_offset = clock_offset;
            if (name_len != -1 && !uni_hid_device_has_name(device)) {
                uni_hid_device_set_name(device, name_buffer, name_len);
            }
        }
        logi("\n");
        fsm_process(device);
    } else {
        logi("\n");
    }
}

// BLE only
static void on_gap_event_advertising_report(uint16_t channel, uint8_t* packet, uint16_t size) {
    bd_addr_t addr;
    bd_addr_type_t addr_type;

    UNUSED(channel);
    UNUSED(size);

    if (adv_event_contains_hid_service(packet) == false)
        return;

    // store remote device address and type
    gap_event_advertising_report_get_address(packet, addr);
    addr_type = (bd_addr_type_t)gap_event_advertising_report_get_address_type(packet); ///
    // connect
    logi("Found, connect to device with %s address %s ...\n", addr_type == 0 ? "public" : "random",
         bd_addr_to_str(addr));
    hog_connect(addr, addr_type);
}

static void on_l2cap_channel_opened(uint16_t channel, uint8_t* packet, uint16_t size) {
    uint16_t psm;
    uint8_t status;
    uint16_t local_cid, remote_cid;
    uint16_t local_mtu, remote_mtu;
    hci_con_handle_t handle;
    bd_addr_t address;
    uni_hid_device_t* device;
    uint8_t incoming;

    UNUSED(size);

    logi("L2CAP_EVENT_CHANNEL_OPENED (channel=0x%04x)\n", channel);

    l2cap_event_channel_opened_get_address(packet, address);
    status = l2cap_event_channel_opened_get_status(packet);
    if (status) {
        logi("L2CAP Connection failed: 0x%02x.\n", status);
        // Practice showed that if any of these two status are received, it is
        // best to remove the link key. But this is based on empirical evidence,
        // not on theory.
        if (status == L2CAP_CONNECTION_RESPONSE_RESULT_RTX_TIMEOUT || status == L2CAP_CONNECTION_BASEBAND_DISCONNECT) {
            logi("Removing previous link key for address=%s.\n", bd_addr_to_str(address));
            uni_hid_device_remove_entry_with_channel(channel);
            // Just in case the key is outdated we remove it. If fixes some
            // l2cap_channel_opened issues. It proves that it works when the status
            // is 0x6a (L2CAP_CONNECTION_BASEBAND_DISCONNECT).
            gap_drop_link_key_for_bd_addr(address);
        }
        return;
    }
    psm = l2cap_event_channel_opened_get_psm(packet);
    local_cid = l2cap_event_channel_opened_get_local_cid(packet);
    remote_cid = l2cap_event_channel_opened_get_remote_cid(packet);
    handle = l2cap_event_channel_opened_get_handle(packet);
    incoming = l2cap_event_channel_opened_get_incoming(packet);
    local_mtu = l2cap_event_channel_opened_get_local_mtu(packet);
    remote_mtu = l2cap_event_channel_opened_get_remote_mtu(packet);

    logi(
        "PSM: 0x%04x, local CID=0x%04x, remote CID=0x%04x, handle=0x%04x, "
        "incoming=%d, local MTU=%d, remote MTU=%d\n",
        psm, local_cid, remote_cid, handle, incoming, local_mtu, remote_mtu);

    device = uni_hid_device_get_instance_for_address(address);
    if (device == NULL) {
        loge("could not find device for address\n");
        uni_hid_device_remove_entry_with_channel(channel);
        return;
    }

    uni_hid_device_set_connected(device, true);

    switch (psm) {
        case PSM_HID_CONTROL:
            device->conn.control_cid = l2cap_event_channel_opened_get_local_cid(packet);
            logi("HID Control opened, cid 0x%02x\n", device->conn.control_cid);
            uni_bt_conn_set_state(&device->conn, UNI_BT_CONN_STATE_L2CAP_CONTROL_CONNECTED);
            break;
        case PSM_HID_INTERRUPT:
            device->conn.interrupt_cid = l2cap_event_channel_opened_get_local_cid(packet);
            logi("HID Interrupt opened, cid 0x%02x\n", device->conn.interrupt_cid);
            uni_bt_conn_set_state(&device->conn, UNI_BT_CONN_STATE_L2CAP_INTERRUPT_CONNECTED);
            break;
        default:
            break;
    }
    fsm_process(device);
}

static void on_l2cap_channel_closed(uint16_t channel, uint8_t* packet, uint16_t size) {
    uint16_t local_cid;
    uni_hid_device_t* device;

    UNUSED(size);

    local_cid = l2cap_event_channel_closed_get_local_cid(packet);
    logi("L2CAP_EVENT_CHANNEL_CLOSED: 0x%04x (channel=0x%04x)\n", local_cid, channel);
    device = uni_hid_device_get_instance_for_cid(local_cid);
    if (device == NULL) {
        // Device might already been closed if the Control or Interrupt PSM was closed first.
        logi("Couldn't not find hid_device for cid = 0x%04x\n", local_cid);
        return;
    }
    uni_hid_device_set_connected(device, false);
}

static void on_l2cap_incoming_connection(uint16_t channel, uint8_t* packet, uint16_t size) {
    bd_addr_t event_addr;
    uni_hid_device_t* device;
    uint16_t local_cid, remote_cid;
    uint16_t psm;
    hci_con_handle_t handle;

    UNUSED(size);

    psm = l2cap_event_incoming_connection_get_psm(packet);
    handle = l2cap_event_incoming_connection_get_handle(packet);
    local_cid = l2cap_event_incoming_connection_get_local_cid(packet);
    remote_cid = l2cap_event_incoming_connection_get_remote_cid(packet);

    logi(
        "L2CAP_EVENT_INCOMING_CONNECTION (psm=0x%04x, local_cid=0x%04x, "
        "remote_cid=0x%04x, handle=0x%04x, "
        "channel=0x%04x\n",
        psm, local_cid, remote_cid, handle, channel);
    switch (psm) {
        case PSM_HID_CONTROL:
            l2cap_event_incoming_connection_get_address(packet, event_addr);
            device = uni_hid_device_get_instance_for_address(event_addr);
            if (device == NULL) {
                device = uni_hid_device_create(event_addr);
                if (device == NULL) {
                    loge("ERROR: no more available free devices\n");
                    l2cap_decline_connection(channel);
                    break;
                }
            }
            l2cap_accept_connection(channel);
            uni_hid_device_set_connection_handle(device, handle);
            device->conn.control_cid = channel;
            uni_hid_device_set_incoming(device, 1);
            break;
        case PSM_HID_INTERRUPT:
            l2cap_event_incoming_connection_get_address(packet, event_addr);
            device = uni_hid_device_get_instance_for_address(event_addr);
            if (device == NULL) {
                loge("Could not find device for PSM_HID_INTERRUPT = 0x%04x\n", channel);
                l2cap_decline_connection(channel);
                break;
            }
            device->conn.interrupt_cid = channel;
            l2cap_accept_connection(channel);
            break;
        default:
            logi("Unknown PSM = 0x%02x\n", psm);
    }
}

static void on_l2cap_data_packet(uint16_t channel, uint8_t* packet, uint16_t size) {
    uni_hid_device_t *d, *sdp_d;
    uint64_t elapsed;

    d = uni_hid_device_get_instance_for_cid(channel);
    if (d == NULL) {
        loge("Invalid cid: 0x%04x\n", channel);
        // printf_hexdump(packet, size);
        return;
    }

    if (channel != d->conn.interrupt_cid)
        return;

    if (!uni_hid_device_has_hid_descriptor(d) || !uni_hid_device_has_controller_type(d)) {
        sdp_d = uni_hid_device_get_sdp_device(&elapsed);
        if (sdp_d == d) {
            logi("Device without HID descriptor or Product/Vendor ID yet.\n");
            // 1 second
            if (elapsed < (1 * 1000000)) {
                logi("Waiting for SDP answer. Ignoring report.\n");
                return;
            } else {
                logi("SDP answer taking too long. Trying heuristics.\n");
                if (!uni_hid_device_guess_controller_type_from_packet(d, packet, size)) {
                    logi("Heuristics failed. Ignoring report.\n");
                    return;
                } else {
                    logi("Device was detected using heuristics.\n");
                    fsm_process(d);
                    return;
                }
            }
        } else {
            logi(
                "Another SDP query in progress. Disconnect gamepad and try "
                "again.\n");
            uni_hid_device_dump_device(d);
            return;
        }
    }

    int report_len = size;
    uint8_t* report = packet;

    // printf_hexdump(packet, size);

    // check if HID Input Report
    if (report_len < 1)
        return;
    if (*report != 0xa1)
        return;
    report++;
    report_len--;

    uni_hid_parser(d, report, report_len);
    uni_hid_device_process_gamepad(d);
}

// BLE only
static void hog_connection_timeout(btstack_timer_source_t* ts) {
    UNUSED(ts);
    logi("Timeout - abort connection\n");
    gap_connect_cancel();
}

/**
 * Connect to remote device but set timer for timeout
 * BLE only
 */
static void hog_connect(bd_addr_t addr, bd_addr_type_t addr_type) {
    // set timer
    btstack_run_loop_set_timer(&connection_timer, 10000);
    btstack_run_loop_set_timer_handler(&connection_timer, &hog_connection_timeout);
    btstack_run_loop_add_timer(&connection_timer);
    gap_connect(addr, addr_type);

    uni_hid_device_t* d = uni_hid_device_create(addr);
    if (!d) {
        loge("\nError: no more available device slots\n");
        return;
    }
    uni_bt_conn_set_state(&d->conn, UNI_BT_CONN_STATE_DEVICE_DISCOVERED);
}

// BLE only
static bool adv_event_contains_hid_service(const uint8_t* packet) {
    const uint8_t* ad_data = gap_event_advertising_report_get_data(packet);
    uint16_t ad_len = gap_event_advertising_report_get_data_length(packet);
    return ad_data_contains_uuid16(ad_len, ad_data, ORG_BLUETOOTH_SERVICE_HUMAN_INTERFACE_DEVICE);
}

static void continue_remote_names(void) {
    uni_hid_device_t* d = uni_hid_device_get_first_device_with_state(UNI_BT_CONN_STATE_REMOTE_NAME_REQUEST);
    if (!d) {
        // No device ready? Continue scanning
        start_scan();
        return;
    }
    uni_bt_conn_set_state(&d->conn, UNI_BT_CONN_STATE_REMOTE_NAME_INQUIRED);
    logi("Fetching remote name of %s\n", bd_addr_to_str(d->conn.remote_addr));
    gap_remote_name_request(d->conn.remote_addr, d->conn.page_scan_repetition_mode, d->conn.clock_offset | 0x8000);
}

//--------------------------------------------------------------------------------
#ifndef MYUNIHID
//--------------------------------------------------------------------------------
static void start_scan(void) {
    logi("--> Scanning for new devices...\n");
    // Passive scanning, 100% (scan interval = scan window)
    // Start GAP BLE scan
#ifdef ENABLE_BLE
    gap_set_scan_parameters(0 /* type */, 48 /* interval */, 48 /* window */);
    gap_start_scan();
#endif  // ENABLE_BLE

    // Start GAP Classic inquiry
    gap_inquiry_start(INQUIRY_INTERVAL);
}
//--------------------------------------------------------------------------------
#endif
//--------------------------------------------------------------------------------


static void sdp_query_hid_descriptor(uni_hid_device_t* device) {
    logi("Starting SDP query for HID descriptor for: %s\n", bd_addr_to_str(device->conn.remote_addr));
    // Needed for the SDP query since it only supports one SDP query at the
    // time.
    uint64_t elapsed;
    uni_hid_device_t* sdp_dev = uni_hid_device_get_sdp_device(&elapsed);
    if (sdp_dev != NULL) {
        // If an SDP query didn't finish in 3 seconds, we can override it.
        if (elapsed < (3 * 1000000)) {
            loge(
                "Error: Another SDP query is in progress (%s). Elapsed time: "
                "%" PRId64 "\n",
                bd_addr_to_str(sdp_dev->conn.remote_addr), elapsed);
            return;
        } else {
            logi("Overriding old SDP query (%s). Elapsed time: %" PRId64 "\n",
                 bd_addr_to_str(sdp_dev->conn.remote_addr), elapsed);
        }
    }

    uni_bt_conn_set_state(&device->conn, UNI_BT_CONN_STATE_SDP_HID_DESCRIPTOR_REQUESTED);
    uni_hid_device_set_sdp_device(device);
    uint8_t status = sdp_client_query_uuid16(&handle_sdp_hid_query_result, device->conn.remote_addr,
                                             BLUETOOTH_SERVICE_CLASS_HUMAN_INTERFACE_DEVICE_SERVICE);
    if (status != 0) {
        uni_hid_device_set_sdp_device(NULL);
        loge("Failed to perform sdp query\n");
    }
}

static void sdp_query_product_id(uni_hid_device_t* device) {
    logi("Starting SDP query for product/vendor ID\n");
    uni_hid_device_t* sdp_dev = uni_hid_device_get_sdp_device(NULL);
    // This query runs after sdp_query_hid_descriptor() so
    // uni_hid_device_get_sdp_device() must not be NULL
    if (sdp_dev == NULL) {
        loge("Error: SDP device is NULL. Should not happen\n");
        return;
    }
    uni_bt_conn_set_state(&device->conn, UNI_BT_CONN_STATE_SDP_VENDOR_REQUESTED);
    uint8_t status = sdp_client_query_uuid16(&handle_sdp_pid_query_result, device->conn.remote_addr,
                                             BLUETOOTH_SERVICE_CLASS_PNP_INFORMATION);
    if (status != 0) {
        uni_hid_device_set_sdp_device(NULL);
        loge("Failed to perform SDP DeviceID query\n");
    }
}

static void list_link_keys(void) {
    bd_addr_t addr;
    link_key_t link_key;
    link_key_type_t type;
    btstack_link_key_iterator_t it;

    int ok = gap_link_key_iterator_init(&it);
    if (!ok) {
        loge("Link key iterator not implemented\n");
        return;
    }
    int32_t delete_keys = uni_get_platform()->get_property(UNI_PLATFORM_PROPERTY_DELETE_STORED_KEYS);
    if (delete_keys == 1)
        logi("Deleting stored link keys:\n");
    else
        logi("Stored link keys:\n");

    while (gap_link_key_iterator_get_next(&it, addr, link_key, &type)) {
        logi("%s - type %u, key: ", bd_addr_to_str(addr), (int)type);
        printf_hexdump(link_key, 16);
        if (delete_keys) {
            gap_drop_link_key_for_bd_addr(addr);
        }
    }
    logi(".\n");
    gap_link_key_iterator_done(&it);
}

static void l2cap_create_control_connection(uni_hid_device_t* d) {
    uint8_t status = l2cap_create_channel(BLUEPAD_packet_handler, d->conn.remote_addr, PSM_HID_CONTROL, L2CAP_CHANNEL_MTU, ///
                                          &d->conn.control_cid);
    if (status) {
        loge("\nConnecting or Auth to HID Control failed: 0x%02x", status);
    } else {
        uni_bt_conn_set_state(&d->conn, UNI_BT_CONN_STATE_L2CAP_CONTROL_CONNECTION_REQUESTED);
    }
}

static void l2cap_create_interrupt_connection(uni_hid_device_t* d) {
    uint8_t status = l2cap_create_channel(BLUEPAD_packet_handler, d->conn.remote_addr, PSM_HID_INTERRUPT, L2CAP_CHANNEL_MTU, ///
                                          &d->conn.interrupt_cid);
    if (status) {
        loge("\nConnecting or Auth to HID Interrupt failed: 0x%02x", status);
    } else {
        uni_bt_conn_set_state(&d->conn, UNI_BT_CONN_STATE_L2CAP_INTERRUPT_CONNECTION_REQUESTED);
    }
}

static void fsm_process(uni_hid_device_t* d) {
    uni_bt_conn_state_t state;

    // logi("fsm_process: %p = 0x%02x\n", d, d->state);
    if (d == NULL) {
        loge("fsm_process: Invalid device\n");
    }
    // Two possible flows:
    // - Incoming (initiated by gamepad)
    // - Or discovered (initiated by Bluepad32).

    state = uni_bt_conn_get_state(&d->conn);

    // XXX: convert to "switch" statement

    // Incoming connections might have the HID already pre-fecthed, or not.
    if (uni_hid_device_is_incoming(d)) {
        if (state == UNI_BT_CONN_STATE_L2CAP_INTERRUPT_CONNECTED) {
            if (uni_hid_device_has_hid_descriptor(d)) {
                /* done */
                uni_hid_device_set_ready(d);
            } else {
                sdp_query_hid_descriptor(d);
            }
        } else if (state == UNI_BT_CONN_STATE_SDP_HID_DESCRIPTOR_FETCHED) {
            sdp_query_product_id(d);
        } else if (state == UNI_BT_CONN_STATE_SDP_VENDOR_FETCHED) {
            /* done */
            uni_hid_device_set_ready(d);
        }
    } else {
        if (state == UNI_BT_CONN_STATE_DEVICE_DISCOVERED) {
            logd("STATE_DEVICE_DISCOVERED\n");
            // FIXME: Temporary skip name discovery
            // uni_bt_conn_set_state(&d->conn, UNI_BT_CONN_STATE_REMOTE_NAME_REQUEST);

            // No name? No problem, we do the SDP query after connect.
            l2cap_create_control_connection(d);
        } else if (state == UNI_BT_CONN_STATE_REMOTE_NAME_FETCHED) {
            logd("STATE_REMOTE_NAME_FETCHED\n");
            if (strncmp(d->name, "Wireless Controller", strlen(d->name)) == 0) {
                logi("Detected DualShock 4. Doing SDP query before connect.\n");
                d->sdp_query_before_connect = 1;
            }
            if (d->sdp_query_before_connect) {
                sdp_query_hid_descriptor(d);
            } else {
                l2cap_create_control_connection(d);
            }
        } else if (state == UNI_BT_CONN_STATE_L2CAP_CONTROL_CONNECTED) {
            logd("STATE_L2CAP_CONTROL_CONNECTED\n");
            l2cap_create_interrupt_connection(d);
        } else if (state == UNI_BT_CONN_STATE_L2CAP_INTERRUPT_CONNECTED) {
            logd("STATE_L2CAP_INTERRUPT_CONNECTED\n");
            if (d->sdp_query_before_connect) {
                /* done */
                uni_hid_device_set_ready(d);
            } else {
                sdp_query_hid_descriptor(d);
            }
        } else if (state == UNI_BT_CONN_STATE_SDP_HID_DESCRIPTOR_FETCHED) {
            logd("STATE_SDP_HID_DESCRIPTOR_FETCHED\n");
            sdp_query_product_id(d);
        } else if (state == UNI_BT_CONN_STATE_SDP_VENDOR_FETCHED) {
            logd("STATE_SDP_HID_VENDOR_FETCHED\n");
            if (d->sdp_query_before_connect) {
                l2cap_create_control_connection(d);
            } else {
                /* done */
                uni_hid_device_set_ready(d);
            }
        }
    }
}

//
// Public functions
//
int uni_bluetooth_init(void) {
    // register for HCI events
    BLUEPAD_hci_event_callback_registration.callback = &BLUEPAD_packet_handler; ///
    hci_add_event_handler(&BLUEPAD_hci_event_callback_registration); ///

    // enabled EIR
    hci_set_inquiry_mode(INQUIRY_MODE_RSSI_AND_EIR);

    // btstack_stdin_setup(stdin_process);
    hci_set_master_slave_policy(HCI_ROLE_MASTER);

    // It seems that with gap_security_level(0) all gamepads work except Nintendo Switch Pro controller.
#ifndef CONFIG_BLUEPAD32_GAP_SECURITY
    gap_set_security_level((gap_security_level_t)0); ///
#endif
    // Allow sniff mode requests by HID device and support role switch
    gap_set_default_link_policy_settings(LM_LINK_POLICY_ENABLE_SNIFF_MODE | LM_LINK_POLICY_ENABLE_ROLE_SWITCH);

    // Using a minimum of 7 bytes needed for Nintendo Wii / Wii U controllers.
    // See: https://github.com/bluekitchen/btstack/issues/299
    gap_set_required_encryption_key_size(7);

    int security_level = gap_get_security_level();
    logi("Gap security level: %d\n", security_level);

    l2cap_register_service(BLUEPAD_packet_handler, PSM_HID_INTERRUPT, L2CAP_CHANNEL_MTU, (gap_security_level_t)security_level); ///
    l2cap_register_service(BLUEPAD_packet_handler, PSM_HID_CONTROL, L2CAP_CHANNEL_MTU, (gap_security_level_t)security_level); ///
    l2cap_init();

#ifdef ENABLE_BLE
    // register for events from Security Manager
    BLUEPAD_sm_event_callback_registration.callback = &sm_packet_handler; ///
    sm_add_event_handler(&BLUEPAD_sm_event_callback_registration); ///

    // setup LE device db
    le_device_db_init();
    sm_init();
    gatt_client_init();
#endif  // ENABLE_BLE

    // Disable stdout buffering
    setbuf(stdout, NULL);

    // Turn on the device
    hci_power_control(HCI_POWER_ON);

    return 0;
}

// Deletes Bluetooth stored keys
void uni_bluetooth_del_keys(void) {
    bd_addr_t addr;
    link_key_t link_key;
    link_key_type_t type;
    btstack_link_key_iterator_t it;

    int ok = gap_link_key_iterator_init(&it);
    if (!ok) {
        loge("Link key iterator not implemented\n");
        return;
    }

    while (gap_link_key_iterator_get_next(&it, addr, link_key, &type)) {
        logi("Deleting key: %s - type %u\n", bd_addr_to_str(addr), (int)type);
        gap_drop_link_key_for_bd_addr(addr);
    }
    gap_link_key_iterator_done(&it);
}
