/*
 * Copyright © 2009-2020 Frictional Games
 * 
 * This file is part of Amnesia: The Dark Descent.
 * 
 * Amnesia: The Dark Descent is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version. 

 * Amnesia: The Dark Descent is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with Amnesia: The Dark Descent.  If not, see <https://www.gnu.org/licenses/>.
 */

#include "resources/EngineFileLoading.h"
#include "graphics/Image.h"

#include <tinyxml2.h>
#include "resources/XmlHelper.h"
#include "resources/Resources.h"
#include "resources/TextureManager.h"
#include "resources/MaterialManager.h"

#include "math/Math.h"

#include "system/String.h"

#include "scene/World.h"
#include "scene/LightPoint.h"
#include "scene/LightSpot.h"
#include "scene/LightArea.h"
#include "scene/MeshEntity.h"
#include "scene/SoundEntity.h"
#include "scene/ParticleEmitter.h"
#include "scene/ParticleSystem.h"
#include "scene/BillBoard.h"
#include "scene/Beam.h"
#include "scene/GuiSetEntity.h"
#include "scene/RopeEntity.h"
#include "scene/FogArea.h"

#include "graphics/Graphics.h"
#include "graphics/LowLevelGraphics.h"
#include "graphics/VertexBuffer.h"
#include "graphics/Mesh.h"
#include "graphics/SubMesh.h"


namespace hpl {

	//////////////////////////////////////////////////////////////////////////
	// DEFINES
	//////////////////////////////////////////////////////////////////////////
	
	//-----------------------------------------------------------------------

	#define kBeginWorldEntityLoad()		\
		tString sName = GetAttributeString(apElement, "Name");
	
	#define kEndWorldEntityLoad(pEntity)		\
		SetupWorldEntity(pEntity, apElement);	\
		return pEntity;

	//-----------------------------------------------------------------------

	//////////////////////////////////////////////////////////////////////////
	// CREATE ENTITIES
	//////////////////////////////////////////////////////////////////////////

	//-----------------------------------------------------------------------

	cFogArea* cEngineFileLoading::LoadFogArea(tinyxml2::XMLElement* apElement, const tString& asNamePrefix, cWorld *apWorld, bool abStatic)
	{
		kBeginWorldEntityLoad();

		cFogArea *pFog = apWorld->CreateFogArea(asNamePrefix+sName, abStatic);

		if(pFog)
		{
			pFog->SetColor(GetAttributeColor(apElement, "Color",cColor(1,1)));
			pFog->SetStart(GetAttributeFloat(apElement, "Start", 0));
			pFog->SetEnd(GetAttributeFloat(apElement, "End", 0));
			pFog->SetFalloffExp(GetAttributeFloat(apElement, "FalloffExp", 0));
			pFog->SetShowBacksideWhenInside(GetAttributeBool(apElement, "ShownBacksideWhenInside", true));
			pFog->SetShowBacksideWhenOutside(GetAttributeBool(apElement, "ShownBacksideWhenOutside", true));
		}

		kEndWorldEntityLoad(pFog);
	}

	//-----------------------------------------------------------------------

	cParticleSystem* cEngineFileLoading::LoadParticleSystem(tinyxml2::XMLElement* apElement, const tString& asNamePrefix, cWorld *apWorld)
	{
		kBeginWorldEntityLoad();

		tString sFile = GetAttributeString(apElement, "File");

		cParticleSystem *pPS = apWorld->CreateParticleSystem(asNamePrefix+sName,sFile,1);

		if(pPS)
		{
			pPS->SetColor(GetAttributeColor(apElement, "Color",cColor(1,1)));
			pPS->SetFadeAtDistance(GetAttributeBool(apElement, "FadeAtDistance", false));
			pPS->SetMinFadeDistanceStart(GetAttributeFloat(apElement, "MinFadeDistanceStart"));
			pPS->SetMinFadeDistanceEnd(GetAttributeFloat(apElement, "MinFadeDistanceEnd"));
			pPS->SetMaxFadeDistanceStart(GetAttributeFloat(apElement, "MaxFadeDistanceStart"));
			pPS->SetMaxFadeDistanceEnd(GetAttributeFloat(apElement, "MaxFadeDistanceEnd"));
		}
		
		kEndWorldEntityLoad(pPS);
	}
	
	//-----------------------------------------------------------------------

