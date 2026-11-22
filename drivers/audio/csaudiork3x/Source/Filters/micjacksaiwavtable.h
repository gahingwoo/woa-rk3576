/*++

Copyright (c) Microsoft Corporation All Rights Reserved

Module Name:

    micjacksaiwavtable.h

Abstract:-

    Wave miniport tables for the jack capture endpoint on an RK3576 SAI.

    A copy of micarraywavtable.h with the format of hdmiwavtable.h: 48 kHz
    stereo, 24 valid bits in a 32-bit container.  csaudiork3x runs the SAI
    with 32-bit slots and VDW 32, one sample per 32-bit FIFO word, so each
    sample comes out of the DMA as one 32-bit word.

--*/

#ifndef _CSAUDIORK3X_MICJACKSAIWAVTABLE_H_
#define _CSAUDIORK3X_MICJACKSAIWAVTABLE_H_

//
// Mic array range.
//
#define MICJACKSAI_RAW_CHANNELS                   2       // Channels for raw mode
#define MICJACKSAI_DEVICE_MAX_CHANNELS            2       // Max channels overall
#define MICJACKSAI_32_BITS_PER_SAMPLE_PCM       32      // 32 Bits Per Sample (container)
#define MICJACKSAI_RAW_SAMPLE_RATE                48000   // Raw sample rate

//
// Max # of pin instances.
//
#define MICJACKSAI_MAX_INPUT_STREAMS              1

//=============================================================================
static
KSDATAFORMAT_WAVEFORMATEXTENSIBLE MicJackSaiPinSupportedDeviceFormats[] =
{
    // 48 KHz 24 in 32-bit 2 channels
    {
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
MODE_AND_DEFAULT_FORMAT MicJackSaiPinSupportedDeviceModes[] =
{
    {
        STATIC_AUDIO_SIGNALPROCESSINGMODE_RAW,
        &MicJackSaiPinSupportedDeviceFormats[0].DataFormat
    }
};

//
// The entries here must follow the same order as the filter's pin
// descriptor array.
//
static
PIN_DEVICE_FORMATS_AND_MODES MicJackSaiPinDeviceFormatsAndModes[] =
{
    {
        BridgePin,
        NULL,
        0,
        NULL,
        0
    },
    {
        SystemCapturePin,
        MicJackSaiPinSupportedDeviceFormats,
        SIZEOF_ARRAY(MicJackSaiPinSupportedDeviceFormats),
        MicJackSaiPinSupportedDeviceModes,
        SIZEOF_ARRAY(MicJackSaiPinSupportedDeviceModes)
    }
};

//=============================================================================
// Data ranges
//
// See CMiniportWaveRT::DataRangeIntersection.
//
static
KSDATARANGE_AUDIO MicJackSaiPinDataRangesRawStream[] =
{
    {
        {
            sizeof(KSDATARANGE_AUDIO),
            KSDATARANGE_ATTRIBUTES,         // An attributes list follows this data range
            0,
            0,
            STATICGUIDOF(KSDATAFORMAT_TYPE_AUDIO),
            STATICGUIDOF(KSDATAFORMAT_SUBTYPE_PCM),
            STATICGUIDOF(KSDATAFORMAT_SPECIFIER_WAVEFORMATEX)
        },
        MICJACKSAI_RAW_CHANNELS,
        MICJACKSAI_32_BITS_PER_SAMPLE_PCM,
        MICJACKSAI_32_BITS_PER_SAMPLE_PCM,
        MICJACKSAI_RAW_SAMPLE_RATE,
        MICJACKSAI_RAW_SAMPLE_RATE
    },
};

static
PKSDATARANGE MicJackSaiPinDataRangePointersStream[] =
{
    // All supported device formats should be listed in the DataRange.
    PKSDATARANGE(&MicJackSaiPinDataRangesRawStream[0]),
    PKSDATARANGE(&PinDataRangeAttributeList),
};

//=============================================================================
static
KSDATARANGE MicJackSaiPinDataRangesBridge[] =
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
PKSDATARANGE MicJackSaiPinDataRangePointersBridge[] =
{
    &MicJackSaiPinDataRangesBridge[0]
};

//=============================================================================
static
PCPIN_DESCRIPTOR MicJackSaiWaveMiniportPins[] =
{
    // Wave In Bridge Pin (Capture - From Topology) KSPIN_WAVE_BRIDGE
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
            SIZEOF_ARRAY(MicJackSaiPinDataRangePointersBridge),
            MicJackSaiPinDataRangePointersBridge,
            KSPIN_DATAFLOW_IN,
            KSPIN_COMMUNICATION_NONE,
            &KSCATEGORY_AUDIO,
            NULL,
            0
        }
    },
    // Wave In Streaming Pin (Capture) KSPIN_WAVE_HOST
    {
        MICJACKSAI_MAX_INPUT_STREAMS,
        MICJACKSAI_MAX_INPUT_STREAMS,
        0,
        NULL,
        {
            0,
            NULL,
            0,
            NULL,
            SIZEOF_ARRAY(MicJackSaiPinDataRangePointersStream),
            MicJackSaiPinDataRangePointersStream,
            KSPIN_DATAFLOW_OUT,
            KSPIN_COMMUNICATION_SINK,
            &KSCATEGORY_AUDIO,
            &KSAUDFNAME_RECORDING_CONTROL,
            0
        }
    }
};

