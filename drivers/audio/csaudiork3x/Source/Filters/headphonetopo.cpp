/*++

Copyright (c) Microsoft Corporation All Rights Reserved

Module Name:

    headphonetopo.cpp

Abstract:

    Implementation of topology miniport for the headphone (jack).
--*/

#pragma warning (disable : 4127)

#include "definitions.h"
#include "endpoints.h"
#include "mintopo.h"
#include "headphonetopo.h"
#include "headphonetoptable.h"


#pragma code_seg("PAGE")
//=============================================================================
NTSTATUS
PropertyHandler_HeadphoneTopoFilter
( 
    _In_ PPCPROPERTY_REQUEST      PropertyRequest 
)
/*++

Routine Description:

  Redirects property request to miniport object

Arguments:

  PropertyRequest - 

Return Value:

  NT status code.

--*/
{
    PAGED_CODE();

    ASSERT(PropertyRequest);

    DPF_ENTER(("[PropertyHandler_HeadphoneTopoFilter]"));

    // PropertryRequest structure is filled by portcls. 
    // MajorTarget is a pointer to miniport object for miniports.
    //
    NTSTATUS            ntStatus = STATUS_INVALID_DEVICE_REQUEST;
    PCMiniportTopology  pMiniport = (PCMiniportTopology)PropertyRequest->MajorTarget;

    if (IsEqualGUIDAligned(*PropertyRequest->PropertyItem->Set, KSPROPSETID_Jack))
    {
        if (PropertyRequest->PropertyItem->Id == KSPROPERTY_JACK_DESCRIPTION)
        {
            ntStatus = pMiniport->PropertyHandlerJackDescription(
                PropertyRequest,
                ARRAYSIZE(HeadphoneJackDescriptions),
                HeadphoneJackDescriptions
                );
        }
        else if (PropertyRequest->PropertyItem->Id == KSPROPERTY_JACK_DESCRIPTION2)
        {
            ntStatus = pMiniport->PropertyHandlerJackDescription2(
                PropertyRequest,
                ARRAYSIZE(HeadphoneJackDescriptions),
                HeadphoneJackDescriptions,
                0 // jack capabilities
                );
        }
    }

    return ntStatus;
} // PropertyHandler_HeadphoneTopoFilter

//=============================================================================
NTSTATUS
PropertyHandler_HeadphoneTopology
(
    _In_ PPCPROPERTY_REQUEST      PropertyRequest
)
/*++

Routine Description:

  Redirects property request to miniport object

Arguments:

  PropertyRequest -

Return Value:

  NT status code.

--*/
{
    PAGED_CODE();

    ASSERT(PropertyRequest);

    DPF_ENTER(("[PropertyHandler_HeadphoneTopology]"));

    // PropertryRequest structure is filled by portcls. 
    // MajorTarget is a pointer to miniport object for miniports.
    //
    PCMiniportTopology pMiniport = (PCMiniportTopology)PropertyRequest->MajorTarget;

    return pMiniport->PropertyHandlerGeneric(PropertyRequest);
} // PropertyHandler_HeadphoneTopology

#ifndef JACKDESC2_PRESENCE_DETECT_CAPABILITY
#define JACKDESC2_PRESENCE_DETECT_CAPABILITY 0x00000001
#endif

//=============================================================================
NTSTATUS
PropertyHandler_HeadphoneSaiTopoFilter
(
    _In_ PPCPROPERTY_REQUEST      PropertyRequest
)
/*++

Routine Description:

  The jack properties of the RK3576 SAI headphone topology: as
  PropertyHandler_HeadphoneTopoFilter, with IsConnected from the codec's
  jack detection and the presence-detect capability set.

--*/
{
    PAGED_CODE();

    ASSERT(PropertyRequest);

    DPF_ENTER(("[PropertyHandler_HeadphoneSaiTopoFilter]"));

    NTSTATUS            ntStatus = STATUS_INVALID_DEVICE_REQUEST;
    PCMiniportTopology  pMiniport = (PCMiniportTopology)PropertyRequest->MajorTarget;

    if (IsEqualGUIDAligned(*PropertyRequest->PropertyItem->Set, KSPROPSETID_Jack))
    {
        KSJACK_DESCRIPTION  jack = HeadphoneJackDescBridge;
        PKSJACK_DESCRIPTION jackDescriptions[] = { NULL, &jack };

        jack.IsConnected = pMiniport->IsJackConnected();

        if (PropertyRequest->PropertyItem->Id == KSPROPERTY_JACK_DESCRIPTION)
        {
            ntStatus = pMiniport->PropertyHandlerJackDescription(
                PropertyRequest,
                ARRAYSIZE(jackDescriptions),
                jackDescriptions
                );
        }
        else if (PropertyRequest->PropertyItem->Id == KSPROPERTY_JACK_DESCRIPTION2)
        {
            ntStatus = pMiniport->PropertyHandlerJackDescription2(
                PropertyRequest,
                ARRAYSIZE(jackDescriptions),
                jackDescriptions,
                JACKDESC2_PRESENCE_DETECT_CAPABILITY
                );
        }
    }

    return ntStatus;
} // PropertyHandler_HeadphoneSaiTopoFilter

//=============================================================================
NTSTATUS
EventHandler_HeadphoneSaiJack
(
    _In_ PPCEVENT_REQUEST         EventRequest
)
/*++

Routine Description:

  KSEVENT_PINCAPS_JACKINFOCHANGE on the SAI headphone topology: the port
  keeps the list, CCsAudioRk3xHW::jack_report generates the event.

--*/
{
    PAGED_CODE();

    ASSERT(EventRequest);

    DPF_ENTER(("[EventHandler_HeadphoneSaiJack]"));

    PCMiniportTopology pMiniport = (PCMiniportTopology)EventRequest->MajorTarget;

    switch (EventRequest->Verb)
    {
    case PCEVENT_VERB_SUPPORT:
        return STATUS_SUCCESS;

    case PCEVENT_VERB_ADD:
        if (EventRequest->EventEntry != NULL &&
            EventRequest->EventItem->Id == KSEVENT_PINCAPS_JACKINFOCHANGE)
        {
            pMiniport->AddEventToEventList(EventRequest->EventEntry);
            return STATUS_SUCCESS;
        }
        return STATUS_INVALID_PARAMETER;

    case PCEVENT_VERB_REMOVE:
        // The port removes the entry from its list itself.
        return STATUS_SUCCESS;

    default:
        return STATUS_INVALID_PARAMETER;
    }
} // EventHandler_HeadphoneSaiJack

#pragma code_seg()
