/*++

Copyright (c) Microsoft Corporation All Rights Reserved

Module Name:

    hdmiwavtable.h

Abstract:

    Wave miniport tables for the HDMI render endpoint.

    A copy of speakerwavtable.h with a different format: 48 kHz stereo, 24
    valid bits in a 32-bit container.  On RK3576 the HDMI endpoint is fed by
    the SAI, which stores one sample per 32-bit FIFO word, so every sample
    has to arrive as one 32-bit DMA word.  The firmware's HDMI audio preset
    declares 24-bit samples in the IEC 60958 channel status to match.
--*/

#ifndef _CSAUDIORK3X_HDMIWAVTABLE_H_
#define _CSAUDIORK3X_HDMIWAVTABLE_H_

// Only 48 kHz, 24 in 32-bit, stereo: the format the firmware set the HDMI controller up for.

#define HDMI_DEVICE_MAX_CHANNELS                 2       // Max Channels.

#define HDMI_HOST_MAX_CHANNELS                   2       // Max Channels.
#define HDMI_HOST_MIN_BITS_PER_SAMPLE            32      // Min Bits Per Sample (container)
#define HDMI_HOST_MAX_BITS_PER_SAMPLE            32      // Max Bits Per Sample (container)
#define HDMI_HOST_MIN_SAMPLE_RATE                48000   // Min Sample Rate
#define HDMI_HOST_MAX_SAMPLE_RATE                48000   // Max Sample Rate

//
// Max # of pin instances.
//
#define HDMI_MAX_INPUT_SYSTEM_STREAMS            1

//=============================================================================

static 
KSDATAFORMAT_WAVEFORMATEXTENSIBLE HdmiHostPinSupportedDeviceFormats[] =
{
    { // 0
        {
            sizeof(KSDATAFORMAT_WAVEFORMATEXTENSIBLE),
            0,
            0,
            0,
            STATICGUIDOF(KSDATAFORMAT_TYPE_AUDIO),
            STATICGUIDOF(KSDATAFORMAT_SUBTYPE_PCM),
            STATICGUIDOF(KSDATAFORMAT_SPECIFIER_WAVEFORMATEX)
        },
        {
            {
                WAVE_FORMAT_EXTENSIBLE,
                2,
                48000,
                384000,
                8,
                32,
                sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX)
            },
            24,
            KSAUDIO_SPEAKER_STEREO,
            STATICGUIDOF(KSDATAFORMAT_SUBTYPE_PCM)
        }
    }
};

//
// Supported modes (only on streaming pins).
//
static
MODE_AND_DEFAULT_FORMAT HdmiHostPinSupportedDeviceModes[] =
{
    {
        STATIC_AUDIO_SIGNALPROCESSINGMODE_DEFAULT,
        &HdmiHostPinSupportedDeviceFormats[0].DataFormat  // 48KHz
    }
};

//
// The entries here must follow the same order as the filter's pin
// descriptor array.
//
static 
PIN_DEVICE_FORMATS_AND_MODES HdmiPinDeviceFormatsAndModes[] = 
{
    {
        SystemRenderPin,
        HdmiHostPinSupportedDeviceFormats,
        SIZEOF_ARRAY(HdmiHostPinSupportedDeviceFormats),
        HdmiHostPinSupportedDeviceModes,
        SIZEOF_ARRAY(HdmiHostPinSupportedDeviceModes)
    },
    {
        BridgePin,
        NULL,
        0,
        NULL,
        0
    }
};

//=============================================================================
static
KSDATARANGE_AUDIO HdmiPinDataRangesStream[] =
{
    { // 0
        {
            sizeof(KSDATARANGE_AUDIO),
            KSDATARANGE_ATTRIBUTES,         // An attributes list follows this data range
            0,
            0,
            STATICGUIDOF(KSDATAFORMAT_TYPE_AUDIO),
            STATICGUIDOF(KSDATAFORMAT_SUBTYPE_PCM),
            STATICGUIDOF(KSDATAFORMAT_SPECIFIER_WAVEFORMATEX)
        },
        HDMI_HOST_MAX_CHANNELS,           
        HDMI_HOST_MIN_BITS_PER_SAMPLE,    
        HDMI_HOST_MAX_BITS_PER_SAMPLE,    
        HDMI_HOST_MIN_SAMPLE_RATE,            
        HDMI_HOST_MAX_SAMPLE_RATE             
    }
};

