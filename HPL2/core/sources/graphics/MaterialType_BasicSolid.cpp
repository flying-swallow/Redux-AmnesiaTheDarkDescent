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

#include "graphics/MaterialType_BasicSolid.h"

#include "system/LowLevelSystem.h"
#include "system/PreprocessParser.h"

#include "resources/Resources.h"
#include "resources/TextureManager.h"

#include "math/Frustum.h"
#include "math/Math.h"

#include "graphics/Graphics.h"
#include "graphics/LowLevelGraphics.h"
#include "graphics/Renderer.h"
#include "graphics/Material.h"
#include "graphics/Renderable.h"

namespace hpl {
	
	//////////////////////////////////////////////////////////////////////////
	// STATIC OBJECTS
	//////////////////////////////////////////////////////////////////////////

	bool iMaterialType_SolidBase::mbGlobalDataCreated = false;
	float iMaterialType_SolidBase::mfVirtualPositionAddScale = 0.03f;

	//////////////////////////////////////////////////////////////////////////
	// SOLID BASE
	//////////////////////////////////////////////////////////////////////////

	iMaterialType_SolidBase::iMaterialType_SolidBase(cGraphics *apGraphics, cResources *apResources) : iMaterialType(apGraphics,apResources)
	{
		mbIsGlobalDataCreator = false;
	}

	iMaterialType_SolidBase::~iMaterialType_SolidBase() {}

	void iMaterialType_SolidBase::LoadData()
	{
		//////////////
		// Create textures
		mpDissolveTexture = mpResources->GetTextureManager()->Create2DImage("core_dissolve.tga",true);
	}

	void iMaterialType_SolidBase::CompileMaterialSpecifics(cMaterial *apMaterial)
	{
		////////////////////////
		// If there is an alpha texture, set alpha mode to trans, else solid.
		if(apMaterial->GetImage(eMaterialTexture_Alpha))
		{
			apMaterial->SetAlphaMode(eMaterialAlphaMode_Trans);
		}
		else
		{
			apMaterial->SetAlphaMode(eMaterialAlphaMode_Solid);
		}

		CompileSolidSpecifics(apMaterial);
	}

	//////////////////////////////////////////////////////////////////////////
	// SOLID DIFFUSE
	//////////////////////////////////////////////////////////////////////////
	
	cMaterialType_SolidDiffuse::cMaterialType_SolidDiffuse(cGraphics *apGraphics, cResources *apResources) : iMaterialType_SolidBase(apGraphics, apResources)
	{
		AddUsedTexture(eMaterialTexture_Diffuse);
		AddUsedTexture(eMaterialTexture_NMap);
		AddUsedTexture(eMaterialTexture_Alpha);
		AddUsedTexture(eMaterialTexture_Specular);
		AddUsedTexture(eMaterialTexture_Height);
		AddUsedTexture(eMaterialTexture_Illumination);
		AddUsedTexture(eMaterialTexture_DissolveAlpha);
		AddUsedTexture(eMaterialTexture_CubeMap);
		AddUsedTexture(eMaterialTexture_CubeMapAlpha);

		mbHasTypeSpecifics[eMaterialRenderMode_Diffuse] = true;

		AddVarFloat("HeightMapScale", 0.05f, "");
		AddVarFloat("HeightMapBias", 0, "");
		AddVarFloat("FrenselBias", 0.2f, "Bias for Fresnel term. values: 0-1. Higher means that more of reflection is seen when looking straight at object.");
		AddVarFloat("FrenselPow", 8.0f, "The higher the 'sharper' the reflection is, meaning that it is only clearly seen at sharp angles.");
		AddVarBool("AlphaDissolveFilter", false, "If alpha values between 0 and 1 should be used and dissolve the texture. This can be useful for things like hair.");
	}

	cMaterialType_SolidDiffuse::~cMaterialType_SolidDiffuse() {}

	void cMaterialType_SolidDiffuse::CompileSolidSpecifics(cMaterial *apMaterial)
	{
		cMaterialType_SolidDiffuse_Vars *pVars = (cMaterialType_SolidDiffuse_Vars*)apMaterial->GetVars();

		//////////////////////////////////
		// Z specifics
		apMaterial->SetHasObjectSpecificsSettings(eMaterialRenderMode_Z_Dissolve,true);
		apMaterial->SetUseAlphaDissolveFilter(pVars->mbAlphaDissolveFilter);
		
		//////////////////////////////////
		// Normal map and height specifics
		if(apMaterial->GetTexture(eMaterialTexture_NMap))
		{
			if(apMaterial->GetTexture(eMaterialTexture_Height))
			{
				apMaterial->SetHasSpecificSettings(eMaterialRenderMode_Diffuse,true);
			}
		}

		//////////////////////////////////
		// UV animation specifics
		if(apMaterial->HasUvAnimation())
		{
			apMaterial->SetHasSpecificSettings(eMaterialRenderMode_Z,true);
			apMaterial->SetHasSpecificSettings(eMaterialRenderMode_Diffuse,true);
			apMaterial->SetHasSpecificSettings(eMaterialRenderMode_Illumination,true);
		}

		//////////////////////////////////
		// Cubemap
		if(apMaterial->GetTexture(eMaterialTexture_CubeMap))
		{
			apMaterial->SetHasSpecificSettings(eMaterialRenderMode_Diffuse,true);
		}

		//////////////////////////////////
		// Illuminations specifics
		if(apMaterial->GetTexture(eMaterialTexture_Illumination))
		{
			apMaterial->SetHasObjectSpecificsSettings(eMaterialRenderMode_Illumination,true);
		}

		//////////////////////////////////
		// Build the bindless descriptor for the per-frame SSBO upload.
		ShaderMaterialData desc{};
		desc.m_id = MaterialID::SolidDiffuse;
		desc.m_solid.m_heightMapScale = pVars->mfHeightMapScale;
		desc.m_solid.m_heightMapBias = pVars->mfHeightMapBias;
		desc.m_solid.m_frenselBias = pVars->mfFrenselBias;
		desc.m_solid.m_frenselPow = pVars->mfFrenselPow;
		desc.m_solid.m_alphaDissolveFilter = pVars->mbAlphaDissolveFilter;
		apMaterial->SetDescriptor(desc);
	}

