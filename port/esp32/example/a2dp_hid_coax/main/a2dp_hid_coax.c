/*
 * Copyright (C) 2016 BlueKitchen GmbH
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

#define __BTSTACK_FILE__ "a2dp_sink_demo.c"

/*
 * a2dp_sink_demo.c
 */

// *****************************************************************************
/* EXAMPLE_START(a2dp_sink_demo): Receive audio stream and control its playback.
 *
 * @text This A2DP Sink example demonstrates how to use the A2DP Sink service to 
 * receive an audio data stream from a remote A2DP Source device. In addition,
 * the AVRCP Controller is used to get information on currently played media, 
 * such are title, artist and album, as well as to control the playback, 
 * i.e. to play, stop, repeat, etc.
 *
 * @test To test with a remote device, e.g. a mobile phone,
 * pair from the remote device with the demo, then start playing music on the remote device.
 * Alternatively, set the device_addr_string to the Bluetooth address of your 
 * remote device in the code, and call connect from the UI.
 * 
 * @test To controll the playback, tap SPACE on the console to show the available 
 * AVRCP commands.
 */
// *****************************************************************************

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "btstack.h"

#define AVRCP_BROWSING_ENABLED 0

#ifdef HAVE_BTSTACK_STDIN
#include "btstack_stdin.h"
#endif

#include "btstack_ring_buffer.h"

#ifdef HAVE_POSIX_FILE_IO
#include "wav_util.h"
#define STORE_SBC_TO_SBC_FILE 
#define STORE_SBC_TO_WAV_FILE 
#endif

#define NUM_CHANNELS 2
#define BYTES_PER_FRAME     (2*NUM_CHANNELS)
#define MAX_SBC_FRAME_SIZE 120

// SBC Decoder for WAV file or live playback
static btstack_sbc_decoder_state_t state;
static btstack_sbc_mode_t mode = SBC_MODE_STANDARD;

// ring buffer for SBC Frames
// below 30: add samples, 30-40: fine, above 40: drop samples
#define OPTIMAL_FRAMES_MIN 30
#define OPTIMAL_FRAMES_MAX 40
#define ADDITIONAL_FRAMES  10
static uint8_t sbc_frame_storage[(OPTIMAL_FRAMES_MAX + ADDITIONAL_FRAMES) * MAX_SBC_FRAME_SIZE];
static btstack_ring_buffer_t sbc_frame_ring_buffer;
static unsigned int sbc_frame_size;
static int sbc_samples_fix;

// rest buffer for not fully used sbc frames
static uint8_t decoded_audio_storage[(MAX_SBC_FRAME_SIZE+4) * BYTES_PER_FRAME];
static btstack_ring_buffer_t decoded_audio_ring_buffer;

// 
static int audio_stream_started;

// temp storage of lower-layer request
static int16_t * request_buffer;
static int       request_samples;

// WAV File
#ifdef STORE_SBC_TO_WAV_FILE    
static int frame_count = 0;
static char * wav_filename = "avdtp_sink.wav";
#endif

#ifdef STORE_SBC_TO_SBC_FILE    
static FILE * sbc_file;
static char * sbc_filename = "avdtp_sink.sbc";
#endif

typedef struct {
    // bitmaps
    uint8_t sampling_frequency_bitmap;
    uint8_t channel_mode_bitmap;
    uint8_t block_length_bitmap;
    uint8_t subbands_bitmap;
    uint8_t allocation_method_bitmap;
    uint8_t min_bitpool_value;
    uint8_t max_bitpool_value;
} adtvp_media_codec_information_sbc_t;

typedef struct {
    int reconfigure;
    int num_channels;
    int sampling_frequency;
    int channel_mode;
    int block_length;
    int subbands;
    int allocation_method;
    int min_bitpool_value;
    int max_bitpool_value;
    int frames_per_buffer;
} avdtp_media_codec_configuration_sbc_t;

#ifdef HAVE_BTSTACK_STDIN
// pts: static bd_addr_t remote = {0x00, 0x1B, 0xDC, 0x08, 0x0A, 0xA5};
// mac 2013: static const char * device_addr_string = "84:38:35:65:d1:15";
// iPhone 5S: 
static const char * device_addr_string = "54:E4:3A:26:A2:39";
#endif

static uint8_t  sdp_avdtp_sink_service_buffer[150];
static avdtp_media_codec_configuration_sbc_t sbc_configuration;
static uint16_t a2dp_cid = 0;
static uint8_t  local_seid = 0;
static uint8_t  value[100];

static btstack_packet_callback_registration_t hci_event_callback_registration;

static int media_initialized = 0;

#ifdef HAVE_BTSTACK_STDIN
static bd_addr_t device_addr;
#endif

static uint16_t a2dp_sink_connected = 0;
static uint16_t avrcp_cid = 0;
static uint8_t  avrcp_connected = 0;
static uint8_t  sdp_avrcp_controller_service_buffer[200];

static uint8_t media_sbc_codec_capabilities[] = {
    0xFF,//(AVDTP_SBC_44100 << 4) | AVDTP_SBC_STEREO,
    0xFF,//(AVDTP_SBC_BLOCK_LENGTH_16 << 4) | (AVDTP_SBC_SUBBANDS_8 << 2) | AVDTP_SBC_ALLOCATION_METHOD_LOUDNESS,
    2, 53
}; 

static uint8_t media_sbc_codec_configuration[] = {
    (AVDTP_SBC_44100 << 4) | AVDTP_SBC_STEREO,
    (AVDTP_SBC_BLOCK_LENGTH_16 << 4) | (AVDTP_SBC_SUBBANDS_8 << 2) | AVDTP_SBC_ALLOCATION_METHOD_LOUDNESS,
    2, 53
}; 
///////////////////////////////////////////////////////////////////////////////////////////////////////
/* @section hid_keyboard definition an functions
 * @text hid+asdp profile combination demo.
 * 
*/
// to enable demo text on POSIX systems
// #undef HAVE_BTSTACK_STDIN

// from USB HID Specification 1.1, Appendix B.1
const uint8_t hid_descriptor_keyboard_boot_mode[] = {

    0x05, 0x01,                    // Usage Page (Generic Desktop)
    0x09, 0x06,                    // Usage (Keyboard)
    0xa1, 0x01,                    // Collection (Application)

    // Modifier byte

    0x75, 0x01,                    //   Report Size (1)
    0x95, 0x08,                    //   Report Count (8)
    0x05, 0x07,                    //   Usage Page (Key codes)
    0x19, 0xe0,                    //   Usage Minimum (Keyboard LeftControl)
    0x29, 0xe7,                    //   Usage Maxium (Keyboard Right GUI)
    0x15, 0x00,                    //   Logical Minimum (0)
    0x25, 0x01,                    //   Logical Maximum (1)
    0x81, 0x02,                    //   Input (Data, Variable, Absolute)

    // Reserved byte

    0x75, 0x01,                    //   Report Size (1)
    0x95, 0x08,                    //   Report Count (8)
    0x81, 0x03,                    //   Input (Constant, Variable, Absolute)

    // LED report + padding

    0x95, 0x05,                    //   Report Count (5)
    0x75, 0x01,                    //   Report Size (1)
    0x05, 0x08,                    //   Usage Page (LEDs)
    0x19, 0x01,                    //   Usage Minimum (Num Lock)
    0x29, 0x05,                    //   Usage Maxium (Kana)
    0x91, 0x02,                    //   Output (Data, Variable, Absolute)

    0x95, 0x01,                    //   Report Count (1)
    0x75, 0x03,                    //   Report Size (3)
    0x91, 0x03,                    //   Output (Constant, Variable, Absolute)

    // Keycodes

    0x95, 0x06,                    //   Report Count (6)
    0x75, 0x08,                    //   Report Size (8)
    0x15, 0x00,                    //   Logical Minimum (0)
    0x25, 0xff,                    //   Logical Maximum (1)
    0x05, 0x07,                    //   Usage Page (Key codes)
    0x19, 0x00,                    //   Usage Minimum (Reserved (no event indicated))
    0x29, 0xff,                    //   Usage Maxium (Reserved)
    0x81, 0x00,                    //   Input (Data, Array)

    0xc0,                          // End collection  
};

