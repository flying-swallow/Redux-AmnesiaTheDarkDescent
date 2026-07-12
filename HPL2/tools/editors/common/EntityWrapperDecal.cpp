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

#include "EntityWrapperDecal.h"

#include "EditorWorld.h"
#include "EditorWindowViewport.h"
#include "EditorWindowEntityEditBoxDecal.h"
#include "EditorHelper.h"
#include "EditorEditModeDecals.h"

#include "EntityIcon.h"

#include "EngineEntity.h"

#include "scene/Decal.h" // eDecalReceiver bits for the receiver mask

#include <tinyxml2.h>
#include "resources/XmlHelper.h"

#include <algorithm>

//-----------------------------------------------------------------------------------------

////////////////////////////////
// Static global decal variables
//  Min and max for decal offset. Actual offset value should pick a random value between these two.
float cEntityWrapperTypeDecal::mfDecalOffsetMin = 0.005f;
float cEntityWrapperTypeDecal::mfDecalOffsetMax = 0.01f;

//	Global max triangles per decal.
int cEntityWrapperTypeDecal::mlGlobalMaxTriangles = 300;
bool cEntityWrapperTypeDecal::mbForcingUpdate = false;

//------------------------------------------------------------------------------------------

cEntityWrapperTypeDecal::cEntityWrapperTypeDecal() : iEntityWrapperType(eEditorEntityType_Decal, _W("Decal"), "Decal")
{
	AddInt(eDecalInt_FileIndex, "MaterialIndex", ePropCopyStep_PreEnt);
	AddString(eDecalStr_Material, "Material", "", ePropCopyStep_PreEnt, false);
	AddInt(eDecalInt_CurrentSubDiv, "CurrentSubDiv", 0, ePropCopyStep_PreEnt);
	AddInt(eDecalInt_MaxTris, "MaxTriangles", -1, ePropCopyStep_PreEnt);

	AddFloat(eDecalFloat_Offset, "Offset", 0, ePropCopyStep_PreEnt);
	AddVec2f(eDecalVec2f_SubDivs, "SubDiv", 1, ePropCopyStep_PreEnt);
	AddColor(eDecalCol_Color, "Color", cColor(1), ePropCopyStep_PreEnt);

	AddBool(eDecalBool_AffectStatic, "OnStatic", true, ePropCopyStep_PreEnt);
	AddBool(eDecalBool_AffectPrimitive, "OnPrimitive", true, ePropCopyStep_PreEnt);
	AddBool(eDecalBool_AffectEntity, "OnEntity", true, ePropCopyStep_PreEnt);
}

//-----------------------------------------------------------------------------------------

iEntityWrapperData* cEntityWrapperTypeDecal::CreateSpecificData()
{
	return hplNew(cEntityWrapperDataDecal,(this));
}

cEntityWrapperDataDecal::cEntityWrapperDataDecal(iEntityWrapperType* apType) : iEntityWrapperData(apType)
{
	mpDecal = NULL;
}

//-----------------------------------------------------------------------------------------

void cEntityWrapperDataDecal::CopyFromEntity(iEntityWrapper* apEntity)
{
	iEntityWrapperData::CopyFromEntity(apEntity);
	mpDecal = apEntity;
}

//-----------------------------------------------------------------------------------------

void cEntityWrapperDataDecal::CopyToEntity(iEntityWrapper* apEntity, int alCopyFlags)
{
	iEntityWrapperData::CopyToEntity(apEntity, alCopyFlags);
	//cEntityWrapperDecal* pDecal = (cEntityWrapperDecal*)apEntity;
	//pDecal->SetAffectedEntityIDs(mvEntityIDs);
}

//-----------------------------------------------------------------------------------------

