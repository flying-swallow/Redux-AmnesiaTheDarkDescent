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

#include "scene/Light.h"

#include "system/LowLevelSystem.h"

#include <cassert>

#include "math/Math.h"
#include "math/Frustum.h"

#include "graphics/Image.h"

#include "resources/Resources.h"
#include "resources/TextureManager.h"

#include "scene/ParticleSystem.h"
#include "scene/World.h"
#include "scene/SoundEntity.h"
#include "scene/MeshEntity.h"
#include "scene/Camera.h"

#include "graphics/Material.h"
#include "graphics/MaterialType.h"
#include "graphics/SubMesh.h"
#include "graphics/LowLevelGraphics.h"
#include "graphics/Renderer.h"



namespace hpl {

	// Spot view-projection biasing (NDC -> texture space) for shadow/gobo sampling.
	static const cMatrixf g_mtxTextureUnitFix(	0.5f,0,   0,   0.5f,
												0,   0.5f,0,   0.5f,
												0,   0,   0.5f,0.5f,
												0,   0,   0,   1.0f
												);

	//////////////////////////////////////////////////////////////////////////
	// CONSTRUCTORS
	//////////////////////////////////////////////////////////////////////////

	//-----------------------------------------------------------------------

	iLight::iLight(tLightData aData, tString asName, cResources *apResources)
		: iRenderable(asName), mLightData(std::move(aData))
	{
		///////////////////////////////
		//Managers and highlevel init
		mpWorld = NULL;
		mpTextureManager = apResources->GetTextureManager();

		///////////////////////////////
		//Render properties
		mbApplyTransformToBV = false;

		mShadowMapResolution = eShadowMapResolution_High;

		///////////////////////////////
		//Fade and flicker init
		mDiffuseColor = 0;
		mDefaultDiffuseColor = 0;
		mbCastShadows = false;
		mlShadowCastersAffected = eObjectVariabilityFlag_All;
		mfIntensity =0;
		mfRadius = 0;
		mfSourceRadius = 0;
		mfFadeTime=0;
		mbFlickering = false;

		mfFlickerStateLength = 0;

		mfFadeTime =0;

		///////////////////////////////
		//Per-type state init. Parameter defaults live in the struct member
		//initializers; only spots need runtime setup (derived caches).
		if(std::holds_alternative<cLightSpotData>(mLightData))
		{
			mpFrustum = hplNew( cFrustum, () );

			mfIntensity = 100.0f;

			// Forward+ uses a clamp-to-edge static sampler at the spot-falloff binding,
			// so the per-texture wrap call from the legacy path is unnecessary.
			SetSpotFalloffMap(mpTextureManager->Create1DImage("core_falloff_linear", false));
		}

		UpdateBoundingVolume();
	}

	//-----------------------------------------------------------------------

	iLight::~iLight()
	{
		if(mpFrustum) hplDelete(mpFrustum);
		// m_goboImage / m_spotFalloffMap (ImageResourceWrapper) free themselves.
	}

	//-----------------------------------------------------------------------

	//////////////////////////////////////////////////////////////////////////
	// PUBLIC METHODS
	//////////////////////////////////////////////////////////////////////////
	
	//-----------------------------------------------------------------------

	bool iLight::IsVisible()
	{
		if(mDiffuseColor.r <=0 && mDiffuseColor.g <=0 && mDiffuseColor.b <=0 && mDiffuseColor.a <=0)
			return false;
		// Box lights stay visible regardless of intensity (they don't use it).
		if(!std::holds_alternative<cLightBoxData>(mLightData) && mfIntensity <= 0) return false;

		return mbIsVisible;
	}

	//-----------------------------------------------------------------------


	void iLight::SetDiffuseColor(cColor aColor)
	{
		bool bWasVisble = (mDiffuseColor.r >0 || mDiffuseColor.g >0 || mDiffuseColor.b >0 || mDiffuseColor.a >0);
		
		mDiffuseColor = aColor;

		bool bVisible = (mDiffuseColor.r >0 || mDiffuseColor.g >0 || mDiffuseColor.b >0 || mDiffuseColor.a >0);
		
		//Check if the light changed its visibility
		if(mbIsVisible && bVisible != bWasVisble && mpRenderCallback)
		{
			mpRenderCallback->OnVisibleChange(this);
		}
	}

	//-----------------------------------------------------------------------