// 
#define CHAR_ILLEGAL     0xff
#define CHAR_RETURN     '\n'
#define CHAR_ESCAPE      27
#define CHAR_TAB         '\t'
#define CHAR_BACKSPACE   0x7f

// Simplified US Keyboard with Shift modifier

/**
 * English (US)
 */
static const uint8_t keytable_us_none [] = {
    CHAR_ILLEGAL, CHAR_ILLEGAL, CHAR_ILLEGAL, CHAR_ILLEGAL,             /*   0-3 */
    'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h', 'i', 'j',                   /*  4-13 */
    'k', 'l', 'm', 'n', 'o', 'p', 'q', 'r', 's', 't',                   /* 14-23 */
    'u', 'v', 'w', 'x', 'y', 'z',                                       /* 24-29 */
    '1', '2', '3', '4', '5', '6', '7', '8', '9', '0',                   /* 30-39 */
    CHAR_RETURN, CHAR_ESCAPE, CHAR_BACKSPACE, CHAR_TAB, ' ',            /* 40-44 */
    '-', '=', '[', ']', '\\', CHAR_ILLEGAL, ';', '\'', 0x60, ',',       /* 45-54 */
    '.', '/', CHAR_ILLEGAL, CHAR_ILLEGAL, CHAR_ILLEGAL, CHAR_ILLEGAL,   /* 55-60 */
    CHAR_ILLEGAL, CHAR_ILLEGAL, CHAR_ILLEGAL, CHAR_ILLEGAL,             /* 61-64 */
    CHAR_ILLEGAL, CHAR_ILLEGAL, CHAR_ILLEGAL, CHAR_ILLEGAL,             /* 65-68 */
    CHAR_ILLEGAL, CHAR_ILLEGAL, CHAR_ILLEGAL, CHAR_ILLEGAL,             /* 69-72 */
    CHAR_ILLEGAL, CHAR_ILLEGAL, CHAR_ILLEGAL, CHAR_ILLEGAL,             /* 73-76 */
    CHAR_ILLEGAL, CHAR_ILLEGAL, CHAR_ILLEGAL, CHAR_ILLEGAL,             /* 77-80 */
    CHAR_ILLEGAL, CHAR_ILLEGAL, CHAR_ILLEGAL, CHAR_ILLEGAL,             /* 81-84 */
    '*', '-', '+', '\n', '1', '2', '3', '4', '5',                       /* 85-97 */
    '6', '7', '8', '9', '0', '.', 0xa7,                                 /* 97-100 */
}; 

static const uint8_t keytable_us_shift[] = {
    CHAR_ILLEGAL, CHAR_ILLEGAL, CHAR_ILLEGAL, CHAR_ILLEGAL,             /*  0-3  */
    'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H', 'I', 'J',                   /*  4-13 */
    'K', 'L', 'M', 'N', 'O', 'P', 'Q', 'R', 'S', 'T',                   /* 14-23 */
    'U', 'V', 'W', 'X', 'Y', 'Z',                                       /* 24-29 */
    '!', '@', '#', '$', '%', '^', '&', '*', '(', ')',                   /* 30-39 */
    CHAR_RETURN, CHAR_ESCAPE, CHAR_BACKSPACE, CHAR_TAB, ' ',            /* 40-44 */
    '_', '+', '{', '}', '|', CHAR_ILLEGAL, ':', '"', 0x7E, '<',         /* 45-54 */
    '>', '?', CHAR_ILLEGAL, CHAR_ILLEGAL, CHAR_ILLEGAL, CHAR_ILLEGAL,   /* 55-60 */
    CHAR_ILLEGAL, CHAR_ILLEGAL, CHAR_ILLEGAL, CHAR_ILLEGAL,             /* 61-64 */
    CHAR_ILLEGAL, CHAR_ILLEGAL, CHAR_ILLEGAL, CHAR_ILLEGAL,             /* 65-68 */
    CHAR_ILLEGAL, CHAR_ILLEGAL, CHAR_ILLEGAL, CHAR_ILLEGAL,             /* 69-72 */
    CHAR_ILLEGAL, CHAR_ILLEGAL, CHAR_ILLEGAL, CHAR_ILLEGAL,             /* 73-76 */
    CHAR_ILLEGAL, CHAR_ILLEGAL, CHAR_ILLEGAL, CHAR_ILLEGAL,             /* 77-80 */
    CHAR_ILLEGAL, CHAR_ILLEGAL, CHAR_ILLEGAL, CHAR_ILLEGAL,             /* 81-84 */
    '*', '-', '+', '\n', '1', '2', '3', '4', '5',                       /* 85-97 */
    '6', '7', '8', '9', '0', '.', 0xb1,                                 /* 97-100 */
}; 

// STATE

static uint8_t hid_service_buffer[250];
static uint8_t device_id_sdp_service_buffer[100];
static const char hid_device_name[] = "BTstack HID Keyboard";
static uint16_t hid_cid;

static enum {
    APP_BOOTING,
    APP_NOT_CONNECTED,
    APP_CONNECTING,
    APP_CONNECTED
} app_state = APP_BOOTING;


// HID Keyboard lookup
static int lookup_keycode(uint8_t character, const uint8_t * table, int size, uint8_t * keycode){
    int i;
    for (i=0;i<size;i++){
        if (table[i] != character) continue;
        *keycode = i;
        return 1;
    }
    return 0;
}

static int keycode_and_modifer_us_for_character(uint8_t character, uint8_t * keycode, uint8_t * modifier){
    int found;
    found = lookup_keycode(character, keytable_us_none, sizeof(keytable_us_none), keycode);
    if (found) {
        *modifier = 0;  // none
        return 1;
    }
    found = lookup_keycode(character, keytable_us_shift, sizeof(keytable_us_shift), keycode);
    if (found) {
        *modifier = 2;  // shift
        return 1;
    }
    return 0;
}

// HID Report sending
static int send_keycode;
static int send_modifier;

static void send_key(int modifier, int keycode){
    send_keycode = keycode;
    send_modifier = modifier;
    hid_device_request_can_send_now_event(hid_cid);
}

static void send_report(int modifier, int keycode){
    uint8_t report[] = { 0xa1, modifier, 0, 0, keycode, 0, 0, 0, 0, 0};
    hid_device_send_interrupt_message(hid_cid, &report[0], sizeof(report));
}