bool cEntityWrapperDataDecal::Load(tinyxml2::XMLElement* apElement)
{
	if(iEntityWrapperData::Load(apElement)==false)
		return false;

	iEditorWorld* pWorld = mpType->GetWorld();

	////////////////////////////////////////////
	// Load File index
	int lFileIndex = GetInt(eDecalInt_FileIndex);
	if(lFileIndex==-1)
	{
		tString sMat = cString::To8Char(pWorld->GetEditor()->GetEngine()->GetResources()->GetFileSearcher()->GetFilePath(GetString(eDecalStr_Material)));
		lFileIndex = pWorld->AddFilenameToIndex("Decals", sMat);
		SetInt(eDecalInt_FileIndex, lFileIndex);
	}

	////////////////////////////////////////////
	// Link to file by index
	tString sMaterial = pWorld->GetFilenameFromIndex("Decals", lFileIndex);
	if(sMaterial=="" && lFileIndex==-1)
	{
		pWorld->SetLoadErrorMessage(_W("File index out of bounds!"));
		return false;
	}

	SetString(eDecalStr_Material, sMaterial);

	////////////////////////////////////////////
	// Fix decal offset inside boundaries
	float fOffset = GetFloat(eDecalFloat_Offset);
	float fOffsetMin = cEntityWrapperTypeDecal::GetDecalOffsetMin();
	float fOffsetMax = cEntityWrapperTypeDecal::GetDecalOffsetMax();
	if(fOffset < fOffsetMin || 
		fOffset > fOffsetMax)
	{
		fOffset = cMath::RandRectf(fOffsetMin, fOffsetMax);

		SetFloat(eDecalFloat_Offset, fOffset);
	}

	// NOTE: the legacy baked <DecalMesh> geometry (if present in old maps) is
	// ignored — decals now render via GPU projection from the cDecal oriented
	// box (cWorld::mpDecalBuffer), driven by the properties loaded above.

	/////////////////////////////////////////////////
	// Load affected entity IDs
	//tString sAffectedEntityIDs = apElement->GetAttributeString("AffectedEnts", "");
	//mvEntityIDs.clear();
	//cString::GetIntVec(sAffectedEntityIDs, mvEntityIDs);

	return true;
}

//-----------------------------------------------------------------------------------------

bool cEntityWrapperDataDecal::SaveSpecific(tinyxml2::XMLElement* apElement)
{
	if(iEntityWrapperData::SaveSpecific(apElement)==false || mpDecal==NULL)
		return false;

	// Decals now render via GPU projection from the cDecal oriented box, so the
	// baked <DecalMesh> geometry is no longer emitted; the loader reconstructs
	// the decal entirely from the saved properties (material, color, offset,
	// sub-div, On* flags). Old maps that still carry <DecalMesh> load fine — it
	// is simply ignored.
	return true;
}

//-----------------------------------------------------------------------------------------

iEntityWrapper* cEntityWrapperDataDecal::CreateSpecificEntity()
{
	/////////////////////////////////////////////////////////////////////////////////////////
	// Check so the File Index is still valid ! (can become invalid after undoing a delete
	iEditorWorld* pWorld = mpType->GetWorld();

	int lFileIndex = GetInt(eDecalInt_FileIndex);
	const tString& sCurrentFile = GetString(eDecalStr_Material);
	tWString sPath = pWorld->GetEditor()->GetEngine()->GetResources()->GetFileSearcher()->GetFilePath(sCurrentFile);
	int lFileIndexForCurrentFile = pWorld->AddFilenameToIndex("Decals", cString::To8Char(sPath));

	// the If first index and the one we just retrieved are different, update first
	if(lFileIndex!=lFileIndexForCurrentFile)
		SetInt(eDecalInt_FileIndex, lFileIndexForCurrentFile);

	return hplNew(cEntityWrapperDecal,(this));
}

//-----------------------------------------------------------------------------------------
//-----------------------------------------------------------------------------------------
//-----------------------------------------------------------------------------------------

cEntityWrapperDecal::cEntityWrapperDecal(iEntityWrapperData* apData) : iEntityWrapper(apData)
{
	mbDecalUpdated=true;
	mbGeometryUpdated=true;
	mbMaterialUpdated=true;
	mColor = 1;
	mvSubDivisions = 1;
	
	mbAffectsStaticObjects = true;
	mbAffectsEntities = true;
	mbAffectsPrimitives = true;

	mbDeployed = false;
}

//-----------------------------------------------------------------------------------------