	iTexture* cMaterialType_SolidDiffuse::GetTextureForUnit(cMaterial *apMaterial,eMaterialRenderMode aRenderMode, int alUnit)
	{
		cMaterialType_SolidDiffuse_Vars *pVars = (cMaterialType_SolidDiffuse_Vars*)apMaterial->GetVars();

		////////////////////////////
		// Z
		if(aRenderMode == eMaterialRenderMode_Z)
		{
			switch(alUnit)
			{
			case 0: return apMaterial->GetTexture(eMaterialTexture_Alpha);
			// case 1: return mpDissolveTexture;
			}
		}

		////////////////////////////
		// Z Dissolve
		else if(aRenderMode == eMaterialRenderMode_Z_Dissolve)
		{
			switch(alUnit)
			{
			case 0: return apMaterial->GetTexture(eMaterialTexture_Alpha);
			//case 1: return mpDissolveTexture;
			case 2: return apMaterial->GetTexture(eMaterialTexture_DissolveAlpha);
			}
		}

		////////////////////////////
		// Diffuse
		else if(aRenderMode == eMaterialRenderMode_Diffuse)
		{
			switch(alUnit)
			{
			case 0: return apMaterial->GetTexture(eMaterialTexture_Diffuse);
			case 1: return apMaterial->GetTexture(eMaterialTexture_NMap);
			case 2: return apMaterial->GetTexture(eMaterialTexture_Specular);
			case 3: return apMaterial->GetTexture(eMaterialTexture_Height);
			case 4: return apMaterial->GetTexture(eMaterialTexture_CubeMap);
			case 5: return apMaterial->GetTexture(eMaterialTexture_CubeMapAlpha);
			}
		}

		////////////////////////////
		// Illumination
		else if(aRenderMode == eMaterialRenderMode_Illumination)
		{
			switch(alUnit)
			{
			case 0: return apMaterial->GetTexture(eMaterialTexture_Illumination);
			}
		}

		return NULL;
	}

	iMaterialVars* cMaterialType_SolidDiffuse::CreateSpecificVariables()
	{
		return hplNew(cMaterialType_SolidDiffuse_Vars,());
	}

	void cMaterialType_SolidDiffuse::LoadVariables(cMaterial* apMaterial, cResourceVarsObject *apVars)
	{
		cMaterialType_SolidDiffuse_Vars *pVars = (cMaterialType_SolidDiffuse_Vars*)apMaterial->GetVars();
		if(pVars==NULL)
		{
			pVars = (cMaterialType_SolidDiffuse_Vars*)CreateSpecificVariables();
			apMaterial->SetVars(pVars);
		}
				
		pVars->mfHeightMapScale = apVars->GetVarFloat("HeightMapScale", 0.1f);
		pVars->mfHeightMapBias = apVars->GetVarFloat("HeightMapBias", 0);
		pVars->mfFrenselBias = apVars->GetVarFloat("FrenselBias", 0.2f);
		pVars->mfFrenselPow = apVars->GetVarFloat("FrenselPow", 8.0f);
		pVars->mbAlphaDissolveFilter = apVars->GetVarBool("AlphaDissolveFilter", false);
	}

	void cMaterialType_SolidDiffuse::GetVariableValues(cMaterial* apMaterial, cResourceVarsObject* apVars)
	{
		cMaterialType_SolidDiffuse_Vars* pVars = (cMaterialType_SolidDiffuse_Vars*)apMaterial->GetVars();

		apVars->AddVarFloat("HeightMapScale", pVars->mfHeightMapScale);
		apVars->AddVarFloat("HeightMapBias", pVars->mfHeightMapBias);
		apVars->AddVarFloat("FrenselBias", pVars->mfFrenselBias);
		apVars->AddVarFloat("FrenselPow", pVars->mfFrenselPow);
		apVars->AddVarBool("AlphaDissolveFilter", pVars->mbAlphaDissolveFilter);
	}
}