// Demo Application
/*
#ifdef HAVE_BTSTACK_STDIN

// On systems with STDIN, we can directly type on the console

static void stdin_process(char character){
    uint8_t modifier;
    uint8_t keycode;
    int found;

    switch (app_state){
        case APP_BOOTING:
        case APP_CONNECTING:
            // ignore
            break;

        case APP_CONNECTED:
            // send keyu
            found = keycode_and_modifer_us_for_character(character, &keycode, &modifier);
            if (found){
                send_key(modifier, keycode);
                return;
            }
            break;
        case APP_NOT_CONNECTED:
            printf("Connecting to %s...\n", bd_addr_to_str(device_addr));
            hid_device_connect(device_addr, &hid_cid);
            break;
    }
}
#else

// On embedded systems, send constant demo text with fixed period

#define TYPING_PERIOD_MS 100
static const char * demo_text = "\n\nHello World!\n\nThis is the BTstack HID Keyboard Demo running on an Embedded Device.\n\n";

static int demo_pos;
static btstack_timer_source_t typing_timer;

static void typing_timer_handler(btstack_timer_source_t * ts){

    // abort if not connected
    if (!hid_cid) return;

    // get next character
    uint8_t character = demo_text[demo_pos++];
    if (demo_text[demo_pos] == 0){
        demo_pos = 0;
    }

    // get keycodeand send
    uint8_t modifier;
    uint8_t keycode;
    int found = keycode_and_modifer_us_for_character(character, &keycode, &modifier);
    if (found){
        send_key(modifier, keycode);
    }

    // set next timer
    btstack_run_loop_set_timer(ts, TYPING_PERIOD_MS);
    btstack_run_loop_add_timer(ts);
}

static void hid_embedded_start_typing(void){
    demo_pos = 0;
    // set one-shot timer
    typing_timer.process = &typing_timer_handler;
    btstack_run_loop_set_timer(&typing_timer, TYPING_PERIOD_MS);
    btstack_run_loop_add_timer(&typing_timer);
}

#endif
*/

static void hci4hid_packet_handler(uint8_t packet_type, uint16_t channel, uint8_t * packet, uint16_t packet_size){
    UNUSED(channel);
    UNUSED(packet_size);
    uint8_t status;
    switch (packet_type){
        case HCI_EVENT_PACKET:
            switch (packet[0]){
                case BTSTACK_EVENT_STATE:
                    if (btstack_event_state_get_state(packet) != HCI_STATE_WORKING) return;
                    app_state = APP_NOT_CONNECTED;
                    break;

                case HCI_EVENT_USER_CONFIRMATION_REQUEST:
                    // ssp: inform about user confirmation request
                    log_info("SSP User Confirmation Request with numeric value '%06"PRIu32"'\n", hci_event_user_confirmation_request_get_numeric_value(packet));
                    log_info("SSP User Confirmation Auto accept\n");                   
                    break; 

                case HCI_EVENT_HID_META:
                    switch (hci_event_hid_meta_get_subevent_code(packet)){
                        case HID_SUBEVENT_CONNECTION_OPENED:
                            status = hid_subevent_connection_opened_get_status(packet);
                            if (status) {
                                // outgoing connection failed
                                printf("Connection failed, status 0x%x\n", status);
                                app_state = APP_NOT_CONNECTED;
                                hid_cid = 0;
                                return;
                            }
                            app_state = APP_CONNECTED;
                            hid_cid = hid_subevent_connection_opened_get_hid_cid(packet);
#ifdef HAVE_BTSTACK_STDIN                        
                            printf("HID Connected, please start typing...\n");
#else                        
                            printf("HID Connected, sending demo text...\n");
                            hid_embedded_start_typing();
#endif
                            break;
                        case HID_SUBEVENT_CONNECTION_CLOSED:
                            printf("HID Disconnected\n");
                            app_state = APP_NOT_CONNECTED;
                            hid_cid = 0;
                            break;
                        case HID_SUBEVENT_CAN_SEND_NOW:
                            if (send_keycode){
                                send_report(send_modifier, send_keycode);
                                send_keycode = 0;
                                send_modifier = 0;
                                hid_device_request_can_send_now_event(hid_cid);
                            } else {
                                send_report(0, 0);
                            }
                            break;
                        default:
                            break;
                    }
                    break;
                default:
                    break;
            }
            break;
        default:
            break;
    }
}


/* @section Main Application Setup
 *
 * @text The Listing MainConfiguration shows how to setup AD2P Sink and AVRCP controller services. 
 * To announce A2DP Sink and AVRCP Controller services, you need to create corresponding
 * SDP records and register them with the SDP service. 
 * You'll also need to register several packet handlers:
 * - a2dp_sink_packet_handler - handles events on stream connection status (established, released), the media codec configuration, and, the status of the stream itself (opened, paused, stopped).
 * - handle_l2cap_media_data_packet - used to receive streaming data. If HAVE_PORTAUDIO or STORE_SBC_TO_WAV_FILE directives (check btstack_config.h) are used, the SBC decoder will be used to decode the SBC data into PCM frames. The resulting PCM frames are then processed in the SBC Decoder callback.
 * - stdin_process callback - used to trigger AVRCP commands to the A2DP Source device, such are get now playing info, start, stop, volume control. Requires HAVE_BTSTACK_STDIN.
 * - avrcp_controller_packet_handler - used to receive answers for AVRCP commands,
 *
 * @text Note, currently only the SBC codec is supported. 
 * If you want to store the audio data in a file, you'll need to define STORE_SBC_TO_WAV_FILE. The HAVE_PORTAUDIO directive indicates if the audio is played back via PortAudio.
 * If HAVE_PORTAUDIO or STORE_SBC_TO_WAV_FILE directives is defined, the SBC decoder needs to get initialized when a2dp_sink_packet_handler receives event A2DP_SUBEVENT_STREAM_STARTED. 
 * The initialization of the SBC decoder requires a callback that handles PCM data:
 * - handle_pcm_data - handles PCM audio frames. Here, they are stored a in wav file if STORE_SBC_TO_WAV_FILE is defined, and/or played using the PortAudio library if HAVE_PORTAUDIO is defined.
 */

/* LISTING_START(MainConfiguration): Setup Audio Sink and AVRCP Controller services */
static void a2dp_sink_packet_handler(uint8_t packet_type, uint16_t channel, uint8_t * event, uint16_t event_size);
static void avrcp_controller_packet_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size);
static void hci_packet_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size);
static void handle_l2cap_media_data_packet(uint8_t seid, uint8_t *packet, uint16_t size);