	cSoundEntity* cEngineFileLoading::LoadSound(tinyxml2::XMLElement* apElement, const tString& asNamePrefix, cWorld *apWorld)
	{
		kBeginWorldEntityLoad();

		tString sSoundFile = GetAttributeString(apElement, "SoundEntityFile");
		bool bUseDefault = GetAttributeBool(apElement, "UseDefault");

		cSoundEntity *pSound = apWorld->CreateSoundEntity(asNamePrefix+sName,sSoundFile,false);
		if(pSound==NULL) return NULL;

		if(bUseDefault==false)
		{
			pSound->SetMinDistance(GetAttributeFloat(apElement, "MinDistance"));
			pSound->SetMaxDistance(GetAttributeFloat(apElement, "MaxDistance"));
			pSound->SetVolume(GetAttributeFloat(apElement, "Volume"));
		}


		kEndWorldEntityLoad(pSound);
	}

	
	//-----------------------------------------------------------------------

	static eBillboardType ToBillboardType(const tString& asType)
	{
		if(asType == "Axis") return eBillboardType_Axis;
		if(asType == "Point") return eBillboardType_Point;
		if(asType == "FixedAxis") return eBillboardType_FixedAxis;

		return eBillboardType_Point;
	}

	cBillboard* cEngineFileLoading::LoadBillboard(tinyxml2::XMLElement* apElement, const tString& asNamePrefix, cWorld *apWorld, cResources *apResources, bool abStatic)
	{
		kBeginWorldEntityLoad();

		cVector2f vSize = GetAttributeVector2f(apElement, "BillboardSize");
		tString sMat = GetAttributeString(apElement, "MaterialFile");
		eBillboardType bbType = ToBillboardType(GetAttributeString(apElement, "BillboardType"));

		cBillboard *pBillboard = apWorld->CreateBillboard(asNamePrefix+sName,vSize,bbType,sMat, abStatic);
		if(pBillboard==NULL) return NULL;

		pBillboard->SetForwardOffset(GetAttributeFloat(apElement, "BillboardOffset"));
		pBillboard->SetColor(GetAttributeColor(apElement, "BillboardColor",cColor(1,1)));

		pBillboard->SetIsHalo(GetAttributeBool(apElement, "IsHalo",false));
		pBillboard->SetHaloSourceSize(GetAttributeVector3f(apElement, "HaloSourceSize",1));

		// The ConnectLight attribute is ignored — light-billboard color sync was
		// a fake-bloom hack; the renderer has real bloom now.

		kEndWorldEntityLoad(pBillboard);
	}

	//-----------------------------------------------------------------------
	
	static eShadowMapResolution ToShadowMapResolution(const tString& asType)
	{
		tString sLowType = cString::ToLowerCase(asType);

        if(sLowType == "high") return eShadowMapResolution_High;
		if(sLowType == "medium") return eShadowMapResolution_Medium;
		if(sLowType == "low") return eShadowMapResolution_Low;
		return eShadowMapResolution_High;
	}

	static eTextureAnimMode ToTextureAnimMode(const tString& asType)
	{
		if(cString::ToLowerCase(asType) == "none") return eTextureAnimMode_None;
		else if(cString::ToLowerCase(asType) == "loop") return eTextureAnimMode_Loop;
		else if(cString::ToLowerCase(asType) == "oscillate") return eTextureAnimMode_Oscillate;

		return eTextureAnimMode_None;
	}
	
