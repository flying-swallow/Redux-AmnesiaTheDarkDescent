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

#ifndef HPLEDITOR_PREFAB_MANAGER_H
#define HPLEDITOR_PREFAB_MANAGER_H

#include "../common/StdAfx.h"

#include "resources/ResourceBase.h"
#include "resources/ResourceManager.h"
#include "system/Event.h"

using namespace hpl;

#include <map>
#include <vector>

//----------------------------------------------------------------------

namespace tinyxml2 {
class XMLElement;
class XMLDocument;
} // namespace tinyxml2

class iEditorBase;

//----------------------------------------------------------------------

// Prefab lifecycle carried by cPrefabResource::OnModified() (see that Event).
enum ePrefabEvent {
  ePrefabEvent_Added,    // a pending in-memory definition appeared
  ePrefabEvent_Modified, // the pending definition was replaced/edited
  ePrefabEvent_Removed,  // the pending definition was dropped (disk is truth
                         // again)
};

//----------------------------------------------------------------------

//////////////////////////////////////////////////////////////////////////
// cPrefabResource
//	One .ent "prefab" as a refcounted engine resource. Identity (both the
//	resource name AND the keying "full path") is the LOWERCASE BARE FILENAME
//— 	the identity the engine's file resolution uses — so a pathless, never-
//	written pending definition is a valid resource. The best-known on-disk
//	path lives separately in msDiskPath (may be empty until first flush).
//
//	Lifetime is reference-driven: every placed cEntityWrapperEntity holds a
//	SharedResourceHandle to its prefab, and the manager pins any resource
//	holding a pending doc. The reference count therefore IS the instance
//	count (+1 while pending); the resource frees itself when the last
//	instance is deleted and no pending doc remains.
//
class cPrefabResource : public iResourceBase {
public:
  explicit cPrefabResource(const tString &asKey)
      : iResourceBase(asKey, cString::To16Char(asKey), 0), mpDoc(NULL),
        mbDirty(false) {}
  ~cPrefabResource();

  // Resource contract: prefabs carry no engine-side payload to reload/unload —
  // instances re-read the doc (or disk) through the pending-first load path.
  bool Reload() { return false; }
  void Unload() {}
  void Destroy() {}

  // Per-resource modification signal. Each placed cEntityWrapperEntity subscribes
  // to ITS prefab (via mPrefabRef->OnModified()) and rebuilds itself when this
  // definition changes — Added/Modified/Removed all mean "re-read me". RAII: when
  // the resource is freed, ~Event auto-disconnects every still-connected instance
  // handler, so a subscriber can never dangle (the trap the old per-record design
  // hit — now made safe by Event/Handler two-way auto-disconnect).
  Event<ePrefabEvent> &OnModified() { return mOnModified; }

  tString msDiskPath; // best-known full path (for disk flushes)
  tinyxml2::XMLDocument
      *mpDoc;   // owned in-memory definition (NULL = on-disk only)
  bool mbDirty; // edited via MCP, awaiting a disk flush

private:
  Event<ePrefabEvent> mOnModified;
};

//----------------------------------------------------------------------

// One row of the MCP 'list_prefabs' query.
class cPrefabInfo {
public:
  cPrefabInfo() : mlInstances(0), mbPending(false), mbDirty(false) {}

  tString msEntFile;
  int mlInstances; // placed instances holding a reference to this prefab
  bool mbPending;  // has an in-memory definition (pending or edited)
  bool mbDirty;    // edited, not yet written to disk
};

//----------------------------------------------------------------------

//////////////////////////////////////////////////////////////////////////
// cPrefabManager
//	Editor-only iResourceManager for the .ent "prefabs" referenced in the
//	current map. Owned by iEditorBase (created in Init before the world,
//	destroyed after it, so wrapper handles never outlive the manager). The
//	single home for the in-memory (MCP-editable) .ent definitions and the
//	query surface the MCP 'list_prefabs'/'get_prefab'/'update_prefab' tools
//	drive.
//
//	Propagation model: PER-RESOURCE. Each cPrefabResource owns an OnModified()
//	Event; the mutation methods (SetPendingDoc / OnExternalFileChange) Signal the
//	specific resource. Every placed cEntityWrapperEntity subscribes to ITS prefab
//	(it already holds the resource handle) and rebuilds itself — Added/Modified/
//	Removed all "re-read me" through the pending-first entity load path (pending
//	doc if present, else disk). Event/Handler auto-disconnect both ways, so an
//	instance handler can never orphan or dangle across a resource free.
//
class cPrefabManager : public iResourceManager {
public:
  explicit cPrefabManager(iEditorBase *apEditor);
  ~cPrefabManager();

