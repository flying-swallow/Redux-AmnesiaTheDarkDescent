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

#include "LevelEditorEntFileSession.h"

#include "LevelEditor.h"

#include <tinyxml2.h>

// The Model Editor's own world (same wrapper/save machinery), reused headless —
// exactly as the MCP create/define_entity_file tools do (cHeadlessEntWorld).
#include "../modeleditor/ModelEditorWorld.h"

#include "../common/EditorEditModeSelect.h"
#include "../common/EditorEditModeLights.h"
#include "../common/EditorEditModeParticleSystems.h"
#include "../common/EditorEditModeSounds.h"
#include "../common/EditorEditModeBillboards.h"
#include "../common/EntityWrapper.h"
#include "../common/PrefabManager.h"

// Body (shape) + Joint creator edit modes. Now compiled into the LevelEditor build
// (premake tools.lua), so the scoped session can CREATE and edit shapes/joints in place.
// Their SetUpTypes()/CreateTypes() also register the BodyShape/Body/Joint types, so a
// placed model that embeds them still loads as wrappers and round-trips through Save.
#include "../common/EditorEditModeBodies.h"
#include "../common/EditorEditModeJoints.h"

//-------------------------------------------------------------------------------

// Category buckets shown when scoped into an entity, in display order. Body-owned
// shapes and mesh-derived SubMesh/Bone are intentionally excluded here (shapes
// nest under their Body).
static const int gvEntFileCategoryOrder[] =
{
	eEditorEntityType_Light,
	eEditorEntityType_ParticleSystem,
	eEditorEntityType_Sound,
	eEditorEntityType_Billboard,
	eEditorEntityType_Body,
	eEditorEntityType_Joint,
};
static const int glEntFileCategoryOrderNum = (int)(sizeof(gvEntFileCategoryOrder)/sizeof(gvEntFileCategoryOrder[0]));

//-------------------------------------------------------------------------------

cLevelEditorEntFileSession::cLevelEditorEntFileSession(cLevelEditor* apEditor, const tString& asFile)
{
	mpEditor  = apEditor;
	msFile    = asFile;
	mmtxMount = cMatrixf::Identity;

	// A headless cModelEditorWorld gets its OWN hidden cWorld, so its sub-entities
	// never render in the Level viewport. It already registers SubMesh/Bone and the
	// <Mesh>/<Bones>/<Animations> save-load; the edit modes below register the
	// remaining editable types (Select -> CompoundObject, the creators -> their
	// light/particle/sound/billboard/body(shape)/joint types) — the SAME registration
	// path the Model Editor uses, so there is exactly one source of types (no double-add).
	mpWorld = hplNew(cModelEditorWorld,(apEditor));

	// Select first (its ctor registers CompoundObject and clears the global
	// selection — which we want on scope-enter anyway), then the creators. All are
	// bound to mpWorld; none are added to the editor's mvEditModes (the scoped
	// toolbar is built from GetEditModes()).
	mpSelectMode = hplNew(cEditorEditModeSelect,(apEditor, mpWorld));
	mvEditModes.push_back(mpSelectMode);

	iEditorEditModeObjectCreator* vCreators[] =
	{
		hplNew(cEditorEditModeLights,         (apEditor, mpWorld)),
		hplNew(cEditorEditModeParticleSystems,(apEditor, mpWorld)),
		hplNew(cEditorEditModeSounds,         (apEditor, mpWorld)),
		hplNew(cEditorEditModeBillboards,     (apEditor, mpWorld)),
		hplNew(cEditorEditModeBodies,         (apEditor, mpWorld)),
		hplNew(cEditorEditModeJoints,         (apEditor, mpWorld)),
	};
	for(size_t i=0; i<sizeof(vCreators)/sizeof(vCreators[0]); ++i)
	{
		// OnAdd() runs SetUpTypes() (registers the mode's types against mpWorld);
		// SetSubType(0) initialises the default sub-type creator (e.g. the Lights
		// mode's point-light sphere creator). Each creator's subtype panel is built
		// per-mode in cLevelEditor::DoEnterEntFileScope, which lets the user pick a
		// different subtype (box/sphere shape, ball/hinge joint, spot light, …).
		vCreators[i]->OnAdd();
		vCreators[i]->SetSubType(0);
		mvEditModes.push_back(vCreators[i]);
	}

	// BodyShape/Body and the four joint types are already registered above by the
	// Bodies/Joints creator modes' SetUpTypes()/CreateTypes() — do NOT re-add them here
	// (a second AddEntityType for the same type id would double-register).

	mpWorld->Reset();

	// Populate the Select mode's type-filter list from the session world's registered
	// types (Light/Particle/Sound/Billboard/Body/Joint + Compound/SubMesh/Bone/BodyShape),
	// the same step iEditorBase::Init runs for normal editors. Required so the scoped
	// "Select Object Type" panel has its per-type filter buttons.
	mpSelectMode->PostEditModesCreation();
}

