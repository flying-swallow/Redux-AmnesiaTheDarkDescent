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

#ifndef HPL_LIGHT_SPOT_H
#define HPL_LIGHT_SPOT_H

#include "graphics/ImageResourceWrapper.h"
#include "scene/Light.h"

namespace hpl {

	class cResources;
	class iTexture;
	class Image;
	class cFrustum;

	//------------------------------------------

	class cLightSpot : public iLight
	{
	public:
		cLightSpot(tString asName, cResources *apResources);
		~cLightSpot();

		const cMatrixf& GetViewMatrix();
		const cMatrixf& GetProjectionMatrix();
		const cMatrixf& GetViewProjMatrix();

		void SetFOV(float afAngle);
		inline float GetFOV() const { return mfFOV;}
		
		inline float GetTanHalfFOV() const{return mfTanHalfFOV; }
		inline float GetCosHalfFOV() const{return mfCosHalfFOV;}

		void SetAspect(float afAngle) { mfAspect = afAngle; mbProjectionUpdated = true;}
		float GetAspect() { return mfAspect;}

		void SetNearClipPlane(float afX) { mfNearClipPlane = afX; mbProjectionUpdated = true;}
		float GetNearClipPlane() { return mfNearClipPlane;}

		void SetIntensity(float afX) override;
		void SetRadius(float afX) override;

		// Frustum far-plane / cull reach = the authored radius (mfRadius);
		// fall back to intensity only if a map authored no radius at all.
		float GetReach() const { return mfRadius > 0.f ? mfRadius : mfIntensity; }

		cFrustum* GetFrustum();

		[[deprecated("use GetSpotFalloffImage")]]
		iTexture *GetSpotFalloffMap();
		[[deprecated("use SetSpotFalloffMap(Image*)")]]
		void SetSpotFalloffMap(iTexture* apTexture);

		void SetSpotFalloffMap(Image* apImage);
		Image* GetSpotFalloffImage() const;

		bool CollidesWithBV(cBoundingVolume *apBV);
		bool CollidesWithFrustum(cFrustum *apFrustum);

	private:
		void ExtraXMLProperties(TiXmlElement *apMainElem);
		void UpdateBoundingVolume();
		

        cMatrixf m_mtxProjection;
		cMatrixf m_mtxViewProj;
		cMatrixf m_mtxView;

		cFrustum *mpFrustum;

		iTexture *mpSpotFalloffMap = nullptr;

		// Image* parallel storage.
		ImageResourceWrapper m_spotFalloffMap;

		float mfFOV;
		float mfAspect;
		float mfNearClipPlane;

		bool mbFovUpdated;
		float mfTanHalfFOV;
		float mfCosHalfFOV;
		

		bool mbProjectionUpdated;
		bool mbViewProjUpdated;
		bool mbFrustumUpdated;

		int mlViewProjMatrixCount;
		int mlViewMatrixCount;
		int mlFrustumMatrixCount;
	};

};
#endif // HPL_LIGHT_SPOT_H
