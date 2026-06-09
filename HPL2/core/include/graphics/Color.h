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

#ifndef HPL_COLOR_H
#define HPL_COLOR_H

#include <cmath>
#include <list>
#include <vector>

#include "system/SystemTypes.h"

namespace hpl {

	class cColor
	{
	public:
		union{
			struct {
				float r,g,b,a;
			};
			float v[4];
		};	
		
		cColor(float afR, float afG, float afB, float afA);
		cColor(float afR, float afG, float afB);
		cColor();
		cColor(float afVal);
		cColor(float afVal, float afA);

		cColor operator*(float afVal) const;
		cColor operator/(float afVal) const;
		
		cColor operator+(const cColor &aCol) const;
		cColor operator-(const cColor &aCol) const;
		cColor operator*(const cColor &aCol) const;
		cColor operator/(const cColor &aCol) const;
		
		bool operator==(cColor aCol) const;

		tString ToString() const;

		tString ToFileString() const;

		void FromVec(float *apV);
	};

	// sRGB → linear transfer (IEC 61966-2-1). Inverse of the swapchain's write
	// encode; brings an artist-authored / display-space channel into the
	// shaders' linear lighting space (and the linear HDR pogo target that
	// additive effects blend into). Used by HybridRenderer for light/fog
	// colours and by cLuxEffectRenderer for its glow colours.
	inline float sRGBToLinear(float afC)
	{
		if (afC <= 0.04045f) return afC / 12.92f;
		return std::pow((afC + 0.055f) / 1.055f, 2.4f);
	}

	// Per-channel sRGB → linear; alpha is a linear weight and is left untouched.
	inline cColor sRGBToLinear(const cColor &aCol)
	{
		return cColor(sRGBToLinear(aCol.r), sRGBToLinear(aCol.g),
					  sRGBToLinear(aCol.b), aCol.a);
	}

	typedef std::list<cColor> tColorList;
	typedef tColorList::iterator tColorListIt;

	typedef std::vector<cColor> tColorVec;
	typedef tColorVec::iterator tColorVecIt;

};
#endif // HPL_COLOR_H
