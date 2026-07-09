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

#ifndef HPL_GUI_MATERIAL_BASIC_TYPES_H
#define HPL_GUI_MATERIAL_BASIC_TYPES_H

#include "gui/GuiMaterial.h"

namespace hpl {

	//-----------------------------------------
	
	class cGuiMaterial_Diffuse : public iGuiMaterial
	{
	public:
		cGuiMaterial_Diffuse():iGuiMaterial("Diffuse"){}
		~cGuiMaterial_Diffuse() {}

		void BeforeRender() {} // STUB
		void AfterRender() {} // STUB
	};

	//-----------------------------------------

	class cGuiMaterial_Alpha : public iGuiMaterial
	{
	public:
		cGuiMaterial_Alpha():iGuiMaterial("Alpha"){}
		~cGuiMaterial_Alpha() {}

		void BeforeRender() {} // STUB
		void AfterRender() {} // STUB
	};

	//-----------------------------------------

	class cGuiMaterial_FontNormal : public iGuiMaterial
	{
	public:
		cGuiMaterial_FontNormal():iGuiMaterial("FontNormal"){}
		~cGuiMaterial_FontNormal() {}

		void BeforeRender() {} // STUB
		void AfterRender() {} // STUB
	};

	//-----------------------------------------

	class cGuiMaterial_Additive : public iGuiMaterial
	{
	public:
		cGuiMaterial_Additive():iGuiMaterial("Additive"){}
		~cGuiMaterial_Additive() {}

		void BeforeRender() {} // STUB
		void AfterRender() {} // STUB
	};

	//-----------------------------------------

	class cGuiMaterial_Modulative : public iGuiMaterial
	{
	public:
		cGuiMaterial_Modulative():iGuiMaterial("Modulative"){}
		~cGuiMaterial_Modulative() {}

		void BeforeRender() {} // STUB
		void AfterRender() {} // STUB
	};

	//-----------------------------------------
	
	class cGuiMaterial_PremulAlpha : public iGuiMaterial
	{
	public:
		cGuiMaterial_PremulAlpha():iGuiMaterial("Alpha"){}
		~cGuiMaterial_PremulAlpha() {}

		void BeforeRender() {} // STUB
		void AfterRender() {} // STUB
	};

	//-----------------------------------------

};
#endif // HPL_GUI_MATERIAL_BASIC_TYPES_H
