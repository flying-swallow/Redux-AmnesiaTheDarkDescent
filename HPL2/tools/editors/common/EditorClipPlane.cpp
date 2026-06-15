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

#include "EditorClipPlane.h"
#include "EntityWrapper.h"
#include "EditorWorld.h"
#include "EditorHelper.h"

#include <tinyxml2.h>
#include "resources/XmlHelper.h"

//------------------------------------------------------------------

cEditorClipPlane::cEditorClipPlane(iEditorWorld* apWorld)
{
	mpWorld = apWorld;
	mbCullingOnPositiveSide = true;
	mbActive = true;
}

//------------------------------------------------------------------
//------------------------------------------------------------------

bool cEditorClipPlane::PointIsOnCullingSide(const cVector3f& avPos)
{
	float fHeight = GetHeight();
	if(mbCullingOnPositiveSide)
	{
		return avPos.v[mNormal] >= fHeight;
	}
	else
		return avPos.v[mNormal] <= fHeight;
}

//------------------------------------------------------------------

void cEditorClipPlane::SetCullingOnPositiveSide(bool abX)
{
	if(mbCullingOnPositiveSide==abX)
		return;

	mbCullingOnPositiveSide = abX;

	OnPlaneModified();
}

//------------------------------------------------------------------

void cEditorClipPlane::Draw(DebugDraw* apFunctions, const cVector3f& avPos)
{
	if(IsActive()==false) return;

	cVector3f vPlaneNormal = GetPlane().GetNormal();
	cVector3f vCenter = vPlaneNormal*GetHeight();
	float fSign[] = { -1, 1 };
	//vCenter + vPlaneNormal*3*fSign[mbCullingOnPositiveSide]
	apFunctions->DebugDrawBoxMinMax(GetProjectedPosOnPlane(vCenter-20), GetProjectedPosOnPlane(vCenter+20), cColor(1,1));
	//cEditorHelper::DrawPyramid(apFunctions, vCenter, vCenter + vPlaneNormal*fSign[mbCullingOnPositiveSide]*5, 1, cColor(1,0,0,1));
	apFunctions->DebugDrawLine(vCenter, vCenter + vPlaneNormal*fSign[mbCullingOnPositiveSide]*5, cColor(1,1));
}

//------------------------------------------------------------------

void cEditorClipPlane::SetActive(bool abX)
{
	if(mbActive==abX)
		return;

	mbActive = abX;

	OnPlaneModified();
}

//------------------------------------------------------------------

void cEditorClipPlane::Load(tinyxml2::XMLElement* apElement)
{
	SetActive(GetAttributeBool(apElement, "Active"));
	SetCullingOnPositiveSide(GetAttributeBool(apElement, "CullPos"));
	SetHeights(GetAttributeVector3f(apElement, "Heights"));
	SetPlaneNormal(GetPlaneNormalFromString(GetAttributeString(apElement, "Plane")));
}

//------------------------------------------------------------------

void cEditorClipPlane::Save(tinyxml2::XMLElement* apElement)
{
	apElement->SetValue("ClipPlane");
	SetAttributeBool(apElement, "Active", IsActive());
	SetAttributeBool(apElement, "CullPos", GetCullingOnPositiveSide());
	SetAttributeVector3f(apElement, "Heights", GetHeights());
	SetAttributeString(apElement, "Plane", cString::To8Char(GetPlaneString()));
}

//------------------------------------------------------------------
//------------------------------------------------------------------

void cEditorClipPlane::OnPlaneModified()
{
	mpWorld->SetClipPlanesUpdated();
}

//------------------------------------------------------------------


