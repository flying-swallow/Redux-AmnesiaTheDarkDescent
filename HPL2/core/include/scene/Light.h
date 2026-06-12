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

#ifndef HPL_LIGHT_H
#define HPL_LIGHT_H

#include "scene/Entity3D.h"
#include "scene/SceneTypes.h"
#include "graphics/GraphicsTypes.h"
#include "graphics/ImageResourceWrapper.h"
#include "graphics/Renderable.h"

#include <variant>

namespace hpl {

	//------------------------------------------

	class cFrustum;
	class Image;
	class cTextureManager;
	class cResources;
	class cWorld;

	//------------------------------------------

	// Per-type parameter blocks for the (formerly subclassed) light kinds —
	// pure authored/modifiable fields only, plain and copyable. iLight holds one
	// of these in a variant; which alternative is held defines the light's type.
	// Derived state (matrices/frustum) lives on iLight and is rebuilt from these.
	// Read via GetLightData(), edit a copy and feed it back via SetLightData().

	struct cLightPointData
	{
		// Point lights need no parameters beyond the shared radius/intensity.
	};

	struct cLightSpotData
	{
		float mfFOV = 1.04719755f;	// 60 degrees
		float mfAspect = 1.0f;
		float mfNearClipPlane = 0.1f;
	};

	struct cLightBoxData
	{
		cVector3f mvSize = cVector3f(1);
		eLightBoxBlendFunc mBlendFunc = eLightBoxBlendFunc_Replace;
	};

	// Active per-type parameters. Which alternative is held defines the light's
	// type; inspect with std::holds_alternative / std::get_if / std::visit.
	typedef std::variant<cLightPointData, cLightSpotData, cLightBoxData> tLightData;

	//------------------------------------------

	class iLight : public iRenderable
	{
	public:
		iLight(tLightData aData, tString asName, cResources *apResources);
		virtual ~iLight();

		void UpdateLogic(float afTimeStep) override;

		bool CheckObjectIntersection(iRenderable *apObject);

		const tLightData& GetLightData() const { return mLightData; }
		// Feed edited parameters back. The active alternative must match the
		// light's type (the type is fixed at construction); triggers the
		// per-type derived-state updates (spot matrices/frustum, box BV).
		void SetLightData(const tLightData& aData);

		// Image* binding API.
		void SetGoboTexture(Image* apImage);
		Image* GetGoboImage() const;

		///////////////////////////////
		//iEntity implementation
		tString GetEntityType() override { return "iLight";}

		bool IsVisible() override;

		///////////////////////////////
		//Renderable implementation:
		cMaterial *GetMaterial() override { return NULL;}
		cVertexBuffer* GetVertexBuffer() override { return NULL;}

		eRenderableType GetRenderType() override { return eRenderableType_Light;}

		cBoundingVolume* GetBoundingVolume() override;

		int GetMatrixUpdateCount() override { return GetTransformUpdateCount();}

		cMatrixf* GetModelMatrix(cFrustum* apFrustum) override;

        //////////////////////////
		//Fading
		void FadeTo(const cColor& aCol, float afIntensity, float afTime);
		void StopFading();
		bool IsFading();
		cColor GetDestColor(){ return mDestCol;}
		float GetDestIntensity(){ return mfDestIntensity;}


		//////////////////////////
		//FLickering
		void SetFlickerActive(bool abX);
		bool GetFlickerActive(){return mbFlickering;}

		void SetFlicker(const cColor& aOffCol, float afOffIntensity,
			float afOnMinLength, float afOnMaxLength,const tString &asOnSound,const tString &asOnPS,
			float afOffMinLength, float afOffMaxLength,const tString &asOffSound,const tString &asOffPS,
			bool abFade,	float afOnFadeMinLength, float afOnFadeMaxLength, 
							float afOffFadeMinLength, float afOffFadeMaxLength);

		tString GetFlickerOffSound(){ return msFlickerOffSound;}
		tString GetFlickerOnSound(){ return msFlickerOnSound;}
		tString GetFlickerOffPS(){ return msFlickerOffPS;}
		tString GetFlickerOnPS(){ return msFlickerOnPS;}
		float GetFlickerOnMinLength(){ return mfFlickerOnMinLength;}
		float GetFlickerOffMinLength(){ return mfFlickerOffMinLength;}
		float GetFlickerOnMaxLength(){ return mfFlickerOnMaxLength;}
		float GetFlickerOffMaxLength(){ return mfFlickerOffMaxLength;}
		cColor GetFlickerOffColor(){ return mFlickerOffColor;}
		float GetFlickerOffIntensity(){ return mfFlickerOffIntensity;}
		bool GetFlickerFade(){ return mbFlickerFade;}
		float GetFlickerOnFadeMinLength(){ return mfFlickerOnFadeMinLength;}
		float GetFlickerOnFadeMaxLength(){ return mfFlickerOnFadeMaxLength;}
		float GetFlickerOffFadeMinLength(){ return mfFlickerOffFadeMinLength;}
		float GetFlickerOffFadeMaxLength(){ return mfFlickerOnFadeMaxLength;}

		cColor GetFlickerOnColor(){ return mFlickerOnColor;}
		float GetFlickerOnIntensity(){ return mfFlickerOnIntensity;}