	void iLight::UpdateLight(float afTimeStep)
	{	
		/////////////////////////////////////////////
		// Fade
		if(mfFadeTime>0)
		{
			//Log("Fading: %f / %f\n",afTimeStep,mfFadeTime);

			float fNewRadius = mfIntensity + mfIntensityAdd*afTimeStep;
			SetIntensity(fNewRadius);
			
			mDiffuseColor.r += mColAdd.r*afTimeStep;
			mDiffuseColor.g += mColAdd.g*afTimeStep;
			mDiffuseColor.b += mColAdd.b*afTimeStep;
			mDiffuseColor.a += mColAdd.a*afTimeStep;
			SetDiffuseColor(mDiffuseColor);

			mfFadeTime-=afTimeStep;

			//Set the dest values.
			if(mfFadeTime<=0)
			{
				mfFadeTime =0;
				SetDiffuseColor(mDestCol);
				mfIntensity = mfDestIntensity;
			}
		}

		/////////////////////////////////////////////
		// Flickering
		if(mbFlickering && mfFadeTime<=0)
		{	
			//////////////////////
			//On
			if(mbFlickerOn)
			{
				if(mfFlickerTime >= mfFlickerStateLength)
				{
					mbFlickerOn = false;
					if(!mbFlickerFade)
					{
						SetDiffuseColor(mFlickerOffColor);
						SetIntensity(mfFlickerOffIntensity);
					}
					else
					{
						FadeTo(mFlickerOffColor,mfFlickerOffIntensity, cMath::RandRectf(mfFlickerOffFadeMinLength, mfFlickerOffFadeMaxLength));
					}
					//Sound
					if(msFlickerOffSound!=""){
						cSoundEntity *pSound = mpWorld->CreateSoundEntity("FlickerOff",
																			msFlickerOffSound,true);
						if(pSound)
						{
							pSound->SetIsSaved(false);
							pSound->SetPosition(GetWorldPosition());
						}
					}

					OnFlickerOff();

					mfFlickerTime =0;
					mfFlickerStateLength = cMath::RandRectf(mfFlickerOffMinLength,mfFlickerOffMaxLength);
				}
			}
			//////////////////////
			//Off
			else 
			{
				if(mfFlickerTime >= mfFlickerStateLength)
				{
					mbFlickerOn = true;
					if(!mbFlickerFade)
					{
						SetDiffuseColor(mFlickerOnColor);
						SetIntensity(mfFlickerOnIntensity);
					}
					else
					{
						FadeTo(mFlickerOnColor,mfFlickerOnIntensity,cMath::RandRectf(mfFlickerOnFadeMinLength, mfFlickerOnFadeMaxLength));
					}
					if(msFlickerOnSound!=""){
						cSoundEntity *pSound = mpWorld->CreateSoundEntity("FlickerOn", msFlickerOnSound,true);
						if(pSound)
						{
							pSound->SetIsSaved(false);
							pSound->SetPosition(GetWorldPosition());
						}
					}

					OnFlickerOn();

					mfFlickerTime =0;
					mfFlickerStateLength = cMath::RandRectf(mfFlickerOnMinLength,mfFlickerOnMaxLength);
				}
			}

			mfFlickerTime += afTimeStep;
		}

		/*Log("Time: %f Length: %f FadeTime: %f Color: (%f %f %f %f)\n",mfFlickerTime, mfFlickerStateLength,
		mfFadeTime,
		mDiffuseColor.r,mDiffuseColor.g,
		mDiffuseColor.b,mDiffuseColor.a);*/
	}

	//-----------------------------------------------------------------------

	void iLight::FadeTo(const cColor& aCol, float afIntensity, float afTime)
	{
		if(afTime<=0) afTime = 0.0001f;

		mfFadeTime = afTime;

		mColAdd.r = (aCol.r - mDiffuseColor.r)/afTime;
		mColAdd.g = (aCol.g - mDiffuseColor.g)/afTime;
		mColAdd.b = (aCol.b - mDiffuseColor.b)/afTime;
		mColAdd.a = (aCol.a - mDiffuseColor.a)/afTime;

		mfIntensityAdd = (afIntensity - mfIntensity)/afTime;

		mfDestIntensity = afIntensity;
		mDestCol = aCol;
	}

	void iLight::StopFading()
	{
		mfFadeTime =0;
	}

	bool iLight::IsFading()
	{
		return mfFadeTime != 0;
	}

	//-----------------------------------------------------------------------

	void iLight::SetFlickerActive(bool abX)
	{
		mbFlickering = abX;
	}