static int a2dp_and_avrcp_setup(void){

    /* Register for HCI events */
	// a2dp
    hci_event_callback_registration.callback = &hci_packet_handler;
    hci_add_event_handler(&hci_event_callback_registration);
	// hid
    hci_event_callback_registration.callback = &hci4hid_packet_handler;
    hci_add_event_handler(&hci_event_callback_registration);
    hci_register_sco_packet_handler(&hci4hid_packet_handler);
	
	// Initialize L2CAP
    l2cap_init();
	
    // Initialize AVDTP Sink
    a2dp_sink_init();
    a2dp_sink_register_packet_handler(&a2dp_sink_packet_handler);
    a2dp_sink_register_media_handler(&handle_l2cap_media_data_packet);

    uint8_t status = a2dp_sink_create_stream_endpoint(AVDTP_AUDIO, AVDTP_CODEC_SBC, media_sbc_codec_capabilities, sizeof(media_sbc_codec_capabilities), media_sbc_codec_configuration, sizeof(media_sbc_codec_configuration), &local_seid);
    if (status != ERROR_CODE_SUCCESS){
        printf("A2DP Sink: not enough memory to create local stream endpoint\n");
        return 1;
    }
    // Initialize AVRCP COntroller
    //avrcp_controller_init();
    //avrcp_controller_register_packet_handler(&avrcp_controller_packet_handler);
    
    // Initialize SDP 
    sdp_init();
	
    // setup AVDTP sink
    memset(sdp_avdtp_sink_service_buffer, 0, sizeof(sdp_avdtp_sink_service_buffer));
    a2dp_sink_create_sdp_record(sdp_avdtp_sink_service_buffer, 0x10001, 1, NULL, NULL);
    sdp_register_service(sdp_avdtp_sink_service_buffer);  
	
    // setup AVRCP
    //memset(sdp_avrcp_controller_service_buffer, 0, sizeof(sdp_avrcp_controller_service_buffer));
    //avrcp_controller_create_sdp_record(sdp_avrcp_controller_service_buffer, 0x10001, AVRCP_BROWSING_ENABLED, 1, NULL, NULL);
    //sdp_register_service(sdp_avrcp_controller_service_buffer);
    
	// setup HID
    memset(hid_service_buffer, 0, sizeof(hid_service_buffer));
    // hid sevice subclass 2540 Keyboard, hid counntry code 33 US, hid virtual cable off, hid reconnect initiate off, hid boot device off 
    hid_create_sdp_record(hid_service_buffer, 0x10003, 0x2540, 33, 0, 0, 0, hid_descriptor_keyboard_boot_mode, sizeof(hid_descriptor_keyboard_boot_mode), hid_device_name);
    printf("HID service record size: %u\n", de_get_len( hid_service_buffer));
    sdp_register_service(hid_service_buffer);
    // See https://www.bluetooth.com/specifications/assigned-numbers/company-identifiers if you don't have a USB Vendor ID and need a Bluetooth Vendor ID
    // device info: BlueKitchen GmbH, product 1, version 1
    device_id_create_sdp_record(device_id_sdp_service_buffer, 0x10004, DEVICE_ID_VENDOR_ID_SOURCE_BLUETOOTH, BLUETOOTH_COMPANY_ID_BLUEKITCHEN_GMBH, 1, 1);
    printf("Device ID SDP service record size: %u\n", de_get_len((uint8_t*)device_id_sdp_service_buffer));
    sdp_register_service(device_id_sdp_service_buffer);
	
	// HID Device
    hid_device_init();
    hid_device_register_packet_handler(&hci4hid_packet_handler);
	
    gap_set_local_name("COAX Demo 00:00:00:00:00:00");
    gap_discoverable_control(1);
    gap_set_class_of_device(0x2540);
    //gap_set_class_of_device(0x200408);

    return 0;
}

static void playback_handler(int16_t * buffer, uint16_t num_samples){
    
    // called from lower-layer but guaranteed to be on main thread

    // first fill from decoded_audio
    uint32_t bytes_read;
    btstack_ring_buffer_read(&decoded_audio_ring_buffer, (uint8_t *) buffer, num_samples * BYTES_PER_FRAME, &bytes_read);
    buffer          += bytes_read / NUM_CHANNELS;
    num_samples     -= bytes_read / BYTES_PER_FRAME;

    // then start decoding sbc frames using request_* globals
    request_buffer = buffer;
    request_samples = num_samples;
    while (request_samples && btstack_ring_buffer_bytes_available(&sbc_frame_ring_buffer) >= sbc_frame_size){
        // log_info("buffer %06u bytes -- need %d", btstack_ring_buffer_bytes_available(&sbc_frame_ring_buffer), request_samples);
        // decode frame
        uint8_t frame[MAX_SBC_FRAME_SIZE];
        btstack_ring_buffer_read(&sbc_frame_ring_buffer, frame, sbc_frame_size, &bytes_read);
        btstack_sbc_decoder_process_data(&state, 0, frame, sbc_frame_size);
    }
}

static void handle_pcm_data(int16_t * data, int num_samples, int num_channels, int sample_rate, void * context){
    UNUSED(sample_rate);
    UNUSED(context);
    UNUSED(num_channels);   // must be stereo == 2

#ifdef STORE_SBC_TO_WAV_FILE
    wav_writer_write_int16(num_samples * NUM_CHANNELS, data);
    frame_count++;
#endif

    int sbc_samples_fix_applied = 0;

    // drop audio frame to fix drift
    if (sbc_samples_fix < 0){
        num_samples--;
        data += NUM_CHANNELS;
        sbc_samples_fix_applied = 1;
    }

    // store data in btstack_audio buffer first
    if (request_samples){

        // add audio frame to fix drift
        if (!sbc_samples_fix_applied && sbc_samples_fix > 0){
            memcpy(request_buffer, data, BYTES_PER_FRAME);
            request_samples--;
            request_buffer += NUM_CHANNELS;
            sbc_samples_fix_applied = 1;
        }

        int samples_to_copy = btstack_min(num_samples, request_samples);
        memcpy(request_buffer, data, samples_to_copy * BYTES_PER_FRAME);
        num_samples     -= samples_to_copy;
        request_samples -= samples_to_copy;
        data            += samples_to_copy * NUM_CHANNELS;
        request_buffer  += samples_to_copy * NUM_CHANNELS;
    }

    // and rest in ring buffer
    if (num_samples){

        // add audio frame to fix drift
        if (!sbc_samples_fix_applied && sbc_samples_fix > 0){
            btstack_ring_buffer_write(&decoded_audio_ring_buffer, (uint8_t *) data, BYTES_PER_FRAME);
            sbc_samples_fix_applied = 1;
        }

        btstack_ring_buffer_write(&decoded_audio_ring_buffer, (uint8_t *) data, num_samples * BYTES_PER_FRAME);
    }
}

static int media_processing_init(avdtp_media_codec_configuration_sbc_t configuration){
    if (media_initialized) return 0;

    btstack_sbc_decoder_init(&state, mode, handle_pcm_data, NULL);

#ifdef STORE_SBC_TO_WAV_FILE
    wav_writer_open(wav_filename, configuration.num_channels, configuration.sampling_frequency);
#endif

#ifdef STORE_SBC_TO_SBC_FILE    
   sbc_file = fopen(sbc_filename, "wb"); 
#endif

    btstack_ring_buffer_init(&sbc_frame_ring_buffer, sbc_frame_storage, sizeof(sbc_frame_storage));
    btstack_ring_buffer_init(&decoded_audio_ring_buffer, decoded_audio_storage, sizeof(decoded_audio_storage));

    // setup audio playback
    const btstack_audio_t * audio = btstack_audio_get_instance();
    if (audio){
        audio->init(NUM_CHANNELS, configuration.sampling_frequency, &playback_handler, NULL);
    }

    audio_stream_started = 0;
    media_initialized = 1;
    return 0;
}

