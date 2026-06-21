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

#ifndef HPL_LIGHT_AREA_H
#define HPL_LIGHT_AREA_H

#include "scene/Light.h"
#include "scene/SceneTypes.h"

namespace hpl {

	//------------------------------------------

	// Rectangular area light, modeled on Unreal's Rect Light. The emitter is a
	// SourceWidth×SourceHeight rectangle centered at the light's world position,
	// lying in the local Y–Z plane (local +Y = width axis, +Z = height axis, +X =
	// one-sided emission normal). Intensity is averaged over the surface area, the
	// generic light Radius is the attenuation radius, and an optional source texture
	// (base gobo slot) tints the emission. BarnDoorAngle/Length shape the beam: edge
	// flaps that confine the emission to a frustum (90°/0 leaves them open). Rendered
	// by the hybrid renderer as the RectLight ILight type; analytic closest-point
	// shading in ILight.slang.
	class cLightArea : public iLight
	{
	public:
		cLightArea(tString asName, cResources *apResources);

		// Source Width — extent along the light's local Y axis.
		void SetWidth(float afX);
		inline float GetWidth() const { return mfWidth; }

		// Source Height — extent along the light's local Z axis.
		void SetHeight(float afX);
		inline float GetHeight() const { return mfHeight; }

		// Barn doors (beam-shaping edge flaps). Angle in radians (90° = open), length in world units (0 = disabled).
		void SetBarnDoorAngle(float afX){ mfBarnDoorAngle = afX; }
		inline float GetBarnDoorAngle() const { return mfBarnDoorAngle; }

		void SetBarnDoorLength(float afX){ mfBarnDoorLength = afX; }
		inline float GetBarnDoorLength() const { return mfBarnDoorLength; }

	private:
		void ExtraXMLProperties(tinyxml2::XMLElement *apMainElem);
		void UpdateBoundingVolume();

		float mfWidth;
		float mfHeight;
		float mfBarnDoorAngle;
		float mfBarnDoorLength;
	};

};
#endif // HPL_LIGHT_AREA_H