	void iLight::SetFlicker(const cColor& aOffCol, float afOffIntensity,
		float afOnMinLength, float afOnMaxLength,const tString &asOnSound,const tString &asOnPS,
		float afOffMinLength, float afOffMaxLength,const tString &asOffSound,const tString &asOffPS,
		bool abFade,	float afOnFadeMinLength, float afOnFadeMaxLength, 
						float afOffFadeMinLength, float afOffFadeMaxLength)
	{
		mFlickerOffColor = aOffCol;
		mfFlickerOffIntensity = afOffIntensity;

		mfFlickerOnMinLength = afOnMinLength;
		mfFlickerOnMaxLength = afOnMaxLength;
		msFlickerOnSound = asOnSound;
		msFlickerOnPS = asOnPS;

		mfFlickerOffMinLength = afOffMinLength;
		mfFlickerOffMaxLength = afOffMaxLength;
		msFlickerOffSound = asOffSound;
		msFlickerOffPS = asOffPS;

		mbFlickerFade = abFade;

		mfFlickerOnFadeMinLength = afOnFadeMinLength;
		mfFlickerOnFadeMaxLength = afOnFadeMaxLength;
		mfFlickerOffFadeMinLength = afOffFadeMinLength;
		mfFlickerOffFadeMaxLength = afOffFadeMaxLength;

		mFlickerOnColor = mDiffuseColor;
		mfFlickerOnIntensity = mfIntensity;

		mbFlickerOn = true;
		mfFlickerTime =0;

		mfFadeTime =0;

		mfFlickerStateLength = cMath::RandRectf(mfFlickerOnMinLength,mfFlickerOnMaxLength);
	}

	//-----------------------------------------------------------------------
	
	bool iLight::CheckObjectIntersection(iRenderable *apObject)
	{
		//Log("------ Checking %s with light %s -----\n",apObject->GetName().c_str(), GetName().c_str());
		//Log(" BV: min: %s max: %s\n",	apObject->GetBoundingVolume()->GetMin().ToString().c_str(),
		//								apObject->GetBoundingVolume()->GetMax().ToString().c_str());
		
		//////////////////////////////////////////////////////////////
		// If the lights cast shadows, cull objects that are in shadow
		if(mbCastShadows)
		{
			return CollidesWithBV(apObject->GetBoundingVolume());
		}
		/////////////////////////////////////////////////
		//Light is not in shadow, do not do any culling
		else
		{
			//Log("No shadow, using BV\n");
			return CollidesWithBV(apObject->GetBoundingVolume());
		}

				
	}

	//-----------------------------------------------------------------------

	void iLight::SetIntensity(float afX)
	{
		// Spot reach is driven by radius, not intensity, so it skips the BV/
		// transform update the other light types do.
		if(std::holds_alternative<cLightSpotData>(mLightData))
		{
			mfIntensity = afX;
			return;
		}

		if(mfIntensity == afX) return;

		mfIntensity = afX;

		mbUpdateBoundingVolume = true;

		//This is so that the render container is updated.
		SetTransformUpdated();
	}

	//-----------------------------------------------------------------------

	void iLight::SetRadius(float afX)
	{
		if (mfRadius == afX) return;

		mfRadius = afX;

		mbUpdateBoundingVolume = true;

		// Spot reach feeds the frustum far-plane, so the cached projection must
		// rebuild too; the lazy getters build it once at first use.
		if(std::holds_alternative<cLightSpotData>(mLightData))
			mlBuiltTransform = -1;

		//This is so that the render container is updated.
		SetTransformUpdated();
	}

	//-----------------------------------------------------------------------

	void iLight::SetSourceRadius(float afX)
	{
		if (mfSourceRadius == afX) return;

		mfSourceRadius = afX;
	}

	//-----------------------------------------------------------------------

	void iLight::UpdateLogic(float afTimeStep)
	{
		UpdateLight(afTimeStep);
		if(mfFadeTime>0 || mbFlickering)
		{
			mbUpdateBoundingVolume = true;
			
			//This is so that the render container is updated.
			//SetTransformUpdated();
		}
	}

	//-----------------------------------------------------------------------

	cBoundingVolume* iLight::GetBoundingVolume()
	{
		if(mbUpdateBoundingVolume)
		{
			UpdateBoundingVolume();
			mbUpdateBoundingVolume = false;
		}

		return &mBoundingVolume;
	}
	
	//-----------------------------------------------------------------------

	cMatrixf* iLight::GetModelMatrix(cFrustum* apFrustum)
	{
		return &GetWorldMatrix();
	}
	
	//-----------------------------------------------------------------------
	
	//-----------------------------------------------------------------------

	void iLight::SetGoboTexture(Image* apImage)
	{
		m_goboImage = ImageResourceWrapper(mpTextureManager, apImage, /*autoDestroy=*/true);
	}

	Image* iLight::GetGoboImage() const
	{
		return m_goboImage.GetImage();
	}

	//-----------------------------------------------------------------------

	//////////////////////////////////////////////////////////////////////////
	// PROTECTED METHODS
	//////////////////////////////////////////////////////////////////////////

	//-----------------------------------------------------------------------

	void iLight::OnFlickerOff()
	{
		//Particle system
		if(msFlickerOffPS!=""){
			cParticleSystem *pPS = mpWorld->CreateParticleSystem(GetName() + "_PS", msFlickerOffPS, cVector3f(1,1,1));
			if(pPS) pPS->SetMatrix(GetWorldMatrix());
		}
	}