//=============================================================================
static
PCNODE_DESCRIPTOR MicJackSaiWaveMiniportNodes[] =
{
    // KSNODE_WAVE_ADC
    {
        0,                      // Flags
        NULL,                   // AutomationTable
        &KSNODETYPE_ADC,        // Type
        NULL                    // Name
    }
};

//=============================================================================
static
PCCONNECTION_DESCRIPTOR MicJackSaiWaveMiniportConnections[] =
{
    { PCFILTER_NODE,        KSPIN_WAVE_BRIDGE,      KSNODE_WAVE_ADC,     1 },
    { KSNODE_WAVE_ADC,      0,                      PCFILTER_NODE,       KSPIN_WAVEIN_HOST },
};

//=============================================================================

static
CSAUDIORK3X_PROPERTY_ITEM PropertiesMicJackSaiWaveFilter[] =
{
    {
        {
            &KSPROPSETID_General,
            KSPROPERTY_GENERAL_COMPONENTID,
            KSPROPERTY_TYPE_GET | KSPROPERTY_TYPE_BASICSUPPORT,
            PropertyHandler_WaveFilter,
        },
        0,
        0,
        NULL,
        NULL,
        NULL,
        NULL,
        0
    },
    {
        {
            &KSPROPSETID_Pin,
            KSPROPERTY_PIN_PROPOSEDATAFORMAT,
            KSPROPERTY_TYPE_SET | KSPROPERTY_TYPE_BASICSUPPORT,
            PropertyHandler_WaveFilter,
        },
        0,
        0,
        NULL,
        NULL,
        NULL,
        NULL,
        0
    },
    {
        {
            &KSPROPSETID_Pin,
            KSPROPERTY_PIN_PROPOSEDATAFORMAT2,
            KSPROPERTY_TYPE_GET | KSPROPERTY_TYPE_BASICSUPPORT,
            PropertyHandler_WaveFilter,
        },
        0,
        0,
        NULL,
        NULL,
        NULL,
        NULL,
        0
    },
};

DEFINE_PCAUTOMATION_TABLE_PROP(AutomationMicJackSaiWaveFilter, PropertiesMicJackSaiWaveFilter);

//=============================================================================
static
PCFILTER_DESCRIPTOR MicJackSaiWaveMiniportFilterDescriptor =
{
    0,                                              // Version
    &AutomationMicJackSaiWaveFilter,                  // AutomationTable
    sizeof(PCPIN_DESCRIPTOR),                       // PinSize
    SIZEOF_ARRAY(MicJackSaiWaveMiniportPins),         // PinCount
    MicJackSaiWaveMiniportPins,                       // Pins
    sizeof(PCNODE_DESCRIPTOR),                      // NodeSize
    SIZEOF_ARRAY(MicJackSaiWaveMiniportNodes),        // NodeCount
    MicJackSaiWaveMiniportNodes,                      // Nodes
    SIZEOF_ARRAY(MicJackSaiWaveMiniportConnections),  // ConnectionCount
    MicJackSaiWaveMiniportConnections,                // Connections
    0,                                              // CategoryCount
    NULL                                            // Categories  - use defaults (audio, render, capture)
};

#endif // _CSAUDIORK3X_MICJACKSAIWAVTABLE_H_