static void media_processing_close(void){
    if (!media_initialized) return;
    media_initialized = 0;
    audio_stream_started = 0;

#ifdef STORE_SBC_TO_WAV_FILE                  
    wav_writer_close();
    int total_frames_nr = state.good_frames_nr + state.bad_frames_nr + state.zero_frames_nr;

    printf("WAV Writer: Decoding done. Processed totaly %d frames:\n - %d good\n - %d bad\n", total_frames_nr, state.good_frames_nr, total_frames_nr - state.good_frames_nr);
    printf("WAV Writer: Written %d frames to wav file: %s\n", frame_count, wav_filename);
#endif

#ifdef STORE_SBC_TO_SBC_FILE
    fclose(sbc_file);
#endif     

    // stop audio playback
    const btstack_audio_t * audio = btstack_audio_get_instance();
    if (audio){
        audio->close();
    }
}

/* @section Handle Media Data Packet 
 *
 * @text Media data packets, in this case the audio data, are received through the handle_l2cap_media_data_packet callback.
 * Currently, only the SBC media codec is supported. Hence, the media data consists of the media packet header and the SBC packet.
 * The SBC frame will be stored in a ring buffer for later processing (instead of decoding it to PCM right away which would require a much larger buffer)
 * If the audio stream wasn't started already and there are enough SBC frames in the ring buffer, start playback.
 */ 

static int read_media_data_header(uint8_t * packet, int size, int * offset, avdtp_media_packet_header_t * media_header);
static int read_sbc_header(uint8_t * packet, int size, int * offset, avdtp_sbc_codec_header_t * sbc_header);

static void handle_l2cap_media_data_packet(uint8_t seid, uint8_t *packet, uint16_t size){
    UNUSED(seid);
    int pos = 0;
    
    avdtp_media_packet_header_t media_header;
    if (!read_media_data_header(packet, size, &pos, &media_header)) return;
    
    avdtp_sbc_codec_header_t sbc_header;
    if (!read_sbc_header(packet, size, &pos, &sbc_header)) return;

    // store sbc frame size for buffer management
    sbc_frame_size = (size-pos)/ sbc_header.num_frames;
        
    btstack_ring_buffer_write(&sbc_frame_ring_buffer, packet+pos, size-pos);

    // decide on audio sync drift based on number of sbc frames in queue
    int sbc_frames_in_buffer = btstack_ring_buffer_bytes_available(&sbc_frame_ring_buffer) / sbc_frame_size;
    if (sbc_frames_in_buffer < OPTIMAL_FRAMES_MIN){
    	sbc_samples_fix = 1;	// duplicate last sample
    } else if (sbc_frames_in_buffer <= OPTIMAL_FRAMES_MAX){
    	sbc_samples_fix = 0;	// nothing to do
    } else {
    	sbc_samples_fix = -1;	// drop last sample
    }

    // dump
    // printf("%6u %03u %d\n",  (int) btstack_run_loop_get_time_ms(), sbc_frames_in_buffer, sbc_samples_fix);
    // log_info("%03u %d", sbc_frames_in_buffer, sbc_samples_fix);

#ifdef STORE_SBC_TO_SBC_FILE
    fwrite(packet+pos, size-pos, 1, sbc_file);
#endif

    // start stream if enough frames buffered
    if (!audio_stream_started && sbc_frames_in_buffer >= (OPTIMAL_FRAMES_MAX+OPTIMAL_FRAMES_MIN)/2){
        audio_stream_started = 1;
        // setup audio playback
        const btstack_audio_t * audio = btstack_audio_get_instance();
        if (audio){
            audio->start_stream();
        }
    }
}

static int read_sbc_header(uint8_t * packet, int size, int * offset, avdtp_sbc_codec_header_t * sbc_header){
    int sbc_header_len = 12; // without crc
    int pos = *offset;
    
    if (size - pos < sbc_header_len){
        printf("Not enough data to read SBC header, expected %d, received %d\n", sbc_header_len, size-pos);
        return 0;
    }

    sbc_header->fragmentation = get_bit16(packet[pos], 7);
    sbc_header->starting_packet = get_bit16(packet[pos], 6);
    sbc_header->last_packet = get_bit16(packet[pos], 5);
    sbc_header->num_frames = packet[pos] & 0x0f;
    pos++;
    // printf("SBC HEADER: num_frames %u, fragmented %u, start %u, stop %u\n", sbc_header.num_frames, sbc_header.fragmentation, sbc_header.starting_packet, sbc_header.last_packet);
    *offset = pos;
    return 1;
}

static int read_media_data_header(uint8_t *packet, int size, int *offset, avdtp_media_packet_header_t *media_header){
    int media_header_len = 12; // without crc
    int pos = *offset;
    
    if (size - pos < media_header_len){
        printf("Not enough data to read media packet header, expected %d, received %d\n", media_header_len, size-pos);
        return 0;
    }

    media_header->version = packet[pos] & 0x03;
    media_header->padding = get_bit16(packet[pos],2);
    media_header->extension = get_bit16(packet[pos],3);
    media_header->csrc_count = (packet[pos] >> 4) & 0x0F;
    pos++;

    media_header->marker = get_bit16(packet[pos],0);
    media_header->payload_type  = (packet[pos] >> 1) & 0x7F;
    pos++;

    media_header->sequence_number = big_endian_read_16(packet, pos);
    pos+=2;

    media_header->timestamp = big_endian_read_32(packet, pos);
    pos+=4;

    media_header->synchronization_source = big_endian_read_32(packet, pos);
    pos+=4;
    *offset = pos;
    // TODO: read csrc list
    
    // printf_hexdump( packet, pos );
    // printf("MEDIA HEADER: %u timestamp, version %u, padding %u, extension %u, csrc_count %u\n", 
    //     media_header->timestamp, media_header->version, media_header->padding, media_header->extension, media_header->csrc_count);
    // printf("MEDIA HEADER: marker %02x, payload_type %02x, sequence_number %u, synchronization_source %u\n", 
    //     media_header->marker, media_header->payload_type, media_header->sequence_number, media_header->synchronization_source);
    return 1;
}

static void dump_sbc_configuration(avdtp_media_codec_configuration_sbc_t configuration){
    printf("Received SBC configuration:\n");
    printf("    - num_channels: %d\n", configuration.num_channels);
    printf("    - sampling_frequency: %d\n", configuration.sampling_frequency);
    printf("    - channel_mode: %d\n", configuration.channel_mode);
    printf("    - block_length: %d\n", configuration.block_length);
    printf("    - subbands: %d\n", configuration.subbands);
    printf("    - allocation_method: %d\n", configuration.allocation_method);
    printf("    - bitpool_value [%d, %d] \n", configuration.min_bitpool_value, configuration.max_bitpool_value);
    printf("\n");
}