	//-----------------------------------------------------------------------

	void iLight::OnFlickerOn()
	{
		//Particle system
		if(msFlickerOnPS!=""){
			cParticleSystem *pPS = mpWorld->CreateParticleSystem(GetName() + "_PS", msFlickerOnPS, cVector3f(1,1,1));
			if(pPS) pPS->SetMatrix(GetWorldMatrix());
		}

	}

	//-----------------------------------------------------------------------

	void iLight::UpdateBoundingVolume()
	{
		if(std::holds_alternative<cLightSpotData>(mLightData))
		{
			mBoundingVolume = GetFrustum()->GetBoundingVolume();
		}
		else if(cLightBoxData* pBox = std::get_if<cLightBoxData>(&mLightData))
		{
			mBoundingVolume.SetSize(pBox->mvSize);
			mBoundingVolume.SetPosition(GetWorldPosition());
		}
		else // point
		{
			mBoundingVolume.SetSize(mfRadius*2);
			mBoundingVolume.SetPosition(GetWorldPosition());
		}
	}

	//-----------------------------------------------------------------------
	// Spot-light implementation
	//-----------------------------------------------------------------------

	void iLight::SetLightData(const tLightData& aData)
	{
		// The active alternative defines the light's type, which is fixed at
		// construction — only same-type parameter updates are allowed.
		assert(aData.index() == mLightData.index());
		mLightData = aData;

		if(std::holds_alternative<cLightSpotData>(mLightData))
		{
			// Projection parameters feed the cached matrices/frustum, and through
			// them the BV; transform update so the render container refreshes.
			mlBuiltTransform = -1;
			mbUpdateBoundingVolume = true;
			SetTransformUpdated();
		}
		else if(std::holds_alternative<cLightBoxData>(mLightData))
		{
			// Size feeds the BV; transform update so the render container refreshes.
			mbUpdateBoundingVolume = true;
			SetTransformUpdated();
		}
	}

	//-----------------------------------------------------------------------

	void iLight::UpdateData(const cLightSpotData& aData)
	{
		const int lTransform = GetTransformUpdateCount();
		if(mlBuiltTransform == lTransform)
			return;

		// View
		const cMatrixf mtxView = cMath::MatrixInverse(GetWorldMatrix());

		// Projection (VK clip convention, same form as the camera path)
		const float fFar = GetReach();
		const cMatrixf mtxProjection = cMath::MatrixPerspectiveProjection(
			aData.mfNearClipPlane, fFar, aData.mfFOV, aData.mfAspect, false);

		// View-projection (with texture-unit fix applied)
		m_mtxViewProj = cMath::MatrixMul(mtxProjection, mtxView);
		m_mtxViewProj = cMath::MatrixMul(g_mtxTextureUnitFix, m_mtxViewProj);

		// Frustum (keeps its own copies of the matrices)
		mpFrustum->SetupPerspectiveProj(mtxProjection,
										mtxView,
										fFar,aData.mfNearClipPlane,
										aData.mfFOV,aData.mfAspect,GetWorldPosition(),false);

		mlBuiltTransform = lTransform;
	}

	//-----------------------------------------------------------------------

	const cMatrixf& iLight::GetViewProjMatrix()
	{
		UpdateLightData();
		return m_mtxViewProj;
	}

	//-----------------------------------------------------------------------

	cFrustum* iLight::GetFrustum()
	{
		UpdateLightData();
		return mpFrustum;
	}

	//-----------------------------------------------------------------------

	void iLight::SetSpotFalloffMap(Image* apImage)
	{
		m_spotFalloffMap = ImageResourceWrapper(mpTextureManager, apImage, /*autoDestroy=*/true);
	}

	Image* iLight::GetSpotFalloffImage() const
	{
		return m_spotFalloffMap.GetImage();
	}

	//-----------------------------------------------------------------------

	bool iLight::CollidesWithBV(cBoundingVolume *apBV)
	{
		if(std::holds_alternative<cLightSpotData>(mLightData))
		{
			if(cMath::CheckBVIntersection(*GetBoundingVolume(), *apBV)==false) return false;
			return GetFrustum()->CollideBoundingVolume(apBV)!= eCollision_Outside;
		}
		return iRenderable::CollidesWithBV(apBV);
	}

	bool iLight::CollidesWithFrustum(cFrustum *apFrustum)
	{
		if(std::holds_alternative<cLightSpotData>(mLightData))
			return apFrustum->CollideFrustum(GetFrustum())!=eCollision_Outside;
		return iRenderable::CollidesWithFrustum(apFrustum);
	}

	//-----------------------------------------------------------------------

}