//-------------------------------------------------------------------------------

cLevelEditorEntFileSession::~cLevelEditorEntFileSession()
{
	// Modes first (they hold non-owning type pointers), then the world (which owns
	// and frees the registered types via STLDeleteAll(mlstEntityTypes)).
	for(size_t i=0; i<mvEditModes.size(); ++i)
		hplDelete(mvEditModes[i]);
	mvEditModes.clear();
	mpSelectMode = NULL;

	hplDelete(mpWorld);
}

//-------------------------------------------------------------------------------

iEditorWorld* cLevelEditorEntFileSession::GetWorld()
{
	return mpWorld;
}

//-------------------------------------------------------------------------------

void cLevelEditorEntFileSession::SetMountMatrix(const cMatrixf& amtxMount)
{
	mmtxMount = amtxMount;
}

//-------------------------------------------------------------------------------

void cLevelEditorEntFileSession::MountSubEntities()
{
	// Overlay each top-level sub-entity onto the placed model: world = mount x local.
	// Direct setters (SetWorldMatrix), so this does NOT bump the modification count
	// — mounting is not a user edit.
	tEntityWrapperList lstSubs;
	GetSubEntities(lstSubs);
	for(tEntityWrapperListIt it=lstSubs.begin(); it!=lstSubs.end(); ++it)
	{
		iEntityWrapper* pEnt = *it;
		pEnt->SetWorldMatrix(cMath::MatrixMul(mmtxMount, pEnt->GetWorldMatrix()));
		// SetWorldMatrix only updates the wrapper's mvPosition/Rot/Scale; push the mounted
		// transform down to the engine object (cLight/cParticleSystem/cBillboard) and cascade
		// to aggregate (Body) shapes, else those draw at their un-mounted local matrix (0,0,0).
		// The headless session world is never pumped per-frame, so this is the only sync.
		pEnt->UpdateEntity();
	}
}

//-------------------------------------------------------------------------------

bool cLevelEditorEntFileSession::Load()
{
	// Clean slate so a re-Load re-syncs from the current definition.
	mpWorld->Reset();

	// Pin the prefab resource: keeps it (and its shared in-memory doc) alive for the
	// whole session, so edits round-trip through the SAME doc the placed instances
	// bind to instead of a disconnected snapshot. RAII-released in the destructor.
	cPrefabManager* pMgr = mpEditor->GetPrefabManager();
	mPrefabRef = pMgr->CreatePrefab(msFile);

	// Read the definition straight from the resource's owned doc (materialized from
	// disk on first scope-in). A pending edit — from a prior scope Save or MCP
	// update_prefab — wins, so re-scoping resumes the in-flight edit. The session
	// never touches the file API itself; all I/O routes through cPrefabManager.
	tinyxml2::XMLElement* pRoot = pMgr->EnsureDoc(msFile);

	bool bOk = false;
	if(pRoot)
		bOk = mpWorld->Load(pRoot);

	// Overlay onto the placed model, then treat the mounted state as the clean
	// baseline so the save-debounce only fires on real edits.
	MountSubEntities();
	mpWorld->UpdateSavedModifications();

	// Re-base the change poll to the loaded/mounted state so building the scope does
	// not look like an edit (mounting uses direct setters that don't bump the counter,
	// but be explicit — Load can be called again to re-sync).
	mlSyncModStamp = mpWorld->GetNumModifications();
	mfSyncDebounce = 0.0f;

	return bOk;
}

//-------------------------------------------------------------------------------

