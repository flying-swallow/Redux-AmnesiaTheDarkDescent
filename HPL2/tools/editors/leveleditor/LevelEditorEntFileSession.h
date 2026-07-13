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

#ifndef LEVEL_EDITOR_ENT_FILE_SESSION_H
#define LEVEL_EDITOR_ENT_FILE_SESSION_H

#include "hpl.h"
using namespace hpl;

#include "../common/EditorTypes.h"
#include "../common/PrefabManager.h"

#include <vector>

//---------------------------------------------------------------

class cLevelEditor;
class cModelEditorWorld;
class cEditorEditModeSelect;
class iEditorEditMode;
class iEditorWorld;
class iEntityWrapper;
class iEntityWrapperType;

//---------------------------------------------------------------

////////////////////////////////////////////////////////////////
// cLevelEditorEntFileSession
//  The single transient editing session created while the Level Editor is
//  "scoped into" a placed model, letting it edit that model's embedded lights /
//  particles / sounds / bodies IN PLACE (the map stays rendered) WITHOUT opening
//  the standalone Model Editor.
//
//  It owns a headless cModelEditorWorld (the same world/wrapper/save machinery the
//  Model Editor uses) with every editable sub-entity type registered by its own
//  Select + creator edit modes, so the file's sub-objects become real
//  iEntityWrapper objects with stock property panels (CreateEditBox) and undoable
//  actions. The session's own hidden cWorld never renders in the Level viewport;
//  the placed instance in the map provides the visuals.
//
//  Definition I/O goes entirely through cPrefabManager (the same store the placed
//  instances are bound to via cEntityWrapperEntity::mPrefabRef) — the session never
//  touches the .ent file itself. Load() PINS the prefab resource (mPrefabRef) and
//  reads its shared in-memory doc (EnsureDoc materializes it from disk on the first
//  scope-in), so edits ride the same doc the instances bind to; Save() hands the
//  edited doc back as the prefab's pending definition (SetPendingDoc), which
//  live-reloads every placed instance and writes to disk on map save — the same
//  store the MCP define_entity_file / update_prefab tools use.
//
//  MOUNTING: on Load() each top-level sub-entity is transformed to world coords =
//  placedEntity.WorldMatrix × local, so its icon / pick bounding-volume overlays
//  the placed model in the map viewport (that's how you click a light in the
//  scene). Save() converts back to local for XML, restores the exact world state.
//
//  cLevelEditor owns the (single) session and its lifetime: created on
//  EnterEntFileScope, destroyed on ExitEntFileScope / new / load map.
class cLevelEditorEntFileSession
{
public:
	// asFile is the resource-relative .ent path (as stored on the placed entity
	// wrapper). Registers the sub-entity types + edit modes; call Load() to populate.
	cLevelEditorEntFileSession(cLevelEditor* apEditor, const tString& asFile);
	~cLevelEditorEntFileSession();

	const tString& GetFile() { return msFile; }

	// The session's world as the base interface (callers only need GetEntity /
	// GetEntities / GetNumModifications / IsModified). Out-of-line so the upcast
	// sees the full cModelEditorWorld type.
	iEditorWorld* GetWorld();

	// The placed entity's world matrix. Sub-entities are mounted (overlaid) onto it
	// so their icons / pick volumes sit on the model in the map. Set BEFORE Load().
	void SetMountMatrix(const cMatrixf& amtxMount);

	// Pin the prefab resource and load its shared in-memory definition (materialized
	// from disk on first scope-in) into the world, mounting its sub-entities onto the
	// mount matrix. Returns false if the definition could not be found / parsed. Safe
	// to call again to re-sync (clears + reloads).
	bool Load();

	// Un-mount (world -> local) -> serialize to .ent XML -> restore exact world state,
	// then hand the doc to cPrefabManager as this prefab's pending definition.
	// abBroadcast live-reloads placed instances. Not written to disk until map save.
	void Save(bool abBroadcast = true);

	// Per-frame tick (driven by cLevelEditor::OnUpdate while scoped, NOT any GUI
	// window): polls the session world's modification counter and, on a debounced
	// bump, Save()s so scoped edits are ALWAYS pushed to every placed instance —
	// persistence never depends on a panel/window being pumped.
	void Update(float afTimeStep);

	// True if the session world has un-saved user edits (mounting/un-mounting does
	// NOT count — it goes through direct setters, not undoable actions).
	bool IsDirty();

	// Top-level editable sub-entities (lights / particles / sounds / billboards /
	// bodies) — SubMesh/Bone and body-owned shapes are filtered out. Rebuilt into
	// avOut (cleared first).
	void GetSubEntities(tEntityWrapperList& avOut);

	// The category buckets the hierarchy shows, in display order.
	static const int* GetCategoryOrder(int& alCountOut);

	// A cEditorEditModeSelect BOUND TO THIS WORLD. Made the editor's current mode
	// while scoped (so viewport picking + icon drawing follow this world), and the
	// apEditMode argument to iEntityWrapper::CreateEditBox (so panel actions resolve
	// against this world, not the map).
	cEditorEditModeSelect* GetSelectMode() { return mpSelectMode; }

	// The session's edit modes (Select + the Add-Light/Particle/Sound/Billboard/Body
	// creators), all bound to this world — the scoped edit-mode toolbar is built
	// from these. Never added to the editor's mvEditModes.
	const std::vector<iEditorEditMode*>& GetEditModes() { return mvEditModes; }

	// True if alTypeID is one of the categories GetSubEntities lists.
	static bool IsEditableCategory(int alTypeID);

private:
	// Mount / un-mount helpers around the mount matrix (see class comment).
	void MountSubEntities();

	cLevelEditor* mpEditor;
	tString msFile;

	cModelEditorWorld* mpWorld;

	// Pure lifetime pin to the prefab resource, so the resource (and its shared
	// in-memory definition doc) survives for the whole scope — the session edits the
	// SAME doc the placed instances bind to. No callbacks; RAII-released in the dtor.
	SharedResourceHandle<cPrefabResource> mPrefabRef;

	// Change-poll bookkeeping (see Update): mlSyncModStamp is the last-seen session
	// world modification count, re-based to the clean baseline in Load(); mfSyncDebounce
	// is the countdown that coalesces an edit burst into one Save + live-reload.
	unsigned int mlSyncModStamp = 0;
	float        mfSyncDebounce = 0.0f;

	// Owned edit modes bound to mpWorld. mpSelectMode aliases mvEditModes[0].
	cEditorEditModeSelect* mpSelectMode;
	std::vector<iEditorEditMode*> mvEditModes;

	cMatrixf mmtxMount;
};

//---------------------------------------------------------------

#endif // LEVEL_EDITOR_ENT_FILE_SESSION_H
