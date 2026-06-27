/**
	@file OAL_Helper.cpp
	@author Luis Rodero
	@date 2006-10-02
	@version 0.1
    Set of Helper functions
*/

#include "OpenAL/OAL_Helper.h"
#include "OpenAL/OAL_Device.h"

ALenum geALErrorState = AL_NO_ERROR;
ALCenum geALCErrorState = ALC_NO_ERROR;

///////////////////////////////////////////////////////////
//
// Wide char to byte char and back again convertion utils
///////////////////////////////////////////////////////////

///////////////////////////////////////////////////////////
//
//
///////////////////////////////////////////////////////////

bool OAL_GetALError()
{
	geALErrorState = alGetError();
	return (geALErrorState != AL_NO_ERROR);
}

bool OAL_GetALCError()
{
	ALCcontext *ctx = alcGetCurrentContext();
	if (ctx==NULL) return false;
	geALCErrorState = alcGetError(alcGetContextsDevice(ctx));
	return (geALCErrorState != ALC_NO_ERROR);
}

///////////////////////////////////////////////////////////
//
//
///////////////////////////////////////////////////////////

std::string OAL_GetALErrorString( )
{
	switch (geALErrorState)
	{
	case AL_INVALID_VALUE:
		return std::string("AL_INVALID_VALUE");
	case AL_INVALID_ENUM:
		return std::string("AL_INVALID_ENUM");
	case AL_INVALID_NAME:
		return std::string("AL_INVALID_NAME");
	case AL_INVALID_OPERATION:
		return std::string("AL_INVALID_OPERATION");
	case AL_NO_ERROR:
		return std::string("AL_NO_ERROR");
	default:
		break;
	}
	return std::string("AL_NO_ERROR");
}

std::string OAL_GetALCErrorString( )
{
	switch (geALCErrorState)
	{
		case ALC_NO_ERROR:
			return std::string("ALC_NO_ERROR");
		case ALC_INVALID_DEVICE:
			return std::string("ALC_INVALID_DEVICE");
		case ALC_INVALID_CONTEXT:
			return std::string("ALC_INVALID_CONTEXT");
		case ALC_INVALID_ENUM:
			return std::string("ALC_INVALID_ENUM");
		case ALC_INVALID_VALUE:
			return std::string("ALC_INVALID_VALUE");
		case ALC_OUT_OF_MEMORY:
			return std::string("ALC_OUT_OF_MEMORY");
		default:
            break;
	}
	return std::string("ALC_NO_ERROR");
}

///////////////////////////////////////////////////////////
//
//
///////////////////////////////////////////////////////////

void ClearALErrors(const std::string& asFunction)
{
	ALenum eStatus = alGetError();
	//if ( eStatus != AL_NO_ERROR )
	//	OAL_Log(2, "%s ClearALErrors raised %d\n", asFunction.c_str(), eStatus );
}

///////////////////////////////////////////////////////////
//
//
///////////////////////////////////////////////////////////


bool CheckALErrors(const std::string& asFunc1, const std::string& asFunc2)
{
	bool bErrorOccurred = OAL_GetALError();
	//if ( (bErrorOccured) && (cOAL_Device::IsLogEnabled()))
		//OAL_Log(2,"%s: %s raised %s\n", asFunc1.c_str(), asFunc2.c_str(), OAL_GetALErrorString().c_str() );
	return bErrorOccurred;
}

///////////////////////////////////////////////////////////
//
//
///////////////////////////////////////////////////////////

void ClearALCErrors(const std::string& asFunction)
{
    ALCcontext *ctx = alcGetCurrentContext();
    if (ctx == NULL) return;
	ALCenum eStatus = alcGetError(alcGetContextsDevice(ctx));
	//if ( eStatus != ALC_NO_ERROR )
	//s	OAL_Log(2, "%s ClearALCErrors raised %d\n", asFunction.c_str(), eStatus );
}

///////////////////////////////////////////////////////////
//
//
///////////////////////////////////////////////////////////

bool CheckALCErrors(const std::string& asFunction)
{
	bool bErrorOccurred = OAL_GetALCError();
	//if ( (bErrorOccured) && (gbLogSounds) )
	//	OAL_Log(2,"%s CheckALCErrors raised %s\n", asFunction.c_str(), OAL_GetALCErrorString().c_str() );
	return bErrorOccurred;
}