void cLevelEditorEntFileSession::Save(bool abBroadcast)
{
	// Un-mount for serialization (XML stores LOCAL coords), then restore the exact
	// pre-save world transforms. We snapshot pos/rot/scale VALUES and restore those
	// directly (not a re-decomposed matrix) so repeated saves don't drift the live
	// state; only the transient local matrix used for XML goes through a decompose.
	struct sSnap { iEntityWrapper* pEnt; cVector3f vPos, vRot, vScale; };
	std::vector<sSnap> vSnaps;

	tEntityWrapperList lstSubs;
	GetSubEntities(lstSubs);

	cMatrixf mtxInvMount = cMath::MatrixInverse(mmtxMount);
	for(tEntityWrapperListIt it=lstSubs.begin(); it!=lstSubs.end(); ++it)
	{
		iEntityWrapper* pEnt = *it;
		sSnap snap = { pEnt, pEnt->GetPosition(), pEnt->GetRotation(), pEnt->GetScale() };
		vSnaps.push_back(snap);

		// world -> local for the upcoming serialization.
		pEnt->SetWorldMatrix(cMath::MatrixMul(mtxInvMount, pEnt->GetWorldMatrix()));
		// Cascade the un-mount to the engine object + aggregate (Body) shapes, so a Body's
		// shapes serialize relative to the un-mounted parent (not the mounted one).
		pEnt->UpdateEntity();
	}

	// Serialize the world back to canonical .ent XML (iEditorWorld::Save renames the
	// root to the world's element name, "Entity") and hand ownership to the prefab
	// manager as this prefab's pending definition — it live-reloads placed instances
	// (the broadcast) and writes to disk on map save.
	tinyxml2::XMLDocument* pDoc = new tinyxml2::XMLDocument();
	tinyxml2::XMLElement* pRoot = pDoc->NewElement("Entity");
	pDoc->InsertEndChild(pRoot);
	mpWorld->Save(pRoot);

	// Preserve the standalone Model Editor's <EditorSession>/<ViewportConfig> block:
	// the world writer only emits <ModelData>/<UserDefinedVariables>, so clone the block
	// from the retained original doc (still alive until SetPendingDoc replaces it) into the
	// new root. Self-sustaining: after the first save the pending doc carries the block,
	// so subsequent saves re-clone it. As first child to match the original layout.
	tinyxml2::XMLElement* pOldRoot = mpEditor->GetPrefabManager()->EnsureDoc(msFile);
	if(pOldRoot)
	{
		tinyxml2::XMLElement* pOldSession = pOldRoot->FirstChildElement("EditorSession");
		if(pOldSession)
			pRoot->InsertFirstChild(pOldSession->DeepClone(pDoc));
	}

	mpEditor->GetPrefabManager()->SetPendingDoc(msFile, pDoc, abBroadcast);

	// Restore exact world state (re-mount) and mark clean.
	for(size_t i=0; i<vSnaps.size(); ++i)
	{
		vSnaps[i].pEnt->SetAbsPosition(vSnaps[i].vPos);
		vSnaps[i].pEnt->SetAbsRotation(vSnaps[i].vRot);
		vSnaps[i].pEnt->SetAbsScale(vSnaps[i].vScale);
		// Re-sync the engine object + shapes to the re-mounted transform, else they'd be
		// left un-mounted (0,0,0 render) after this save while still scoped.
		vSnaps[i].pEnt->UpdateEntity();
	}
	mpWorld->UpdateSavedModifications();
}

//-------------------------------------------------------------------------------

void cLevelEditorEntFileSession::Update(float afTimeStep)
{
	// Scoped property/geometry edits (through the session Select mode's panel and edit
	// modes) commit as undoable actions on the session world; a bump in its modification
	// counter means the .ent changed and every placed instance must be re-synced.
	// Debounce so a burst (e.g. a slider/gizmo drag) collapses into a single Save +
	// live-reload rather than tearing down/rebuilding all instances every frame.
	//
	// This runs from cLevelEditor::OnUpdate (always ticks while scoped), so scoped edits
	// are ALWAYS persisted to all instances — never contingent on a GUI window updating.
	// Return-to-World (TeardownScope) and map-save (OnPostSave) remain the flush safety
	// nets; map new/load intentionally discards (no Save).
	unsigned int lMods = mpWorld->GetNumModifications();
	if(lMods != mlSyncModStamp)
	{
		mlSyncModStamp = lMods;
		mfSyncDebounce = 0.25f;
	}
	else if(mfSyncDebounce > 0.0f)
	{
		mfSyncDebounce -= afTimeStep;
		if(mfSyncDebounce <= 0.0f)
		{
			mfSyncDebounce = 0.0f;
			Save(true);
		}
	}
}

//-------------------------------------------------------------------------------

bool cLevelEditorEntFileSession::IsDirty()
{
	return mpWorld->IsModified();
}

//-------------------------------------------------------------------------------

bool cLevelEditorEntFileSession::IsEditableCategory(int alTypeID)
{
	for(int i=0;i<glEntFileCategoryOrderNum;++i)
		if(gvEntFileCategoryOrder[i]==alTypeID)
			return true;
	return false;
}

const int* cLevelEditorEntFileSession::GetCategoryOrder(int& alCountOut)
{
	alCountOut = glEntFileCategoryOrderNum;
	return gvEntFileCategoryOrder;
}

//-------------------------------------------------------------------------------

void cLevelEditorEntFileSession::GetSubEntities(tEntityWrapperList& avOut)
{
	avOut.clear();

	tEntityWrapperMap& mapEntities = mpWorld->GetEntities();
	tEntityWrapperMapIt it = mapEntities.begin();
	for(; it != mapEntities.end(); ++it)
	{
		iEntityWrapper* pEnt = it->second;
		if(IsEditableCategory(pEnt->GetTypeID())==false)
			continue;
		// Shapes/components owned by an aggregate (a Body) nest under it, not here.
		if(pEnt->BelongsToCompoundObject())
			continue;
		avOut.push_back(pEnt);
	}
}

//-------------------------------------------------------------------------------