static
PKSDATARANGE HdmiPinDataRangePointersStream[] =
{
    PKSDATARANGE(&HdmiPinDataRangesStream[0]),
    PKSDATARANGE(&PinDataRangeAttributeList),
};

//=============================================================================
static
KSDATARANGE HdmiPinDataRangesBridge[] =
{
    {
        sizeof(KSDATARANGE),
        0,
        0,
        0,
        STATICGUIDOF(KSDATAFORMAT_TYPE_AUDIO),
        STATICGUIDOF(KSDATAFORMAT_SUBTYPE_ANALOG),
        STATICGUIDOF(KSDATAFORMAT_SPECIFIER_NONE)
    }
};

static
PKSDATARANGE HdmiPinDataRangePointersBridge[] =
{
    &HdmiPinDataRangesBridge[0]
};

//=============================================================================
static
PCPIN_DESCRIPTOR HdmiWaveMiniportPins[] =
{
    // Wave Out Streaming Pin (Renderer) KSPIN_WAVE_RENDER3_SINK_SYSTEM
    {
        HDMI_MAX_INPUT_SYSTEM_STREAMS,
        HDMI_MAX_INPUT_SYSTEM_STREAMS, 
        0,
        NULL,        // AutomationTable
        {
            0,
            NULL,
            0,
            NULL,
            SIZEOF_ARRAY(HdmiPinDataRangePointersStream),
            HdmiPinDataRangePointersStream,
            KSPIN_DATAFLOW_IN,
            KSPIN_COMMUNICATION_SINK,
            &KSCATEGORY_AUDIO,
            NULL,
            0
        }
    },
    // Wave Out Bridge Pin (Renderer) KSPIN_WAVE_RENDER3_SOURCE
    {
        0,
        0,
        0,
        NULL,
        {
            0,
            NULL,
            0,
            NULL,
            SIZEOF_ARRAY(HdmiPinDataRangePointersBridge),
            HdmiPinDataRangePointersBridge,
            KSPIN_DATAFLOW_OUT,
            KSPIN_COMMUNICATION_NONE,
            &KSCATEGORY_AUDIO,
            NULL,
            0
        }
    },
};

//=============================================================================
//
//                   ----------------------------      
//                   |                          |      
//  Host Pin     0-->|                          |--> 1 KSPIN_WAVE_RENDER3_SOURCE
//                   |                          |      
//                   ----------------------------
static
PCCONNECTION_DESCRIPTOR HdmiWaveMiniportConnections[] =
{
    { PCFILTER_NODE,            KSPIN_WAVE_RENDER3_SINK_SYSTEM,     PCFILTER_NODE,   KSPIN_WAVE_RENDER3_SOURCE }
};

//=============================================================================
static
PCPROPERTY_ITEM PropertiesHdmiWaveFilter[] =
{
    {
        &KSPROPSETID_Pin,
        KSPROPERTY_PIN_PROPOSEDATAFORMAT,
        KSPROPERTY_TYPE_SET | KSPROPERTY_TYPE_BASICSUPPORT,
        PropertyHandler_WaveFilter
    },
    {
        &KSPROPSETID_Pin,
        KSPROPERTY_PIN_PROPOSEDATAFORMAT2,
        KSPROPERTY_TYPE_GET | KSPROPERTY_TYPE_BASICSUPPORT,
        PropertyHandler_WaveFilter
    }
};

DEFINE_PCAUTOMATION_TABLE_PROP(AutomationHdmiWaveFilter, PropertiesHdmiWaveFilter);

//=============================================================================
static
PCFILTER_DESCRIPTOR HdmiWaveMiniportFilterDescriptor =
{
    0,                                              // Version
    &AutomationHdmiWaveFilter,                   // AutomationTable
    sizeof(PCPIN_DESCRIPTOR),                       // PinSize
    SIZEOF_ARRAY(HdmiWaveMiniportPins),          // PinCount
    HdmiWaveMiniportPins,                        // Pins
    sizeof(PCNODE_DESCRIPTOR),                      // NodeSize
    0,                                              // NodeCount
    NULL,                                           // Nodes
    SIZEOF_ARRAY(HdmiWaveMiniportConnections),   // ConnectionCount
    HdmiWaveMiniportConnections,                 // Connections
    0,                                              // CategoryCount
    NULL                                            // Categories  - use defaults (audio, render, capture)
};

#endif // _CSAUDIORK3X_HDMIWAVTABLE_H_
