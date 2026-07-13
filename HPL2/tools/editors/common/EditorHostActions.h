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

#ifndef HPLEDITOR_EDITOR_HOST_ACTIONS_H
#define HPLEDITOR_EDITOR_HOST_ACTIONS_H

#include "../common/StdAfx.h"

using namespace hpl;

//---------------------------------------------------------------

////////////////////////////////////////////////////////////////
// iEditorHostActions
//	Optional capabilities a CONCRETE editor (today only cLevelEditor) offers to the
//	SHARED editor widgets — the entity / particle-system edit-boxes and the edit-mode
//	sidebar. These are deliberately NOT on iEditorBase: cModelEditor / cParticleEditor
//	/ cMaterialEditor neither need nor should carry them. A shared widget discovers the
//	capability with dynamic_cast<iEditorHostActions*>(GetEditor()) — a NULL result means
//	"not supported", which is exactly how the LevelEditor-only buttons stay hidden
//	elsewhere. Bare interface: no data, no base, just the contract.
class iEditorHostActions
{
public:
	virtual ~iEditorHostActions() {}

	// Launch the co-located standalone editors on a resource-relative file.
	virtual void OpenInModelEditor(const tString& asEntFile) = 0;
	virtual void OpenInParticleEditor(const tString& asPsFile) = 0;

	// Ent-file scope: edit a placed model's embedded lights/particles/sounds/bodies
	// in place (the .ent definition, so every placed instance updates live).
	virtual void EnterEntFileScope(int alEntityID) = 0;
	virtual void ExitEntFileScope() = 0;
	virtual bool IsEntFileScoped() = 0;

	// While scoped, (re)show the scoped-into entity's OWN property box — wired to the
	// edit-mode sidebar's "Entity" button. Only meaningful while IsEntFileScoped().
	virtual void ShowScopedEntityProperties() = 0;
};

//---------------------------------------------------------------

#endif // HPLEDITOR_EDITOR_HOST_ACTIONS_H