cEntityWrapperDecal::~cEntityWrapperDecal()
{
}

//-----------------------------------------------------------------------------------------

bool cEntityWrapperDecal::SetProperty(int alPropID, const int& alX)
{
	switch(alPropID)
	{
	case eDecalInt_FileIndex:
		SetFileIndex(alX);
		break;
	case eDecalInt_CurrentSubDiv:
		SetCurrentSubDiv(alX);
		break;
	case eDecalInt_MaxTris:
		SetMaxTriangles(alX);
		break;
	default:
		return iEntityWrapper::SetProperty(alPropID, alX);
	}

	return true;
}

//-----------------------------------------------------------------------------------------

bool cEntityWrapperDecal::SetProperty(int alPropID, const float& afX)
{
	switch(alPropID)
	{
	case eDecalFloat_Offset:
		SetOffset(afX);
		break;
	default:
		return iEntityWrapper::SetProperty(alPropID, afX);
	}

	return true;
}

//-----------------------------------------------------------------------------------------

bool cEntityWrapperDecal::SetProperty(int alPropID, const bool& abX)
{
	switch(alPropID)
	{
	case eDecalBool_AffectStatic:
		SetAffectStatic(abX);
		break;
	case eDecalBool_AffectPrimitive:
		SetAffectPrimitive(abX);
		break;
	case eDecalBool_AffectEntity:
		SetAffectEntity(abX);
		break;
	default:
		return iEntityWrapper::SetProperty(alPropID, abX);
	}

	return true;
}

//-----------------------------------------------------------------------------------------

bool cEntityWrapperDecal::SetProperty(int alPropID, const tString& asX)
{
	switch(alPropID)
	{
	case eDecalStr_Material:
		SetMaterial(asX);
		break;
	default:
		return iEntityWrapper::SetProperty(alPropID, asX);
	}
	
	return true;
}

//-----------------------------------------------------------------------------------------

bool cEntityWrapperDecal::SetProperty(int alPropID, const cVector2f& avX)
{
	switch(alPropID)
	{
	case eDecalVec2f_SubDivs:
		{
			cVector2l vSubDivs = cVector2l((int)avX.x, (int)avX.y);	
			SetUVSubDivisions(vSubDivs);
		}
		break;
	default:
		return iEntityWrapper::SetProperty(alPropID, avX);
	}

	return true;
}

//-----------------------------------------------------------------------------------------

bool cEntityWrapperDecal::SetProperty(int alPropID, const cColor& aX)
{
	switch(alPropID)
	{
	case eDecalCol_Color:
		SetColor(aX);
		break;
	default:
		return iEntityWrapper::SetProperty(alPropID, aX);
	}

	return true;
}

//-----------------------------------------------------------------------------------------

bool cEntityWrapperDecal::GetProperty(int alPropID, int& alX)
{
	switch(alPropID)
	{
	case eDecalInt_FileIndex:
		alX = GetFileIndex();
		break;
	case eDecalInt_CurrentSubDiv:
		alX = GetCurrentSubDiv();
		break;
	case eDecalInt_MaxTris:
		alX = GetMaxTriangles();
		break;
	default:
		return iEntityWrapper::GetProperty(alPropID, alX);
	}

	return true;
}

//-----------------------------------------------------------------------------------------

bool cEntityWrapperDecal::GetProperty(int alPropID, float& afX)
{
	switch(alPropID)
	{
	case eDecalFloat_Offset:
		afX = GetOffset();
		break;
	default:
		return iEntityWrapper::GetProperty(alPropID, afX);
	}

	return true;
}

//-----------------------------------------------------------------------------------------

bool cEntityWrapperDecal::GetProperty(int alPropID, bool& abX)
{
	switch(alPropID)
	{
	case eDecalBool_AffectStatic:
		abX = mbAffectsStaticObjects;
		break;
	case eDecalBool_AffectPrimitive:
		abX = mbAffectsPrimitives;
		break;
	case eDecalBool_AffectEntity:
		abX = mbAffectsEntities;
		break;
	default:
		return iEntityWrapper::GetProperty(alPropID, abX);
	}

	return true;
}

