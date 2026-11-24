
/*++

Copyright (c) Microsoft Corporation All Rights Reserved

Module Name:

    minitopo.h

Abstract:

    Declaration of topology miniport.
--*/

#ifndef _CSAUDIORK3X_MINTOPO_H_
#define _CSAUDIORK3X_MINTOPO_H_

#include "basetopo.h"

//=============================================================================
// Classes
//=============================================================================

///////////////////////////////////////////////////////////////////////////////
// CMiniportTopology 
//   

class CMiniportTopology : 
    public CMiniportTopologyCsAudioRk3x,
    public IMiniportTopology,
    public CUnknown
{
  private:
    eDeviceType             m_DeviceType;
    BOOLEAN                 m_JackPresence;     // ENDPOINT_JACK_PRESENCE
    BOOLEAN                 m_JackRegistered;

    union {
        PVOID               m_DeviceContext;
    };

public:
    DECLARE_STD_UNKNOWN();
    CMiniportTopology
    (
        _In_opt_    PUNKNOWN                UnknownOuter,
        _In_        PCFILTER_DESCRIPTOR    *FilterDesc,
        _In_        USHORT                  DeviceMaxChannels,
        _In_        eDeviceType             DeviceType, 
        _In_opt_    PVOID                   DeviceContext
    )
    : CUnknown(UnknownOuter),
      CMiniportTopologyCsAudioRk3x(FilterDesc, DeviceMaxChannels),
      m_DeviceType(DeviceType),
      m_JackPresence(FALSE),
      m_JackRegistered(FALSE),
      m_DeviceContext(DeviceContext)
    {
    }

    ~CMiniportTopology();

    IMP_IMiniportTopology;

    NTSTATUS PropertyHandlerJackDescription
    (
        _In_        PPCPROPERTY_REQUEST                         PropertyRequest,
        _In_        ULONG                                       cJackDescriptions,
        _In_reads_(cJackDescriptions) PKSJACK_DESCRIPTION       *JackDescriptions
    );

    NTSTATUS PropertyHandlerJackDescription2
    ( 
        _In_        PPCPROPERTY_REQUEST                         PropertyRequest,
        _In_        ULONG                                       cJackDescriptions,
        _In_reads_(cJackDescriptions) PKSJACK_DESCRIPTION       *JackDescriptions,
        _In_        DWORD                                       JackCapabilities
    );
    
    PVOID GetDeviceContext() { return m_DeviceContext;  }

    // Set before Init for an endpoint with ENDPOINT_JACK_PRESENCE.
    VOID SetJackPresence(BOOLEAN JackPresence) { m_JackPresence = JackPresence; }

    // The headphone jack as the codec last reported it (plugged until then).
    BOOLEAN IsJackConnected()
    {
        return m_AdapterCommon ? m_AdapterCommon->JackIsConnected() : TRUE;
    }
};

typedef CMiniportTopology *PCMiniportTopology;

#endif // _CSAUDIORK3X_MINTOPO_H_
