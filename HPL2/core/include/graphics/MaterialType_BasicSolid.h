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

#ifndef HPL_MATERIAL_TYPE_BASIC_SURFACES_H
#define HPL_MATERIAL_TYPE_BASIC_SURFACES_H

#include "graphics/Image.h"
#include "graphics/MaterialType.h"
#include "graphics/Material.h"

namespace hpl {

	//---------------------------------------------------
	// SOLID BASE
	//---------------------------------------------------

	class iMaterialType_SolidBase : public iMaterialType
	{
	public:
		iMaterialType_SolidBase(cGraphics *apGraphics, cResources *apResources);
		~iMaterialType_SolidBase();

		bool SupportsHWSkinning() { return true; }

		void LoadVariables(cMaterial* apMaterial, cResourceVarsObject* apVars) {} // STUB
		void GetVariableValues(cMaterial* apMaterial, cResourceVarsObject* apVars) {} // STUB

	protected:
		virtual void LoadSpecificData() = 0;

		void LoadData();
		void DestroyData() {} // STUB

		bool mbIsGlobalDataCreator;
		static bool mbGlobalDataCreated;
		//[skeleton][uv animation]

		Image* mpDissolveTexture;

		static float mfVirtualPositionAddScale;
	};

	//---------------------------------------------------
	// SOLID DIFFUSE
	//---------------------------------------------------

	class cMaterialType_SolidDiffuse : public iMaterialType_SolidBase
	{
	public:
		cMaterialType_SolidDiffuse(cGraphics *apGraphics, cResources *apResources);
		~cMaterialType_SolidDiffuse();

		bool SupportsHWSkinning() { return true; }

		MaterialID GetMaterialID() const override { return MaterialID::SolidDiffuse; }

		void LoadVariables(cMaterial *apMaterial, cResourceVarsObject *apVars);
		void GetVariableValues(cMaterial *apMaterial, cResourceVarsObject *apVars);

	private:
		void LoadSpecificData() {} // STUB
	};

	//---------------------------------------------------

};
#endif // HPL_MATERIAL_TYPE_BASIC_SURFACES_H