		//////////////////////////
		//Properties
		const cColor& GetDiffuseColor(){ return mDiffuseColor; }
		void SetDiffuseColor(cColor aColor);
		
		const cColor&  GetDefaultDiffuseColor(){ return mDefaultDiffuseColor;}
		void SetDefaultDiffuseColor(const cColor& aColor) { mDefaultDiffuseColor = aColor; }
		
		bool GetCastShadows(){ return mbCastShadows;}
		void SetCastShadows(bool afX){ mbCastShadows = afX;}

		tObjectVariabilityFlag GetShadowCastersAffected(){ return mlShadowCastersAffected;}
		void SetShadowCastersAffected(tObjectVariabilityFlag alX){ mlShadowCastersAffected = alX;}

		inline eShadowMapResolution GetShadowMapResolution() const{ return mShadowMapResolution;}
		inline void SetShadowMapResolution(eShadowMapResolution aQuality){ mShadowMapResolution  = aQuality;}

		void SetIntensity(float afX);
		float GetIntensity(){return mfIntensity;}

		void SetRadius(float afX);
		float GetRadius() { return mfRadius; }

		void SetSourceRadius(float afX);
		float GetSourceRadius(){ return mfSourceRadius; }

		void UpdateLight(float afTimeStep);

		void SetWorld(cWorld *apWorld){ mpWorld = apWorld;}

		//////////////////////////
		// Spot-light API (valid when the light holds cLightSpotData) — derived
		// state and resources; the parameters live in cLightSpotData.
		const cMatrixf& GetViewProjMatrix();

		// Falls back to intensity when no radius is authored (PBR maps derive
		// reach from intensity) so code-created spots never get a zero far plane.
		float GetReach() const { return mfRadius > 0.f ? mfRadius : mfIntensity; }

		cFrustum* GetFrustum();

		void SetSpotFalloffMap(Image* apImage);
		Image* GetSpotFalloffImage() const;

		bool CollidesWithBV(cBoundingVolume *apBV) override;
		bool CollidesWithFrustum(cFrustum *apFrustum) override;

	protected:
		void OnFlickerOff();
		void OnFlickerOn();

		void UpdateBoundingVolume();

		// Lazily rebuilds cached per-type derived state (spot matrices/frustum);
		// cheap no-op when nothing changed, so getters call it freely. Dispatches
		// on the active alternative — adding a light type means adding a data
		// struct to tLightData plus one UpdateData overload.
		void UpdateLightData(){ std::visit([this](const auto& aData){ UpdateData(aData); }, mLightData); }

		void UpdateData(const cLightPointData& aData){}
		void UpdateData(const cLightSpotData& aData);
		void UpdateData(const cLightBoxData& aData){}

		tLightData mLightData;

		// Spot derived caches + resource (valid when holding cLightSpotData).
		cMatrixf m_mtxViewProj;
		cFrustum *mpFrustum = nullptr;
		ImageResourceWrapper m_spotFalloffMap;
		// Transform count the caches were last built against; -1 forces a rebuild
		// (the entity count never goes negative), so projection-parameter changes
		// (FOV/aspect/near/reach) just reset it instead of tracking their own dirty.
		int mlBuiltTransform = -1;

		cTextureManager *mpTextureManager;
		cWorld *mpWorld;

		// Image* texture storage.
		ImageResourceWrapper m_goboImage;

		eShadowMapResolution mShadowMapResolution;

		cColor mDiffuseColor;
		cColor mDefaultDiffuseColor;

		float mfIntensity;
		float mfRadius;
		float mfSourceRadius;

		bool mbCastShadows;
		tObjectVariabilityFlag mlShadowCastersAffected;

		///////////////////////////
		//Fading.
		cColor mColAdd;
		float mfIntensityAdd;
		cColor mDestCol;
		float mfDestIntensity;
		float mfFadeTime;

		///////////////////////////
		//Flicker
		bool mbFlickering;
		tString msFlickerOffSound;
		tString msFlickerOnSound;
		tString msFlickerOffPS;
		tString msFlickerOnPS;
		float mfFlickerOnMinLength;
		float mfFlickerOffMinLength;
		float mfFlickerOnMaxLength;
		float mfFlickerOffMaxLength;
		cColor mFlickerOffColor;
		float mfFlickerOffIntensity;
		bool mbFlickerFade;
		float mfFlickerOnFadeMinLength;
		float mfFlickerOnFadeMaxLength;
		float mfFlickerOffFadeMinLength;
		float mfFlickerOffFadeMaxLength;

		cColor mFlickerOnColor;
		float mfFlickerOnIntensity;

		bool mbFlickerOn;
		float mfFlickerTime;
		float mfFlickerStateLength;
	};

	typedef std::list<iLight*> tLightList;
	typedef tLightList::iterator tLightListIt;

	// The light kinds were flattened into the single concrete iLight (which
	// holds per-type state in a variant). These aliases remain for source
	// compatibility; construct via cWorld::CreateLightPoint/Spot/Box.
	using cLightPoint = iLight;
	using cLightSpot = iLight;
	using cLightBox = iLight;
};
#endif // HPL_LIGHT_H