static void avrcp_controller_packet_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size){
    UNUSED(channel);
    UNUSED(size);
    uint16_t local_cid;
    uint8_t  status = 0xFF;
    bd_addr_t adress;
    
    if (packet_type != HCI_EVENT_PACKET) return;
    if (hci_event_packet_get_type(packet) != HCI_EVENT_AVRCP_META) return;
    switch (packet[2]){
        case AVRCP_SUBEVENT_CONNECTION_ESTABLISHED: {
            local_cid = avrcp_subevent_connection_established_get_avrcp_cid(packet);
            if (avrcp_cid != 0 && avrcp_cid != local_cid) {
                printf("AVRCP demo: Connection failed, expected 0x%02X l2cap cid, received 0x%02X\n", avrcp_cid, local_cid);
                return;
            }

            status = avrcp_subevent_connection_established_get_status(packet);
            if (status != ERROR_CODE_SUCCESS){
                printf("AVRCP demo: Connection failed: status 0x%02x\n", status);
                avrcp_cid = 0;
                return;
            }
            
            avrcp_cid = local_cid;
            avrcp_connected = 1;
            avrcp_subevent_connection_established_get_bd_addr(packet, adress);
            printf("AVRCP demo: Channel successfully opened: %s, avrcp_cid 0x%02x\n", bd_addr_to_str(adress), avrcp_cid);

            // automatically enable notifications
            avrcp_controller_enable_notification(avrcp_cid, AVRCP_NOTIFICATION_EVENT_PLAYBACK_STATUS_CHANGED);
            avrcp_controller_enable_notification(avrcp_cid, AVRCP_NOTIFICATION_EVENT_NOW_PLAYING_CONTENT_CHANGED);
            avrcp_controller_enable_notification(avrcp_cid, AVRCP_NOTIFICATION_EVENT_VOLUME_CHANGED);
            avrcp_controller_enable_notification(avrcp_cid, AVRCP_NOTIFICATION_EVENT_TRACK_CHANGED);
            return;
        }
        case AVRCP_SUBEVENT_CONNECTION_RELEASED:
            printf("AVRCP demo: Channel released: avrcp_cid 0x%02x\n", avrcp_subevent_connection_released_get_avrcp_cid(packet));
            avrcp_cid = 0;
            avrcp_connected = 0;
            return;
        default:
            break;
    }

    status = packet[5];
    if (!avrcp_cid) return;

    // ignore INTERIM status
    if (status == AVRCP_CTYPE_RESPONSE_INTERIM){
        switch (packet[2]){
            case AVRCP_SUBEVENT_NOTIFICATION_PLAYBACK_POS_CHANGED:{
                uint32_t playback_position_ms = avrcp_subevent_notification_playback_pos_changed_get_playback_position_ms(packet);
                if (playback_position_ms == AVRCP_NO_TRACK_SELECTED_PLAYBACK_POSITION_CHANGED){
                    printf("notification, playback position changed, no track is selected\n");
                }  
                break;
            }
            default:
                printf(" INTERIM response \n"); 
                break;
        }
        return;
    } 
            
    printf("AVRCP demo: command status: %s, ", avrcp_ctype2str(status));
    switch (packet[2]){
        case AVRCP_SUBEVENT_NOTIFICATION_PLAYBACK_POS_CHANGED:
            printf("notification, playback position changed, position %d ms\n", (unsigned int) avrcp_subevent_notification_playback_pos_changed_get_playback_position_ms(packet));
            break;
        case AVRCP_SUBEVENT_NOTIFICATION_PLAYBACK_STATUS_CHANGED:
            printf("notification, playback status changed %s\n", avrcp_play_status2str(avrcp_subevent_notification_playback_status_changed_get_play_status(packet)));
            return;
        case AVRCP_SUBEVENT_NOTIFICATION_NOW_PLAYING_CONTENT_CHANGED:
            printf("notification, playing content changed\n");
            return;
        case AVRCP_SUBEVENT_NOTIFICATION_TRACK_CHANGED:
            printf("notification track changed\n");
            return;
        case AVRCP_SUBEVENT_NOTIFICATION_VOLUME_CHANGED:
            printf("notification absolute volume changed %d\n", avrcp_subevent_notification_volume_changed_get_absolute_volume(packet));
            return;
        case AVRCP_SUBEVENT_NOTIFICATION_AVAILABLE_PLAYERS_CHANGED:
            printf("notification changed\n");
            return; 
        case AVRCP_SUBEVENT_SHUFFLE_AND_REPEAT_MODE:{
            uint8_t shuffle_mode = avrcp_subevent_shuffle_and_repeat_mode_get_shuffle_mode(packet);
            uint8_t repeat_mode  = avrcp_subevent_shuffle_and_repeat_mode_get_repeat_mode(packet);
            printf("%s, %s\n", avrcp_shuffle2str(shuffle_mode), avrcp_repeat2str(repeat_mode));
            break;
        }
        case AVRCP_SUBEVENT_NOW_PLAYING_TITLE_INFO:
            if (avrcp_subevent_now_playing_title_info_get_value_len(packet) > 0){
                memcpy(value, avrcp_subevent_now_playing_title_info_get_value(packet), avrcp_subevent_now_playing_title_info_get_value_len(packet));
                printf("    Title: %s\n", value);
            }  
            break;

        case AVRCP_SUBEVENT_NOW_PLAYING_ARTIST_INFO:
            if (avrcp_subevent_now_playing_artist_info_get_value_len(packet) > 0){
                memcpy(value, avrcp_subevent_now_playing_artist_info_get_value(packet), avrcp_subevent_now_playing_artist_info_get_value_len(packet));
                printf("    Artist: %s\n", value);
            }  
            break;
        
        case AVRCP_SUBEVENT_NOW_PLAYING_ALBUM_INFO:
            if (avrcp_subevent_now_playing_album_info_get_value_len(packet) > 0){
                memcpy(value, avrcp_subevent_now_playing_album_info_get_value(packet), avrcp_subevent_now_playing_album_info_get_value_len(packet));
                printf("    Album: %s\n", value);
            }  
            break;
        
        case AVRCP_SUBEVENT_NOW_PLAYING_GENRE_INFO:
            if (avrcp_subevent_now_playing_genre_info_get_value_len(packet) > 0){
                memcpy(value, avrcp_subevent_now_playing_genre_info_get_value(packet), avrcp_subevent_now_playing_genre_info_get_value_len(packet));
                printf("    Genre: %s\n", value);
            }  
            break;
        
        case AVRCP_SUBEVENT_PLAY_STATUS:
            printf("song length: %"PRIu32" ms, song position: %"PRIu32" ms, play status: %s\n", 
                avrcp_subevent_play_status_get_song_length(packet), 
                avrcp_subevent_play_status_get_song_position(packet),
                avrcp_play_status2str(avrcp_subevent_play_status_get_play_status(packet)));
            break;
        case AVRCP_SUBEVENT_OPERATION_COMPLETE:
            printf("operation done %s\n", avrcp_operation2str(avrcp_subevent_operation_complete_get_operation_id(packet)));
            break;
        case AVRCP_SUBEVENT_OPERATION_START:
            printf("operation start %s\n", avrcp_operation2str(avrcp_subevent_operation_complete_get_operation_id(packet)));
            break;
        case AVRCP_SUBEVENT_PLAYER_APPLICATION_VALUE_RESPONSE:
            // response to set shuffle and repeat mode
            printf("\n");
            break;
        default:
            printf("AVRCP demo: event is not parsed\n");
            break;
    }  
}