	iLight* cEngineFileLoading::LoadLight(	tinyxml2::XMLElement* apElement, const tString& asNamePrefix, cWorld *apWorld, cResources *apResources, bool abStatic)
	{
		kBeginWorldEntityLoad();

		iLight *pLight = NULL;

		bool bStatic = abStatic;

		//////////////////////////
		// Spotlightt
		if(tString(apElement->Value()) == "SpotLight")
		{
			cLightSpot *pLightSpot = apWorld->CreateLightSpot(asNamePrefix+sName,"", bStatic);
			pLight = pLightSpot;

			//Frustum related
			pLightSpot->SetFOV(GetAttributeFloat(apElement, "FOV", 1.0f));
			pLightSpot->SetAspect(GetAttributeFloat(apElement, "Aspect", 1.0f));
			pLightSpot->SetNearClipPlane(GetAttributeFloat(apElement, "NearClipPlane", 0.1f));

			//Spot fall off
			tString sSpotFalloffMap = GetAttributeString(apElement, "SpotFalloffMap");
			if(sSpotFalloffMap != "")
			{
				Image *pFalloff = apResources->GetTextureManager()->Create1DImage(sSpotFalloffMap,true).Release();
				if(pFalloff) pLightSpot->SetSpotFalloffMap(pFalloff);
			}
		}
		//////////////////////////
		// Area Light
		else if(tString(apElement->Value()) == "AreaLight")
		{
			cLightArea *pLightArea = apWorld->CreateLightArea(asNamePrefix+sName, bStatic);
			pLight = pLightArea;

			pLightArea->SetWidth(GetAttributeFloat(apElement, "SourceWidth", 1.0f));
			pLightArea->SetHeight(GetAttributeFloat(apElement, "SourceHeight", 1.0f));
			pLightArea->SetBarnDoorAngle(GetAttributeFloat(apElement, "BarnDoorAngle", cMath::ToRad(88.0f)));
			pLightArea->SetBarnDoorLength(GetAttributeFloat(apElement, "BarnDoorLength", 0.0f));

			//Optional source texture (stored in the base gobo slot — a 2D image). Tints emission.
			tString sSourceTex = GetAttributeString(apElement, "SourceTexture");
			if(sSourceTex != "")
			{
				Image *pTex = apResources->GetTextureManager()->Create2DImage(sSourceTex,true).Release();
				if(pTex) pLightArea->SetGoboTexture(pTex);
			}
		}
		//////////////////////////
		// Point Light
		else if(tString(apElement->Value()) == "PointLight")
		{
			cLightPoint *pLightPoint  = apWorld->CreateLightPoint(asNamePrefix+sName,"", bStatic);
			pLight = pLightPoint;
		}
		else
		{
			Error("Unknown light type '%s'\n", apElement->Value());
			return NULL;
		}

		//////////////////////////
		// General properties
		eLightType lightType = pLight->GetLightType();
		const bool bSpotLight = lightType == eLightType_Spot;

		//Spot and point
		if(lightType == eLightType_Point || bSpotLight)
		{
			//Falloff
			tString sFalloffMap = GetAttributeString(apElement, "FalloffMap");
			if(sFalloffMap != "")
			{
				Image *pFalloff = apResources->GetTextureManager()->Create1DImage(sFalloffMap,true).Release();
				if(pFalloff) pLight->SetFalloffMap(pFalloff);
			}

			//Gobo
			tString sGobo = GetAttributeString(apElement, "Gobo","");
			if(sGobo  != "")
			{
				eTextureAnimMode animMode = ToTextureAnimMode(GetAttributeString(apElement, "GoboAnimMode",""));
				float fAnimFrameTime = GetAttributeFloat(apElement, "GoboAnimFrameTime", 1);

				Image *pGoboTex=NULL;
				if(bSpotLight)
				{
					if(animMode == eTextureAnimMode_None)
						pGoboTex = apResources->GetTextureManager()->Create2DImage(sGobo,true).Release();
					else
						pGoboTex = apResources->GetTextureManager()->CreateAnimImage(sGobo, true, eTextureType_2D).Release();
				}
				else
				{
					if(animMode == eTextureAnimMode_None)
						pGoboTex = apResources->GetTextureManager()->CreateCubeMapImage(sGobo,true).Release();
					else
						pGoboTex = apResources->GetTextureManager()->CreateAnimImage(sGobo,true, eTextureType_CubeMap).Release();
				}

				if(pGoboTex)
				{
					pLight->SetGoboTexture(pGoboTex);
					pGoboTex->SetFrameTime(fAnimFrameTime);
				}
			}
		}

		//All types
		pLight->SetCastShadows(GetAttributeBool(apElement, "CastShadows", false));
		pLight->SetDiffuseColor(GetAttributeColor(apElement, "DiffuseColor", cColor(1)));
		pLight->SetDefaultDiffuseColor(pLight->GetDiffuseColor());
		// Light brightness = "Intensity", reach = "Radius". Two input shapes:
		//   - new map: authors "Intensity" + "Radius" (reach) explicitly.
		//   - original/old map: only "Radius", which the PBR model uses directly
		//     AS the intensity (matches the shipped renderer). The cull reach is
		//     derived from intensity: the distance where the brightest channel's
		//     radiance dims to kLightRadianceFloor. Constants mirror
		//     amnesia/slang/Constants.h.
		{
			const float kLightRadianceFloor       = 0.005f;
			const float kPointLightSourceRadiusSq = 0.25f;
			auto sRGBToLinear = [](float c){
				return c <= 0.04045f ? c/12.92f : powf((c+0.055f)/1.055f, 2.4f); };
			// reach where color·intensity·1/(d²+srcSq) falls to the radiance floor.
			auto deriveReach = [&](float afIntensity){
				const cColor c = pLight->GetDiffuseColor();
				const float maxC = std::max(sRGBToLinear(c.r),
									std::max(sRGBToLinear(c.g), sRGBToLinear(c.b)));
				const float reachSq = maxC > 0.f
					? maxC * afIntensity / kLightRadianceFloor - kPointLightSourceRadiusSq
					: 0.f;
				return reachSq > 0.f ? sqrtf(reachSq) : afIntensity;
			};

			float fIntensity, fReach;
			if(apElement->Attribute("Intensity"))
			{
				fIntensity = GetAttributeFloat(apElement, "Intensity", 1);
				fReach     = GetAttributeFloat(apElement, "Radius", deriveReach(fIntensity));
			}
			else
			{
				// Old map: "Radius" is the value the PBR renderer treats as
				// intensity; reach is derived so existing maps look unchanged.
				fIntensity = GetAttributeFloat(apElement, "Radius", 1);
				fReach     = deriveReach(fIntensity);
			}
			pLight->SetIntensity(fIntensity);
			pLight->SetRadius(fReach);
			pLight->SetSourceRadius(GetAttributeFloat(apElement, "SourceRadius", 0.f));
		}

		pLight->SetShadowMapResolution( ToShadowMapResolution(GetAttributeString(apElement, "ShadowResolution", "High")) );

		bool bShadowsAffectDynamic = GetAttributeBool(apElement, "ShadowsAffectDynamic", true);
		bool bShadowsAffectStatic = GetAttributeBool(apElement, "ShadowsAffectStatic", true);
		tObjectVariabilityFlag lFlags =0;
		if(bShadowsAffectDynamic)	lFlags |= eObjectVariabilityFlag_Dynamic;
		if(bShadowsAffectStatic)	lFlags |= eObjectVariabilityFlag_Static;
		pLight->SetShadowCastersAffected(lFlags);

		//////////////////////
		// Backwards compitabilty:
		float fDefaultFadeOn = GetAttributeFloat(apElement, "FlickerOnFadeLength",0);
		float fDefaultFadeOff = GetAttributeFloat(apElement, "FlickerOffFadeLength",0);

		pLight->SetFlickerActive(GetAttributeBool(apElement, "FlickerActive", false));
		pLight->SetFlicker(
			GetAttributeColor(apElement, "FlickerOffColor"),
			GetAttributeFloat(apElement, "FlickerOffRadius"),

			GetAttributeFloat(apElement, "FlickerOnMinLength"),
			GetAttributeFloat(apElement, "FlickerOnMaxLength"),
			GetAttributeString(apElement, "FlickerOnSound"),
			GetAttributeString(apElement, "FlickerOnPS"),

			GetAttributeFloat(apElement, "FlickerOffMaxLength"),
			GetAttributeFloat(apElement, "FlickerOffMinLength"),
			GetAttributeString(apElement, "FlickerOffSound"),
			GetAttributeString(apElement, "FlickerOffPS"),

			GetAttributeBool(apElement, "FlickerFade"),
			GetAttributeFloat(apElement, "FlickerOnFadeMinLength", fDefaultFadeOn),
			GetAttributeFloat(apElement, "FlickerOnFadeMaxLength", fDefaultFadeOn),

			GetAttributeFloat(apElement, "FlickerOffFadeMinLength", fDefaultFadeOff),
			GetAttributeFloat(apElement, "FlickerOffFadeMaxLength", fDefaultFadeOff)
			);
 

		kEndWorldEntityLoad(pLight);
	}
	

