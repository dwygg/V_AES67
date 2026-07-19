/*++
Copyright (c) 1997-2000  Microsoft Corporation All Rights Reserved

Module Name:
    hw.cpp

Abstract:
    Implementation of AES67 HW class. 
    AES67 HW has an array for storing mixer and volume settings
    for the topology.
--*/
#include "aes67driver.h"
#include "hw.h"

//=============================================================================
// CAES67HW
//=============================================================================

//=============================================================================
#pragma code_seg("PAGE")
CAES67HW::CAES67HW() : m_ulMux(0), m_bDevSpecific(FALSE), m_iDevSpecific(0), m_uiDevSpecific(0)
/*++
Routine Description:
  Constructor for AES67HW. 

Arguments:

Return Value:
  void
--*/
{
    PAGED_CODE();
    
    MixerReset();
} // CAES67HW
#pragma code_seg()

//=============================================================================
BOOL CAES67HW::bGetDevSpecific()
/*++
Routine Description:
  Gets the HW (!) Device Specific info

Arguments:
  N/A

Return Value:
  True or False (in this example).
--*/
{
    return m_bDevSpecific;
} // bGetDevSpecific

//=============================================================================
void CAES67HW::bSetDevSpecific(IN BOOL bDevSpecific)
/*++
Routine Description:
  Sets the HW (!) Device Specific info

Arguments:
  fDevSpecific - true or false for this example.

Return Value:
  void
--*/
{
    m_bDevSpecific = bDevSpecific;
} // bSetDevSpecific

//=============================================================================
INT CAES67HW::iGetDevSpecific()
/*++
Routine Description:
  Gets the HW (!) Device Specific info

Arguments:
  N/A

Return Value:
  int (in this example).
--*/
{
    return m_iDevSpecific;
} // iGetDevSpecific

//=============================================================================
void CAES67HW::iSetDevSpecific(IN INT iDevSpecific)
/*++
Routine Description:
  Sets the HW (!) Device Specific info

Arguments:
  fDevSpecific - true or false for this example.

Return Value:
  void
--*/
{
    m_iDevSpecific = iDevSpecific;
} // iSetDevSpecific

//=============================================================================
UINT CAES67HW::uiGetDevSpecific()
/*++
Routine Description:
  Gets the HW (!) Device Specific info

Arguments:
  N/A

Return Value:
  UINT (in this example).
--*/
{
    return m_uiDevSpecific;
} // uiGetDevSpecific

//=============================================================================
void CAES67HW::uiSetDevSpecific(IN UINT uiDevSpecific)
/*++
Routine Description:
  Sets the HW (!) Device Specific info

Arguments:
  uiDevSpecific - int for this example.

Return Value:
  void
--*/
{
    m_uiDevSpecific = uiDevSpecific;
} // uiSetDevSpecific


//=============================================================================
BOOL CAES67HW::GetMixerMute(IN ULONG ulNode)
/*++
Routine Description:
  Gets the HW (!) mute levels for AES67

Arguments:
  ulNode - topology node id

Return Value:
  mute setting
--*/
{
    if (ulNode < MAX_TOPOLOGY_NODES) {
        return m_MuteControls[ulNode];
    }

    return 0;
} // GetMixerMute

//=============================================================================
ULONG CAES67HW::GetMixerMux()
/*++
Routine Description:
  Return the current mux selection

Arguments:

Return Value:
  ULONG
--*/
{
    return m_ulMux;
} // GetMixerMux

//=============================================================================
LONG CAES67HW::GetMixerVolume(   
    IN  ULONG ulNode,
    IN  LONG  lChannel
)
/*++
Routine Description:
  Gets the HW (!) volume for AES67.

Arguments:
  ulNode - topology node id
  lChannel - which channel are we setting?

Return Value:
  LONG - volume level
--*/
{
    UNREFERENCED_PARAMETER(lChannel);

    if (ulNode < MAX_TOPOLOGY_NODES) {
        return m_VolumeControls[ulNode];
    }

    return 0;
} // GetMixerVolume

//=============================================================================
#pragma code_seg("PAGE")
void CAES67HW::MixerReset()
/*++
Routine Description:
  Resets the mixer registers.

Arguments:

Return Value:
  void
--*/
{
    PAGED_CODE();
    
    RtlFillMemory(m_VolumeControls, sizeof(LONG) * MAX_TOPOLOGY_NODES, 0xFF);
    RtlFillMemory(m_MuteControls, sizeof(BOOL) * MAX_TOPOLOGY_NODES, TRUE);
    
    // BUGBUG change this depending on the topology
    m_ulMux = 2;
} // MixerReset
#pragma code_seg()

//=============================================================================
void CAES67HW::SetMixerMute(
    IN  ULONG                   ulNode,
    IN  BOOL                    fMute
)
/*++
Routine Description:
  Sets the HW (!) mute levels for AES67

Arguments:
  ulNode - topology node id
  fMute - mute flag

Return Value:
  void
--*/
{
    if (ulNode < MAX_TOPOLOGY_NODES) {
        m_MuteControls[ulNode] = fMute;
    }
} // SetMixerMute

//=============================================================================
void CAES67HW::SetMixerMux(
    IN  ULONG                   ulNode
)
/*++
Routine Description:
  Sets the HW (!) mux selection

Arguments:
  ulNode - topology node id

Return Value:
  void
--*/
{
    m_ulMux = ulNode;
} // SetMixMux

//=============================================================================
void CAES67HW::SetMixerVolume(   
    IN  ULONG                   ulNode,
    IN  LONG                    lChannel,
    IN  LONG                    lVolume
)
/*++
Routine Description:
  Sets the HW (!) volume for AES67.

Arguments:
  ulNode - topology node id
  lChannel - which channel are we setting?
  lVolume - volume level

Return Value:
  void
--*/
{
    UNREFERENCED_PARAMETER(lChannel);

    if (ulNode < MAX_TOPOLOGY_NODES) {
        m_VolumeControls[ulNode] = lVolume;
    }
} // SetMixerVolume