static void hci_packet_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size){
    UNUSED(channel);
    UNUSED(size);
    if (packet_type != HCI_EVENT_PACKET) return;
    if (hci_event_packet_get_type(packet) == HCI_EVENT_PIN_CODE_REQUEST) {
        bd_addr_t address;
        printf("Pin code request - using '0000'\n");
        hci_event_pin_code_request_get_bd_addr(packet, address);
        gap_pin_code_response(address, "0000");
    }
}

static void a2dp_sink_packet_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size){
    UNUSED(channel);
    UNUSED(size);
    uint16_t cid;
    bd_addr_t address;
    uint8_t status;

    if (packet_type != HCI_EVENT_PACKET) return;
    if (hci_event_packet_get_type(packet) != HCI_EVENT_A2DP_META) return;

    switch (packet[2]){
        case A2DP_SUBEVENT_SIGNALING_MEDIA_CODEC_OTHER_CONFIGURATION:
            printf("A2DP Sink demo: received non SBC codec. not implemented.\n");
            break;
        case A2DP_SUBEVENT_SIGNALING_MEDIA_CODEC_SBC_CONFIGURATION:{
            printf("A2DP Sink demo: received SBC codec configuration.\n");
            sbc_configuration.reconfigure = a2dp_subevent_signaling_media_codec_sbc_configuration_get_reconfigure(packet);
            sbc_configuration.num_channels = a2dp_subevent_signaling_media_codec_sbc_configuration_get_num_channels(packet);
            sbc_configuration.sampling_frequency = a2dp_subevent_signaling_media_codec_sbc_configuration_get_sampling_frequency(packet);
            sbc_configuration.channel_mode = a2dp_subevent_signaling_media_codec_sbc_configuration_get_channel_mode(packet);
            sbc_configuration.block_length = a2dp_subevent_signaling_media_codec_sbc_configuration_get_block_length(packet);
            sbc_configuration.subbands = a2dp_subevent_signaling_media_codec_sbc_configuration_get_subbands(packet);
            sbc_configuration.allocation_method = a2dp_subevent_signaling_media_codec_sbc_configuration_get_allocation_method(packet);
            sbc_configuration.min_bitpool_value = a2dp_subevent_signaling_media_codec_sbc_configuration_get_min_bitpool_value(packet);
            sbc_configuration.max_bitpool_value = a2dp_subevent_signaling_media_codec_sbc_configuration_get_max_bitpool_value(packet);
            sbc_configuration.frames_per_buffer = sbc_configuration.subbands * sbc_configuration.block_length;
            dump_sbc_configuration(sbc_configuration);

            if (sbc_configuration.reconfigure){
                media_processing_close();
            }
            // prepare media processing
            media_processing_init(sbc_configuration);
            break;
        }  
        case A2DP_SUBEVENT_STREAM_ESTABLISHED:
            a2dp_subevent_stream_established_get_bd_addr(packet, address);
            status = a2dp_subevent_stream_established_get_status(packet);
            cid = a2dp_subevent_stream_established_get_a2dp_cid(packet);
            printf("A2DP_SUBEVENT_STREAM_ESTABLISHED %d, %d \n", cid, a2dp_cid);
            if (!a2dp_cid){
                // incoming connection
                a2dp_cid = cid;
            } else if (cid != a2dp_cid) {
                break;
            }
            if (status){
                a2dp_sink_connected = 0;
                printf("A2DP Sink demo: streaming connection failed, status 0x%02x\n", status);
                break;
            }
            printf("A2DP Sink demo: streaming connection is established, address %s, a2dp cid 0x%02X, local_seid %d\n", bd_addr_to_str(address), a2dp_cid, local_seid);
            
            memcpy(device_addr, address, 6);

            local_seid = a2dp_subevent_stream_established_get_local_seid(packet);
            a2dp_sink_connected = 1;
            break;
        
        case A2DP_SUBEVENT_STREAM_STARTED:
            cid = a2dp_subevent_stream_started_get_a2dp_cid(packet);
            if (cid != a2dp_cid) break;
            local_seid = a2dp_subevent_stream_started_get_local_seid(packet);
            printf("A2DP Sink demo: stream started, a2dp cid 0x%02X, local_seid %d\n", a2dp_cid, local_seid);
            // started
            media_processing_init(sbc_configuration);
            break;
        
        case A2DP_SUBEVENT_STREAM_SUSPENDED:
            cid = a2dp_subevent_stream_suspended_get_a2dp_cid(packet);
            if (cid != a2dp_cid) break;
            local_seid = a2dp_subevent_stream_suspended_get_local_seid(packet);
            printf("A2DP Sink demo: stream paused, a2dp cid 0x%02X, local_seid %d\n", a2dp_cid, local_seid);
            media_processing_close();
            break;
        
        case A2DP_SUBEVENT_STREAM_RELEASED:
            cid = a2dp_subevent_stream_released_get_a2dp_cid(packet);
            if (cid != a2dp_cid) {
                printf("A2DP Sink demo: unexpected cid 0x%02x instead of 0x%02x\n", cid, a2dp_cid);
                break;
            }
            local_seid = a2dp_subevent_stream_released_get_local_seid(packet);
            printf("A2DP Sink demo: stream released, a2dp cid 0x%02X, local_seid %d\n", a2dp_cid, local_seid);
            media_processing_close();
            break;
        case A2DP_SUBEVENT_SIGNALING_CONNECTION_RELEASED:
            cid = a2dp_subevent_signaling_connection_released_get_a2dp_cid(packet);
            if (cid != a2dp_cid) {
                printf("A2DP Sink demo: unexpected cid 0x%02x instead of 0x%02x\n", cid, a2dp_cid);
                break;
            }
            a2dp_sink_connected = 0;
            printf("A2DP Sink demo: signaling connection released\n");
            break;
        default:
            printf("A2DP Sink demo: not parsed 0x%02x\n", packet[2]);
            break; 
    }
}

#ifdef HAVE_BTSTACK_STDIN
static void show_usage(void){
    bd_addr_t      iut_address;
    gap_local_bd_addr(iut_address);
    printf("\n--- Bluetooth AVDTP Sink/AVRCP Connection Test Console %s ---\n", bd_addr_to_str(iut_address));
    printf("b      - AVDTP Sink create  connection to addr %s\n", bd_addr_to_str(device_addr));
    printf("B      - AVDTP Sink disconnect\n");
    printf("c      - AVRCP create connection to addr %s\n", bd_addr_to_str(device_addr));
    printf("C      - AVRCP disconnect\n");

    printf("\n--- Bluetooth AVRCP Commands %s ---\n", bd_addr_to_str(iut_address));
    printf("O - get play status\n");
    printf("j - get now playing info\n");
    printf("k - play\n");
    printf("K - stop\n");
    printf("L - pause\n");
    printf("u - start fast forward\n");
    printf("U - stop  fast forward\n");
    printf("n - start rewind\n");
    printf("N - stop rewind\n");
    printf("i - forward\n");
    printf("I - backward\n");
    printf("t - volume up\n");
    printf("T - volume down\n");
    printf("p - absolute volume of 50 percent\n");
    printf("M - mute\n");
    printf("r - skip\n");
    printf("q - query repeat and shuffle mode\n");
    printf("v - repeat single track\n");
    printf("x - repeat all tracks\n");
    printf("X - disable repeat mode\n");
    printf("z - shuffle all tracks\n");
    printf("Z - disable shuffle mode\n");

    printf("a/A - register/deregister TRACK_CHANGED\n");
    printf("d/D - register/deregister PLAYBACK_POS_CHANGED\n");

    printf("---\n");
}
#endif