	//-----------------------------------------------------------------------

	int glDecalNumOfElements[4] = {4,3,3,4};
	eVertexBufferElement glDecalElementType[4] = {	eVertexBufferElement_Position, 
													eVertexBufferElement_Normal,
													eVertexBufferElement_Texture0,
													eVertexBufferElement_Texture1Tangent};
	cMesh* cEngineFileLoading::LoadDecalMeshHelper(tinyxml2::XMLElement* apElement, cGraphics* apGraphics, cResources* apResources, const tString& asName, const tString& asMaterial, const cColor& aColor)
	{
		////////////////////////////////
		//Load Vertex data
		if(apElement==NULL)return NULL;

		int lNumOfVtx = GetAttributeInt(apElement, "NumVerts", 0);
		int lNumOfIdx = GetAttributeInt(apElement, "NumInds", 0);

		if(lNumOfIdx <=0 || lNumOfVtx<=0)
		{
			Warning("Decal %s is missing geometry, skipping!\n", asName.c_str());
			return NULL;
		}

		tinyxml2::XMLElement *pDataArrayElem[4];
		pDataArrayElem[0] = apElement->FirstChildElement("Positions");
		pDataArrayElem[1] = apElement->FirstChildElement("Normals");
		pDataArrayElem[2] = apElement->FirstChildElement("TexCoords");
		pDataArrayElem[3] = apElement->FirstChildElement("Tangents");
		tinyxml2::XMLElement *pIndicesElem = apElement->FirstChildElement("Indices");

		tFloatVec vDataArrays[4];
		tIntVec vIdxArray;
		tString sSepp=" ";
		for(int i=0; i<4; ++i)
		{
			vDataArrays->reserve(lNumOfVtx * glDecalNumOfElements[i]);
			cString::GetFloatVec(GetAttributeString(pDataArrayElem[i], "Array"), vDataArrays[i],&sSepp);
		}
		vIdxArray.reserve(lNumOfIdx);
		cString::GetIntVec(GetAttributeString(pIndicesElem, "Array"), vIdxArray,&sSepp);
	
		//////////////////////////////////
		// Create vertex buffer
		cVertexBuffer *pVtxBuffer = apGraphics->GetLowLevel()->CreateVertexBuffer(eVertexBufferType_Software, eVertexBufferDrawType_Tri, 
																					eVertexBufferUsageType_Static,lNumOfVtx, lNumOfIdx);

		//Create arrays	
		for(int i=0; i<4; ++i)
			pVtxBuffer->CreateElementArray(glDecalElementType[i],eVertexBufferElementFormat_Float, glDecalNumOfElements[i]);
		pVtxBuffer->CreateElementArray(eVertexBufferElement_Color0,eVertexBufferElementFormat_Float,4);
		
		//Copy the data!
		// TODO: This needs to be made faster so that data is loaded directly into mesh!
		for(int vtx=0; vtx<lNumOfVtx; ++vtx)
		{
			for(int i=0; i<4; ++i)
			{
				float *pData = &vDataArrays[i][vtx*glDecalNumOfElements[i]];

				if(glDecalNumOfElements[i]==2)
					pVtxBuffer->AddVertexVec3f(glDecalElementType[i], cVector3f(pData[0],pData[1],0) );
				else if(glDecalNumOfElements[i]==3)
					pVtxBuffer->AddVertexVec3f(glDecalElementType[i], cVector3f(pData[0],pData[1],pData[2]) );
				else if(glDecalNumOfElements[i]==4)
					pVtxBuffer->AddVertexVec4f(glDecalElementType[i], cVector3f(pData[0],pData[1],pData[2]),pData[3]);
			}

			pVtxBuffer->AddVertexColor(eVertexBufferElement_Color0, aColor);
		}

		for(int i=0; i<lNumOfIdx; ++i)
			pVtxBuffer->AddIndex(vIdxArray[i]);

		//Compile
		pVtxBuffer->Compile(0);
		
		/////////////////////////
		// Create the mesh
		cMesh *pMesh = hplNew( cMesh, (asName, _W(""), apResources->GetMaterialManager(), apResources->GetAnimationManager()) );
		pMesh->AddReference(); // hand-built mesh: take the one owning reference the entity drops

		cSubMesh *pSubMesh = pMesh->CreateSubMesh("Main");
		
		pSubMesh->SetMaterial(apResources->GetMaterialManager()->CreateMaterial(asMaterial));
		pSubMesh->SetVertexBuffer(pVtxBuffer);
		pSubMesh->SetMaterialName(asMaterial);


		return pMesh;
	}

	//////////////////////////////////////////////////////////////////////////
	// PRIVATE METHODS
	//////////////////////////////////////////////////////////////////////////

	//-----------------------------------------------------------------------

	void cEngineFileLoading::SetupWorldEntity(iEntity3D *apEntity, tinyxml2::XMLElement* apElement)
	{
		if(apEntity==NULL) return;

		int lID = GetAttributeInt(apElement, "ID");
		cVector3f vPosition = GetAttributeVector3f(apElement, "WorldPos",0);
		cVector3f vScale = GetAttributeVector3f(apElement, "Scale",1);
		cVector3f vRotation = GetAttributeVector3f(apElement, "Rotation",0);

		cMatrixf mtxTransform = cMath::MatrixMul(cMath::MatrixRotate(vRotation, eEulerRotationOrder_XYZ),cMath::MatrixScale(vScale));
		mtxTransform.SetTranslation(vPosition);

		apEntity->SetMatrix(mtxTransform);
		apEntity->SetUniqueID(lID);
	}

    //-----------------------------------------------------------------------
}