//-----------------------------------------------------------------------------------------

bool cEntityWrapperDecal::GetProperty(int alPropID, tString& asX)
{
	switch(alPropID)
	{
	case eDecalStr_Material:
		asX = GetMaterial();
		break;
	default:
		return iEntityWrapper::GetProperty(alPropID, asX);
	}
	
	return true;
}

//-----------------------------------------------------------------------------------------

bool cEntityWrapperDecal::GetProperty(int alPropID, cVector2f& avX)
{
	switch(alPropID)
	{
	case eDecalVec2f_SubDivs:
		{
			const cVector2l& vSubDivs = GetUVSubDivisions();
			avX = cVector2f((float)vSubDivs.x, (float)vSubDivs.y);
		}
		break;
	default:
		return iEntityWrapper::GetProperty(alPropID, avX);
	}

	return true;
}

//-----------------------------------------------------------------------------------------

bool cEntityWrapperDecal::GetProperty(int alPropID, cColor& aX)
{
	switch(alPropID)
	{
	case eDecalCol_Color:
		aX = GetColor();
		break;
	default:
		return iEntityWrapper::SetProperty(alPropID, aX);
	}

	return true;
}

//-----------------------------------------------------------------------------------------

void cEntityWrapperDecal::OnPostDeployAll(bool abX)
{
	mbDeployed = true;

	UpdateEntity();

	iEntityWrapper::OnPostDeployAll(abX);
}


//-----------------------------------------------------------------------------------------

cEditorWindowEntityEditBox* cEntityWrapperDecal::CreateEditBox(cEditorEditModeSelect* apEditMode)
{
	return hplNew(cEditorWindowEntityEditBoxDecal,(apEditMode, this));
}

//-----------------------------------------------------------------------------------------

void cEntityWrapperDecal::SetAffectStatic(bool abX)
{
	if(mbAffectsStaticObjects==abX)
		return;
	mbAffectsStaticObjects = abX;

	mbDecalUpdated = true;
	mbGeometryUpdated = true;
}

void cEntityWrapperDecal::SetAffectPrimitive(bool abX)
{
	if(mbAffectsPrimitives==abX)
		return;
	mbAffectsPrimitives = abX;

	mbDecalUpdated = true;
	mbGeometryUpdated = true;
}

void cEntityWrapperDecal::SetAffectEntity(bool abX)
{
	if(mbAffectsEntities==abX)
		return;
	mbAffectsEntities = abX;

	mbDecalUpdated = true;
	mbGeometryUpdated = true;
}

//-----------------------------------------------------------------------------------------

void cEntityWrapperDecal::SetMaxTriangles(int alX)
{
	if(alX!=-1 && mlMaxTriangles==alX)
		return;
	mlMaxTriangles = alX;

	mbDecalUpdated = true;
	mbGeometryUpdated = true;
}

//-----------------------------------------------------------------------------------------

void cEntityWrapperDecal::SetMaterial(const tString& asX)
{
	if(msMaterial==asX)
		return;
	msMaterial = asX;

	mbDecalUpdated = true;
	mbMaterialUpdated = true;
	mbEntityUpdated = true;
}

//-----------------------------------------------------------------------------------------

void cEntityWrapperDecal::SetOffset(float afX)
{
	if(mfOffset==afX)
		return;

	mfOffset = afX;

	mbDecalUpdated = true;
	mbGeometryUpdated = true;
}

//-----------------------------------------------------------------------------------------

void cEntityWrapperDecal::SetColor(const cColor& aX)
{
	if(mColor==aX)
		return;

	mColor = aX;

	// color is construction-time on cDecal → needs a recreate
	mbDecalUpdated = true;
	mbMaterialUpdated = true;
}

//-----------------------------------------------------------------------------------------

void cEntityWrapperDecal::SetUVSubDivisions(const cVector2l& avX)
{
	if(mvSubDivisions==avX)
		return;

	mvSubDivisions = avX;

	// sub-div grid is construction-time on cDecal → needs a recreate
	mbDecalUpdated = true;
	mbMaterialUpdated = true;
}

