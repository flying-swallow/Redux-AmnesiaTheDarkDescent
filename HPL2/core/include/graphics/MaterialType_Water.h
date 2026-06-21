/*
 * Copyright © 2009-2020 Frictional Games
 *
 * This file is part of Amnesia: The Dark Descent.
 *
 * Amnesia: The Dark Descent is free software: you can redistribute it and/or
 modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.

 * Amnesia: The Dark Descent is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Amnesia: The Dark Descent.  If not, see
 <https://www.gnu.org/licenses/>.
 */

#ifndef HPL_MATERIAL_WATER_H
#define HPL_MATERIAL_WATER_H

#include "graphics/Material.h"
#include "graphics/MaterialType.h"

namespace hpl {

//---------------------------------------------------
// WATER
//---------------------------------------------------

class cMaterialType_Water : public iMaterialType {
public:
  cMaterialType_Water(cGraphics *apGraphics, cResources *apResources);
  ~cMaterialType_Water();

  bool SupportsHWSkinning() { return false; }

  MaterialID GetMaterialID() const override { return MaterialID::Water; }

  void LoadVariables(cMaterial *apMaterial, cResourceVarsObject *apVars);
  void GetVariableValues(cMaterial *apMaterial, cResourceVarsObject *apVars);

private:
  void LoadData() {}    // STUB
  void DestroyData() {} // STUB
};

//---------------------------------------------------

}; // namespace hpl
#endif // HPL_MATERIAL_WATER_H
