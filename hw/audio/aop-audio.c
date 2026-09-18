/*
 * Apple Always-On Processor: Audio.
 *
 * Copyright (c) 2025-2026 Visual Ehrmanntraut (VisualEhrmanntraut).
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include "hw/audio/aop-audio.h"
#include "hw/misc/aop.h"
#include "qemu/units.h"

#if 0
    #define AOP_DPRINTF(fmt, ...) fprintf(stderr, fmt "\n", ##__VA_ARGS__)
#else
    #define AOP_DPRINTF(fmt, ...) \
        do { }                    \
        while (0)
#endif

#define PROPERTY_IDENTITY         (0x64)
#define PROPERTY_DEVICE_COUNT     (0x65)
#define PROPERTY_IO_HANDLER_COUNT (0x66)
#define PROPERTY_VERSION          (0x67)

#define COMMAND_GET_DEVICE_ID   (0xC3000001)
#define COMMAND_ATTACH_DEVICE   (0xC3000002)
#define COMMAND_DETACH_DEVICE   (0xC3000003)
#define COMMAND_GET_DEVICE_PROP (0xC3000004)
#define COMMAND_SET_DEVICE_PROP (0xC3000005)
#define COMMAND_REGISTER_ACCESS (0xC3000006)
#define COMMAND_HANDLE_EVENT    (0xC3000008)

#define COMMAND_HDR_LEN (0x24)

#define DEV_PROP_MCA_RX_STATUS            (0x79)
#define DEV_PROP_MCA_RX_STATUS_LEN        (0x18)
#define DEV_PROP_MCA_TX_STATUS            (0x7A)
#define DEV_PROP_MCA_TX_STATUS_LEN        (0x30)
#define DEV_PROP_MCA_RX0_SHIM_OVERRUN     (0xD7)
#define DEV_PROP_MCA_RX0_SHIM_OVERRUN_LEN (4)

#define DEV_PCM_MGR 'pcmM'
// AppleAOPAudioAssetManagerProperties::kNumberOfSupportedAssets
#define DEV_PROP_PCM_NUM_SUPPORTED_ASSETS     (0xC8)
#define DEV_PROP_PCM_NUM_SUPPORTED_ASSETS_LEN (4)

#define DEV_LEAP_FW 'lpfw'
// AppleAOPAudioFirmware::FirmwareAssetProperties::kFirmwareBufferBytesMax
#define DEV_PROP_LEAP_FW_BUFFER_BYTES_MAX     (0xC9)
#define DEV_PROP_LEAP_FW_BUFFER_BYTES_MAX_LEN (8)

#define DEV_PROP_STATE                        (0xC8)
#define DEV_PROP_STATE_LEN                    (4)
#define DEV_PROP_SUPPORTS_HISTORICAL_DATA     (0x12C)
#define DEV_PROP_SUPPORTS_HISTORICAL_DATA_LEN (0x4)
#define DEV_PROP_CHANNEL_CTRL                 (0x12D)
#define DEV_PROP_CHANNEL_CTRL_LEN             (0x10)
#define DEV_PROP_STREAM_FORMAT                (0x12E)
#define DEV_PROP_STREAM_FORMAT_LEN            (0x10)

struct AppleAOPAudioState
{
    SysBusDevice parent_obj;

    AppleAOPEndpoint* ep;
    uint32_t          supported_chans;
    uint32_t          enabled_chans;
    uint32_t          voice_trigger_chans;
    uint32_t          history_chans;
};

static void apple_aop_audio_class_init(ObjectClass* klass, const void* data)
{
    DeviceClass* dc;

    dc = DEVICE_CLASS(klass);

    dc->desc           = "Apple Always-On Processor Audio";
    dc->user_creatable = false;
    set_bit(DEVICE_CATEGORY_SOUND, dc->categories);
}

OBJECT_DEFINE_SIMPLE_TYPE_CLASS_INIT(AppleAOPAudioState, apple_aop_audio, APPLE_AOP_AUDIO, SYS_BUS_DEVICE)

// AOP Audio Devices
// - edtC | Embedded Device Tree Config
// - acmm | AOP Audio Codec Mclk Manager
// - aphc | AOP Audio Haptic Control
// - lpfw | LEAP Firmware
// - leap | Low Energy Actuator Processor?
// - aph  | AOP Audio Haptics
// - aphd | AOP Audio Haptics Debug?
// - ahdc | AOP Audio Haptics Debug Control?
// - pcmM | PCM Asset Manager
// - lpai | Low Power Audio Input
// - mcaN | Multi-Channel Audio (cluster N)
// - apac | AOP Audio Controller?
static const uint32_t apple_aop_devices[] = {
    'edtC', 'acmm', 'aphc', 'lpfw', 'leap', 'aphd', 'aph ', 'ahdc', 'pcmM', 'lpai', 'mca0', 'mca1', 'apac',
};

static AppleAOPResult apple_aop_audio_get_prop(void* opaque, uint32_t prop, void* out)
{
    AOP_DPRINTF("AOPAudio GetProperty 0x%X", prop);

    switch (prop) {
        case PROPERTY_IDENTITY:    // ???
            stl_le_p(out, 'aop ');
            break;
        case PROPERTY_DEVICE_COUNT    : stl_le_p(out, ARRAY_SIZE(apple_aop_devices)); break;
        case PROPERTY_IO_HANDLER_COUNT: stl_le_p(out, 0); break;
        default                       : break;
    }

    return AOP_RESULT_OK;
}

static AppleAOPResult apple_aop_audio_handle_command(void* opaque, uint16_t seq, void* payload, uint32_t len,
                                                     void* payload_out, uint32_t out_len)
{
    AppleAOPAudioState* s = opaque;

    if (payload == NULL || len < COMMAND_HDR_LEN || ldl_le_p(payload) != 0xFFFFFFFF) { return AOP_RESULT_ERROR; }

    switch (ldl_le_p(payload + sizeof(uint32_t))) {
        case COMMAND_GET_DEVICE_ID:
            AOP_DPRINTF("AOPAudio GetDeviceID %d", ldl_le_p(payload + COMMAND_HDR_LEN));

            stl_le_p(payload_out, apple_aop_devices[ldl_le_p(payload + COMMAND_HDR_LEN)]);
            break;
        case COMMAND_GET_DEVICE_PROP:
            AOP_DPRINTF("AOPAudio GetDeviceProperty %X 0x%X", ldl_le_p(payload + COMMAND_HDR_LEN),
                        ldl_le_p(payload + COMMAND_HDR_LEN + 4));

            switch (ldl_le_p(payload + COMMAND_HDR_LEN)) {
                case 'lpai':
                    switch (ldl_le_p(payload + COMMAND_HDR_LEN + 4)) {
                        case DEV_PROP_STATE:
                            stl_le_p(payload_out, DEV_PROP_STATE_LEN);
                            stl_le_p(payload_out + 4, 'idle');
                            break;
                        case DEV_PROP_CHANNEL_CTRL:
                            stl_le_p(payload_out, DEV_PROP_CHANNEL_CTRL_LEN);
                            stl_le_p(payload_out + 4, s->supported_chans);
                            stl_le_p(payload_out + 8, s->enabled_chans);
                            stl_le_p(payload_out + 12, s->voice_trigger_chans);
                            stl_le_p(payload_out + 16, s->history_chans);
                            break;
                        case DEV_PROP_STREAM_FORMAT:
                            stl_le_p(payload_out, DEV_PROP_STREAM_FORMAT_LEN);
                            stl_le_p(payload_out + 4, 'pcm ');
                            stl_le_p(payload_out + 8, 48000);
                            stl_le_p(payload_out + 12, 2);
                            stl_le_p(payload_out + 16, 2);
                            break;
                        case DEV_PROP_SUPPORTS_HISTORICAL_DATA:
                            stl_le_p(payload_out, DEV_PROP_SUPPORTS_HISTORICAL_DATA_LEN);
                            stl_le_p(payload_out + 4, 0);
                            break;
                    }
                    break;
                case 'lai ':
                    switch (ldl_le_p(payload + COMMAND_HDR_LEN + 4)) {
                        case DEV_PROP_STATE:
                            stl_le_p(payload_out, DEV_PROP_STATE_LEN);
                            stl_le_p(payload_out + 4, 'idle');
                            break;
                        case DEV_PROP_SUPPORTS_HISTORICAL_DATA:
                            stl_le_p(payload_out, DEV_PROP_SUPPORTS_HISTORICAL_DATA_LEN);
                            stl_le_p(payload_out + 4, 0);
                            break;
                    }
                    break;
                case 'mca0':
                case 'mca1':
                    switch (ldl_le_p(payload + COMMAND_HDR_LEN + 4)) {
                        case DEV_PROP_MCA_RX_STATUS: stl_le_p(payload_out, DEV_PROP_MCA_RX_STATUS_LEN); break;
                        case DEV_PROP_MCA_TX_STATUS: stl_le_p(payload_out, DEV_PROP_MCA_TX_STATUS_LEN); break;
                        case DEV_PROP_MCA_RX0_SHIM_OVERRUN:
                            stl_le_p(payload_out, DEV_PROP_MCA_RX0_SHIM_OVERRUN_LEN);
                            break;
                    }
                    break;
                case 'acmm':
                case 'apac':
                    switch (ldl_le_p(payload + COMMAND_HDR_LEN + 4)) {
                        case DEV_PROP_STATE:
                            stl_le_p(payload_out, DEV_PROP_STATE_LEN);
                            stl_le_p(payload_out + 4, 'pwrd');
                            break;
                    }
                    break;
                case 'aphc':
                case 'aph ':
                    switch (ldl_le_p(payload + COMMAND_HDR_LEN + 4)) {
                        case DEV_PROP_STATE:
                            stl_le_p(payload_out, DEV_PROP_STATE_LEN);
                            stl_le_p(payload_out + 4, 'pw1 ');
                            break;
                    }
                    break;
                case DEV_PCM_MGR:
                    switch (ldl_le_p(payload + COMMAND_HDR_LEN + 4)) {
                        case DEV_PROP_PCM_NUM_SUPPORTED_ASSETS:
                            stl_le_p(payload_out, DEV_PROP_PCM_NUM_SUPPORTED_ASSETS_LEN);
                            stl_le_p(payload_out + 4, 2);
                            break;
                    }
                    break;
                case DEV_LEAP_FW:
                    switch (ldl_le_p(payload + COMMAND_HDR_LEN + 4)) {
                        case DEV_PROP_LEAP_FW_BUFFER_BYTES_MAX:
                            stl_le_p(payload_out, DEV_PROP_LEAP_FW_BUFFER_BYTES_MAX_LEN);
                            stq_le_p(payload_out + 4, 128 * KiB);
                            break;
                    }
                    break;
            }
            break;
        case COMMAND_SET_DEVICE_PROP:
            AOP_DPRINTF("AOPAudio SetDeviceProperty %X 0x%X", ldl_le_p(payload + COMMAND_HDR_LEN),
                        ldl_le_p(payload + COMMAND_HDR_LEN + 4));

            switch (ldl_le_p(payload + COMMAND_HDR_LEN)) {
                case 'lpai':
                    switch (ldl_le_p(payload + COMMAND_HDR_LEN + 4)) {
                        case DEV_PROP_CHANNEL_CTRL:
                            s->supported_chans     = ldl_le_p(payload + COMMAND_HDR_LEN + 12);
                            s->enabled_chans       = ldl_le_p(payload + COMMAND_HDR_LEN + 16);
                            s->voice_trigger_chans = ldl_le_p(payload + COMMAND_HDR_LEN + 20);
                            s->history_chans       = ldl_le_p(payload + COMMAND_HDR_LEN + 24);
                            break;
                    }
                    break;
            }
            break;
        default: break;
    }

    return AOP_RESULT_OK;
}

static const AppleAOPEndpointDescription apple_aop_audio_ep_descr = {
    .type           = AOP_EP_TYPE_APP,
    .service_name   = "aop-audio",
    .service_id     = 0x1000000D,
    .rx_len         = 0x4000,
    .tx_len         = 0xD0000,
    .get_property   = apple_aop_audio_get_prop,
    .handle_command = apple_aop_audio_handle_command,
};

SysBusDevice* apple_aop_audio_create(AppleAOP* aop)
{
    DeviceState*        dev;
    SysBusDevice*       sbd;
    AppleAOPAudioState* s;

    dev = qdev_new(TYPE_APPLE_AOP_AUDIO);
    sbd = SYS_BUS_DEVICE(dev);
    s   = APPLE_AOP_AUDIO(dev);

    s->ep = apple_aop_ep_create(aop, s, &apple_aop_audio_ep_descr);

    return sbd;
}