  // iResourceManager contract; prefabs hold no unloadable payload.
  void Unload(iResourceBase *apResource) {}

  ////////////////////////////////////////////////////////
  // Instance references. Get-or-create by bare-filename key; the returned
  // handle is what entity wrappers hold. Creating does NOT read the disk —
  // the resource is the identity plus an optional pending doc; disk loads
  // keep flowing through the fresh LoadXmlDocument path so on-disk truth is
  // never stale-cached here.
  SharedResourceHandle<cPrefabResource> CreatePrefab(const tString &asEntFile);

  ////////////////////////////////////////////////////////
  // In-memory definition (the pending-first entity load path consults this)
  tinyxml2::XMLElement *GetPendingRoot(const tString &asEntFile);

  // Ensure the prefab resource carries an in-memory definition doc, materializing
  // it from disk on first use, and return its <Entity> root (NULL if the file is
  // missing / unparseable). An existing pending doc (edited via the scope session
  // or MCP) wins and its dirty state is preserved; a freshly materialized doc is
  // CLEAN (mbDirty stays false) — bringing disk into memory is not an edit, so it
  // must not flush back on map save. The resource owns the doc, so a caller that
  // pins the resource (CreatePrefab) can read it for the pin's lifetime without
  // any ownership juggling.
  tinyxml2::XMLElement *EnsureDoc(const tString &asEntFile);
  // Takes ownership of apDoc (root must be <Entity>); replaces + re-dirties an
  // existing definition for the same prefab. While a pending doc exists the
  // manager holds its own pin reference so an instance-less definition (e.g.
  // define_entity_file before placement) cannot be freed. Broadcasts Added/
  // Modified unless abBroadcast is false (callers that run their own reload).
  void SetPendingDoc(const tString &asEntFile, tinyxml2::XMLDocument *apDoc,
                     bool abBroadcast = true);

  // The file changed on disk OUTSIDE the manager (ModelEditor save, hand edit,
  // reload_entity_file). If a NON-dirty pending doc shadows it, drop the doc
  // (+ its pin) so the disk becomes truth again (broadcasts Removed unless
  // abBroadcast=false — the file watcher / reload_entity_file already trigger
  // their own reload). A DIRTY pending doc is kept (unsaved MCP edit wins)
  // with a conflict Warning. Instance references keep the resource itself
  // alive either way — only the shadowing doc goes.
  void OnExternalFileChange(const tString &asEntFile, bool abBroadcast = true);

  ////////////////////////////////////////////////////////
  // Persistence
  // Write every dirty definition to its path (creating folders + re-indexing);
  // clears the dirty flag on success, leaves failures dirty. Returns count.
  // No broadcast: content is unchanged, only persisted.
  int Flush();
  // Write just asEntFile if dirty (backs the MCP 'update_prefab' persist:true).
  bool FlushOne(const tString &asEntFile);
  int DirtyCount();

  // Drop all pending docs + their pins (map new/load/reset — unsaved edits are
  // lost). Instance-pinned resources stay as empty shells and free themselves
  // as the world teardown drops the wrapper handles. Deliberately does NOT
  // broadcast: this only runs while the world itself is being torn down or
  // replaced; firing reloads mid-teardown would be harmful.
  void DiscardAllPending();

  ////////////////////////////////////////////////////////
  // Query surface for the MCP 'list_prefabs' tool: one row per live prefab
  // resource. Instance counts come straight from the reference count (minus
  // the manager's own pending pin) — no world scan.
  void ListPrefabs(std::vector<cPrefabInfo> &avOut);

private:
  cPrefabResource *GetPrefab(const tString &asEntFile);
  bool WritePrefabToDisk(cPrefabResource &aRes);

  iEditorBase *mpEditor; // back-ref for resource-dir re-indexing at flush

  // One pin per resource currently holding a pending doc, keyed like the
  // resources (lowercase bare filename). Held so a definition with zero
  // placed instances survives transient handles dropping.
  std::map<tString, SharedResourceHandle<cPrefabResource>> mmapPendingPins;
};

//----------------------------------------------------------------------

#endif // HPLEDITOR_PREFAB_MANAGER_H