//-----------------------------------------------------------------------------------------

void cEntityWrapperDecal::SetCurrentSubDiv(int alX)
{
	if(mlCurrentSubDiv==alX)
		return;

	mlCurrentSubDiv = alX;

	mbDecalUpdated = true;
	mbGeometryUpdated = true;
}

//-----------------------------------------------------------------------------------------

void cEntityWrapperDecal::UpdateDecal()
{
	bool bForce = cEntityWrapperTypeDecal::IsForcingUpdate();

	/////////////////////////////////////////////////////////////////////////
	// (Re)create the cDecal when it doesn't exist yet or when a construction-
	// time property (material/color/sub-div) changed. cDecal carries no
	// geometry, so a recreate is cheap; the heavy part is CompileDecals() below.
	if(mpEngineEntity==NULL || bForce || mbMaterialUpdated)
	{
		if(CreateEngineEntity()==false)
			mpEngineEntity = NULL;	// e.g. no material yet → wrapper shows its icon
		mbMaterialUpdated = false;
		mbDecalUpdated = true;		// force the transform/prop push below
	}

	if(bForce==false && mbDecalUpdated==false)
		return;
	mbDecalUpdated = false;
	mbGeometryUpdated = false;

	/////////////////////////////////////////////////////////////////////////
	// Push the transform (incl. scale) + live props onto the cDecal BEFORE
	// rebuilding the per-object association so it reflects the new decal box,
	// then refresh the association so the GPU projection lands correctly.
	cWorld* pWorld = GetEditorWorld()->GetWorld();
	if(mpEngineEntity)
	{
		cEngineEntityDecal* pDecalEnt = (cEngineEntityDecal*)mpEngineEntity;
		pDecalEnt->SetMatrix(mmtxTransform);
		pDecalEnt->SetCurrentSubDiv(mlCurrentSubDiv);
		pDecalEnt->SetReceiverMask(GetReceiverMask());
	}
	// Covers add/move/edit; a failed create still needs this so the other decals'
	// association lists stay valid. Debounced — PrepareFrame rebuilds once per
	// frame (a drag fires this every step but only recompiles once).
	pWorld->MarkDecalAssociationsDirty();
}

//-----------------------------------------------------------------------------------------

void cEntityWrapperDecal::SetAbsPosition(const cVector3f& avPosition)
{
	iEntityWrapper::SetAbsPosition(avPosition);
	mbDecalUpdated = true;
	mbGeometryUpdated = true;
}

//-----------------------------------------------------------------------------------------

void cEntityWrapperDecal::SetAbsRotation(const cVector3f& avRotation)
{
	iEntityWrapper::SetAbsRotation(avRotation);
	mbDecalUpdated = true;
	mbGeometryUpdated = true;
}

//-----------------------------------------------------------------------------------------

void cEntityWrapperDecal::SetAbsScale(const cVector3f& avScale, int alAxis)
{
	iEntityWrapper::SetAbsScale(avScale);
	mbDecalUpdated = true;
	mbGeometryUpdated = true;
}

//-----------------------------------------------------------------------------------------

void cEntityWrapperDecal::UpdateEntity()
{
	if(mbDeployed==false) return;
	
	UpdateMatrix();
	UpdateDecal();

	mpIcon->SetVisible(mpEngineEntity==NULL);
}

//-----------------------------------------------------------------------------------------

