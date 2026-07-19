/*++
Copyright (c) 1997-2000  Microsoft Corporation All Rights Reserved

Module Name:
    hw.h

Abstract:
    Declaration of AES67 HW class. 
    AES67 HW has an array for storing mixer and volume settings
    for the topology.

--*/

#ifndef _AES67_HW_H_
#define _AES67_HW_H_

//=============================================================================
// Defines
//=============================================================================
// BUGBUG we should dynamically allocate this...
#define MAX_TOPOLOGY_NODES      20

//=============================================================================
// Classes
//=============================================================================
///////////////////////////////////////////////////////////////////////////////
// CAES67HW
// This class represents virtual AES67 HW. An array representing volume
// registers and mute registers.

class CAES67HW {
protected:
    BOOL  m_MuteControls[MAX_TOPOLOGY_NODES];
    LONG  m_VolumeControls[MAX_TOPOLOGY_NODES];
    ULONG m_ulMux;            // Mux selection
    BOOL  m_bDevSpecific;
    INT   m_iDevSpecific;
    UINT  m_uiDevSpecific;

public:
    CAES67HW();
    
    void MixerReset();
    BOOL bGetDevSpecific();
    void bSetDevSpecific(IN BOOL bDevSpecific);

    INT  iGetDevSpecific();
    void iSetDevSpecific(IN INT iDevSpecific);

    UINT  uiGetDevSpecific();
    void  uiSetDevSpecific(IN UINT uiDevSpecific);
	
    BOOL  GetMixerMute(IN ULONG ulNode);
    void  SetMixerMute(IN ULONG ulNode, IN BOOL fMute);
    ULONG GetMixerMux();
    void  SetMixerMux(IN ULONG ulNode);
    LONG  GetMixerVolume(IN ULONG ulNode, IN LONG lChannel);
    void  SetMixerVolume(IN ULONG ulNode, IN LONG lChannel, IN LONG lVolume);
};
typedef CAES67HW* PCAES67HW;

#endif