#ifdef HAVE_BTSTACK_STDIN
static void stdin_process(char cmd){
    uint8_t status = ERROR_CODE_SUCCESS;
    uint8_t modifier;
    uint8_t keycode;
    uint8_t character;
    int found;

    character = (uint8_t)cmd;

    switch (app_state){
        case APP_BOOTING:
        case APP_CONNECTING:
            // ignore
            break;

        case APP_CONNECTED:
            // send keyu
            found = keycode_and_modifer_us_for_character(character, &keycode, &modifier);
            if (found){
                send_key(modifier, keycode);
               // return;
		break;
            }
            break;
        case APP_NOT_CONNECTED:
            printf("Connecting to %s...\n", bd_addr_to_str(device_addr));
            hid_device_connect(device_addr, &hid_cid);
            break;
    }
    if (!avrcp_connected){
        switch (cmd){
            case 'b':
            case 'c':
                break;
            case '\n':
            case '\r':
            case ' ':
                show_usage();
                return;
            default:
                show_usage();
                printf("Not connected. Please use 'b' or c' to establish a connection with device (addr %s).\n", bd_addr_to_str(device_addr));
                return;    
        }
    }
    
    switch (cmd){
        case 'b':
            status = a2dp_sink_establish_stream(device_addr, local_seid, &a2dp_cid);
            printf(" - Create AVDTP connection to addr %s, and local seid %d, expected cid 0x%02x.\n", bd_addr_to_str(device_addr), local_seid, a2dp_cid);
            break;
        case 'B':
            printf(" - AVDTP disconnect from addr %s.\n", bd_addr_to_str(device_addr));
            status = avdtp_sink_disconnect(a2dp_cid);
            break;
        case 'c':
            printf(" - Create AVRCP connection to addr %s.\n", bd_addr_to_str(device_addr));
            status = avrcp_controller_connect(device_addr, &avrcp_cid);
            break;
        case 'C':
            printf(" - AVRCP disconnect from addr %s.\n", bd_addr_to_str(device_addr));
            status = avrcp_controller_disconnect(avrcp_cid);
            break;

        case '\n':
        case '\r':
            break;
        case 'O':
            printf(" - get play status\n");
            status = avrcp_controller_get_play_status(avrcp_cid);
            break;
        case 'j':
            printf(" - get now playing info\n");
            status = avrcp_controller_get_now_playing_info(avrcp_cid);
            break;
        case 'k':
            printf(" - play\n");
            status = avrcp_controller_play(avrcp_cid);
            break;
        case 'K':
            printf(" - stop\n");
            status = avrcp_controller_stop(avrcp_cid);
            break;
        case 'L':
            printf(" - pause\n");
            status = avrcp_controller_pause(avrcp_cid);
            break;
        case 'u':
            printf(" - start fast forward\n");
            status = avrcp_controller_press_and_hold_fast_forward(avrcp_cid);
            break;
        case 'U':
            printf(" - stop fast forward\n");
            status = avrcp_controller_release_press_and_hold_cmd(avrcp_cid);
            break;
        case 'n':
            printf(" - start rewind\n");
            status = avrcp_controller_press_and_hold_rewind(avrcp_cid);
            break;
        case 'N':
            printf(" - stop rewind\n");
            status = avrcp_controller_release_press_and_hold_cmd(avrcp_cid);
            break;
        case 'i':
            printf(" - forward\n");
            status = avrcp_controller_forward(avrcp_cid); 
            break;
        case 'I':
            printf(" - backward\n");
            status = avrcp_controller_backward(avrcp_cid);
            break;
        case 't':
            printf(" - volume up\n");
            status = avrcp_controller_volume_up(avrcp_cid);
            break;
        case 'T':
            printf(" - volume down\n");
            status = avrcp_controller_volume_down(avrcp_cid);
            break;
        case 'p':
            printf(" - absolute volume of 50 percent\n");
            status = avrcp_controller_set_absolute_volume(avrcp_cid, 50);
            break;
        case 'M':
            printf(" - mute\n");
            status = avrcp_controller_mute(avrcp_cid);
            break;
        case 'r':
            printf(" - skip\n");
            status = avrcp_controller_skip(avrcp_cid);
            break;
        case 'q':
            printf(" - query repeat and shuffle mode\n");
            status = avrcp_controller_query_shuffle_and_repeat_modes(avrcp_cid);
            break;
        case 'v':
            printf(" - repeat single track\n");
            status = avrcp_controller_set_repeat_mode(avrcp_cid, AVRCP_REPEAT_MODE_SINGLE_TRACK);
            break;
        case 'x':
            printf(" - repeat all tracks\n");
            status = avrcp_controller_set_repeat_mode(avrcp_cid, AVRCP_REPEAT_MODE_ALL_TRACKS);
            break;
        case 'X':
            printf(" - disable repeat mode\n");
            status = avrcp_controller_set_repeat_mode(avrcp_cid, AVRCP_REPEAT_MODE_OFF);
            break;
        case 'z':
            printf(" - shuffle all tracks\n");
            status = avrcp_controller_set_shuffle_mode(avrcp_cid, AVRCP_SHUFFLE_MODE_ALL_TRACKS);
            break;
        case 'Z':
            printf(" - disable shuffle mode\n");
            status = avrcp_controller_set_shuffle_mode(avrcp_cid, AVRCP_SHUFFLE_MODE_OFF);
            break;
        case 'a':
            printf("AVRCP: enable notification TRACK_CHANGED\n");
            avrcp_controller_enable_notification(avrcp_cid, AVRCP_NOTIFICATION_EVENT_TRACK_CHANGED);
            break;
        case 'A':
            printf("AVRCP: disable notification TRACK_CHANGED\n");
            avrcp_controller_disable_notification(avrcp_cid, AVRCP_NOTIFICATION_EVENT_TRACK_CHANGED);
            break;
        case 'd':
            printf("AVRCP: enable notification PLAYBACK_POS_CHANGED\n");
            avrcp_controller_enable_notification(avrcp_cid, AVRCP_NOTIFICATION_EVENT_PLAYBACK_POS_CHANGED);
            break;
        case 'D':
            printf("AVRCP: disable notification PLAYBACK_POS_CHANGED\n");
            avrcp_controller_disable_notification(avrcp_cid, AVRCP_NOTIFICATION_EVENT_PLAYBACK_POS_CHANGED);
            break;

        default:
            show_usage();
            return;
    }
    if (status != ERROR_CODE_SUCCESS){
        printf("Could not perform command, status 0x%2x\n", status);
    }
}
#endif

int btstack_main(int argc, const char * argv[]);
int btstack_main(int argc, const char * argv[]){
    UNUSED(argc);
    (void)argv;

    a2dp_and_avrcp_setup();

#ifdef HAVE_BTSTACK_STDIN
    // parse human readable Bluetooth address
    sscanf_bd_addr(device_addr_string, device_addr);
    btstack_stdin_setup(stdin_process);
#endif

    // turn on!
    printf("Starting BTstack ...\n");
    hci_power_control(HCI_POWER_ON);
    return 0;
}