cVertexBuffer* cEntityWrapperDecal::BuildDecalVertexBuffer(cWorld* apWorld, cDecalCreator* apCreator,
													  const cVector3f& avPos, const cVector3f& avSize, float afOffset,
													  const cVector3f& avRight, const cVector3f& avUp, const cVector3f& avFwd,
													  const tString& asMaterial, const cColor& aCol,
													  const cVector2l& avSubDivs,int alSubDiv, int alMaxTris,
													  bool abAffectStaticObject, bool abAffectPrimitive, bool abAffectEntity)
{
	/////////////////////////////////////////
	// Set up decal parameters
	apCreator->SetDecalPosition(avPos);
	apCreator->SetDecalSize(avSize);
	apCreator->SetDecalOffset(afOffset);
	apCreator->SetDecalRight(avRight,false);
	apCreator->SetDecalUp(avUp,false);
	apCreator->SetDecalForward(avFwd, false);

	// Debug
	//Log("[Decal] Pos:%s Size:%s Right:%s Up:%s Forward:%s\n", avPos.ToFileString().c_str(), avSize.ToFileString().c_str(),
	//														  avRight.ToFileString().c_str(), avUp.ToFileString().c_str(), avFwd.ToFileString().c_str());

	apCreator->SetMaterial(asMaterial);
	apCreator->SetColor(aCol);
	apCreator->SetUVSubDivisions(avSubDivs);
	apCreator->SetCurrentSubDiv(alSubDiv);
	apCreator->SetMaxTrianglesPerDecal( (alMaxTris>=0)? alMaxTris : 
														cEntityWrapperTypeDecal::GetGlobalMaxTriangles() );

	if(apCreator->IsUpdated()==false)
		return apCreator->GetVB();
	
	apCreator->ClearMeshes();

	/////////////////////////////
	// Get renderable sets
    cRenderableSet *pSets[2] ={
		apWorld->GetRenderableSet(eWorldContainerType_Dynamic),
		apWorld->GetRenderableSet(eWorldContainerType_Static),
	};

	/////////////////////////////
	// Scan the sets to get geometry
	for(int i=0; i<2; ++i)
	{
		pSets[i]->UpdateBeforeRendering();
		IterateRenderables(pSets[i], apCreator,
							abAffectStaticObject, abAffectPrimitive, abAffectEntity);
	}

	apCreator->CanCreateDecal();
	
	return apCreator->GetVB();
}

//-----------------------------------------------------------------------------------------
//-----------------------------------------------------------------------------------------
//-----------------------------------------------------------------------------------------

cEntityIcon* cEntityWrapperDecal::CreateIcon()
{
	return hplNew(cEntityIcon,(this, "Decal"));
}

//-----------------------------------------------------------------------------------------

int cEntityWrapperDecal::GetReceiverMask() const
{
	int lMask = 0;
	if(mbAffectsStaticObjects) lMask |= eDecalReceiver_Static;
	if(mbAffectsPrimitives)    lMask |= eDecalReceiver_Primitive;
	if(mbAffectsEntities)      lMask |= eDecalReceiver_Entity;
	return lMask;
}

//-----------------------------------------------------------------------------------------

iEngineEntity* cEntityWrapperDecal::CreateSpecificEngineEntity()
{
	return hplNew(cEngineEntityDecal,(this, msMaterial, mColor, mvSubDivisions, mlCurrentSubDiv, GetReceiverMask()));
}

//-----------------------------------------------------------------------------------------
//-----------------------------------------------------------------------------------------

void cEntityWrapperDecal::IterateRenderables(cRenderableSet *apSet, cDecalCreator* apCreator,
										bool abAffectStaticObject, bool abAffectPrimitive, bool abAffectEntity)
{
	cBoundingVolume* pDecalBV = apCreator->GetDecalBoundingVolume();

	for(iRenderable *pObject : apSet->GetObjects())
	{
		//////////////////////////////////////////
		// Check if mesh object
		if(pObject->GetRenderType() != eRenderableType_SubMesh) continue;

		cSubMeshEntity* pSubMesh = (cSubMeshEntity*)pObject;
		iEntityWrapper* pEnt = (iEntityWrapper*) pSubMesh->GetUserData();
		if(pEnt==NULL ||
			pEnt->IsAffectedByDecal(abAffectStaticObject, abAffectPrimitive, abAffectEntity)==false)
			continue;

		//////////////////////////////////////////
		// Prune by decal AABB before handing to the creator
		cBoundingVolume* pObjectBV = pObject->GetBoundingVolume();
		if(cMath::CheckAABBIntersection(pObjectBV->GetMin(), pObjectBV->GetMax(), pDecalBV->GetMin(), pDecalBV->GetMax())==false)
			continue;

		apCreator->AddSubMesh(pSubMesh);
	}
}

//-----------------------------------------------------------------------------------------

