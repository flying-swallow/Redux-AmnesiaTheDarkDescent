/**
	@file OAL_Types.h
	@author Luis Rodero
	@date 2006-10-02
	@version 0.1
	Types for OpenAL
*/


#ifndef _OAL_TYPES_H
#define _OAL_TYPES_H


////////////////////////////////
// Extension stuff

#define NUM_EXTENSIONS 13
#define NUM_ALC_EXTENSIONS 4
//#define NUM_AL_EXTENSIONS 9

#define OAL_ALC_EXT_CAPTURE 0
#define OAL_ALC_EXT_EFX 1
#define OAL_AL_EXT_OFFSET 2
#define OAL_AL_EXT_LINEAR_DISTANCE 3
#define OAL_AL_EXT_EXPONENT_DISTANCE 4
#define OAL_EAX 5
#define OAL_EAX2_0 6
#define OAL_EAX3_0 7
#define OAL_EAX4_0 8
#define OAL_EAX5_0 9
#define OAL_EAX_RAM 10

////////////////////////////////////////////////////
// Specifies the size of the memory unit in streaming ( currently 4 KB ) 
// (buffer sizes are set using multiples of this unit)
#define STREAMING_BLOCK_SIZE 4096
//#define BUFFERCOUNT 8

#if defined(__ppc__)
	#define SYS_ENDIANNESS 1
#else
	#define SYS_ENDIANNESS 0
#endif

#define OAL_FREE	-3
#define OAL_ALL		-2

#include <string>
#include <vector>
#include <list>

#include <AL/al.h>
#include <AL/alc.h>
#include <AL/alut.h>
//#include <AL/alext.h>
#include <AL/efx.h>
#include <AL/efx-creative.h>

class cOAL_Source;
class cOAL_Sample;
class cOAL_Stream;
class cOAL_Buffer;

typedef std::vector<cOAL_Source*>			tSourceVec;
typedef tSourceVec::iterator			tSourceVecIt;

//typedef set<cOAL_Source*>				tSourceSet;
//typedef set<cOAL_Source*>::iterator		tSourceSetIt;

typedef std::list<cOAL_Source*>				tSourceList;
typedef tSourceList::iterator			tSourceListIt;

typedef std::list<cOAL_Sample*>				tSampleList;
typedef tSampleList::iterator			tSampleListIt;

typedef std::list<cOAL_Stream*>				tStreamList;
typedef tStreamList::iterator			tStreamListIt;

typedef std::vector<cOAL_Buffer*>			tBufferVec;
typedef tBufferVec::iterator			tBufferVecIt;


class cOAL_EffectSlot;

class cOAL_Filter;
class cOAL_Filter_LowPass;
class cOAL_Filter_HighPass;
class cOAL_Filter_BandPass;

class cOAL_Effect;
class cOAL_Effect_Reverb;

class cOAL_SourceSend;

typedef std::vector<cOAL_EffectSlot*>			tSlotVector;
typedef std::vector<cOAL_EffectSlot*>::iterator	tSlotVectorIt;

typedef std::list<cOAL_Effect*>					tOALEffectList;
typedef std::list<cOAL_Effect*>::iterator		tOALEffectListIt;

typedef std::list<cOAL_Filter*>					tOALFilterList;
typedef std::list<cOAL_Filter*>::iterator		tOALFilterListIt;

typedef std::vector<cOAL_SourceSend*>			tSendVector;
typedef std::vector<cOAL_SourceSend*>::iterator	tSendVectorIt;

typedef enum 
{
	eOAL_SourceStatus_Free,
	eOAL_SourceStatus_Busy,
	eOAL_SourceStatus_Busy_BufferUnderrun,
	eOAL_SourceStatus_Default
} eOAL_SourceStatus;

typedef enum
{
	eOAL_DistanceModel_Inverse,
	eOAL_DistanceModel_Inverse_Clamped,
	eOAL_DistanceModel_Linear,
	eOAL_DistanceModel_Linear_Clamped,
	eOAL_DistanceModel_Exponent,
	eOAL_DistanceModel_Exponent_Clamped,
	eOAL_DistanceModel_None,
	eOAL_DistanceModel_Default,
} eOAL_DistanceModel;



//////////////////////////////////////////
/// EFX Stuff
//////////////////////////////////////////


// Function pointers 
////////////////////

// Effect slots

extern LPALGENAUXILIARYEFFECTSLOTS oalw_alGenAuxiliaryEffectSlots;
extern LPALDELETEAUXILIARYEFFECTSLOTS oalw_alDeleteAuxiliaryEffectSlots;
extern LPALISAUXILIARYEFFECTSLOT oalw_alIsAuxiliaryEffectSlot;
extern LPALAUXILIARYEFFECTSLOTI oalw_alAuxiliaryEffectSloti;
extern LPALAUXILIARYEFFECTSLOTIV oalw_alAuxiliaryEffectSlotiv;
extern LPALAUXILIARYEFFECTSLOTF oalw_alAuxiliaryEffectSlotf;
extern LPALAUXILIARYEFFECTSLOTFV oalw_alAuxiliaryEffectSlotfv;
extern LPALGETAUXILIARYEFFECTSLOTI oalw_alGetAuxiliaryEffectSloti;
extern LPALGETAUXILIARYEFFECTSLOTIV oalw_alGetAuxiliaryEffectSlotiv;
extern LPALGETAUXILIARYEFFECTSLOTF oalw_alGetAuxiliaryEffectSlotf;
extern LPALGETAUXILIARYEFFECTSLOTFV oalw_alGetAuxiliaryEffectSlotfv;

// Effects

extern LPALGENEFFECTS oalw_alGenEffects;
extern LPALDELETEEFFECTS oalw_alDeleteEffects;
extern LPALISEFFECT oalw_alIsEffect;
extern LPALEFFECTI oalw_alEffecti;
extern LPALEFFECTIV oalw_alEffectiv;
extern LPALEFFECTF oalw_alEffectf;
extern LPALEFFECTFV oalw_alEffectfv;
extern LPALGETEFFECTI oalw_alGetEffecti;
extern LPALGETEFFECTIV oalw_alGetEffectiv;
extern LPALGETEFFECTF oalw_alGetEffectf;
extern LPALGETEFFECTFV oalw_alGetEffectfv;

// Filters

extern LPALGENFILTERS oalw_alGenFilters;
extern LPALDELETEFILTERS oalw_alDeleteFilters;
extern LPALISFILTER oalw_alIsFilter;
extern LPALFILTERI oalw_alFilteri;
extern LPALFILTERIV oalw_alFilteriv;
extern LPALFILTERF oalw_alFilterf;
extern LPALFILTERFV oalw_alFilterfv;
extern LPALGETFILTERI oalw_alGetFilteri;
extern LPALGETFILTERIV oalw_alGetFilteriv;
extern LPALGETFILTERF oalw_alGetFilterf;
extern LPALGETFILTERFV oalw_alGetFilterfv;


typedef enum _eOALFilterType
{
	eOALFilterType_LowPass,
	eOALFilterType_HighPass,
	eOALFilterType_BandPass,
	eOALFilterType_Null,
} eOALFilterType;


#endif	// _OAL_TYPES_H
