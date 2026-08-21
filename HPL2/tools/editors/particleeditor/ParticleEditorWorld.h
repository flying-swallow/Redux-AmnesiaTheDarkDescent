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

#ifndef HPLEDITOR_PARTICLE_EDITOR_WORLD_H
#define HPLEDITOR_PARTICLE_EDITOR_WORLD_H

#include "../common/EditorWorld.h"

namespace tinyxml2 { class XMLElement; class XMLDocument; }

class cEntityWrapperParticleEmitter;

//---------------------------------------------------------------

class cParticleEditorWorld : public iEditorWorld
{
public:
	cParticleEditorWorld(iEditorBase* apEditor);
	~cParticleEditorWorld();

	tinyxml2::XMLElement* CreateWorldDataElement(tinyxml2::XMLElement* apXmlDoc) { return NULL; }
	tinyxml2::XMLElement* CreateWorldObjectsElement(tinyxml2::XMLElement* apWorldDataElement) { return NULL; }

	tinyxml2::XMLElement* GetWorldDataElement(tinyxml2::XMLElement* apXmlDoc) { return apXmlDoc; }
	void LoadWorldData(tinyxml2::XMLElement* apWorldDataElement) {}
	tinyxml2::XMLElement* GetWorldObjectsElement(tinyxml2::XMLElement* apWorldDataElement) { return NULL; }
	void LoadWorldObjects(tinyxml2::XMLElement* apWorldObjectsElement) {}

	bool Load(tinyxml2::XMLElement*);
	bool Save(tinyxml2::XMLElement*);

	void Reset();

	iEntityWrapper* AddEmitter(iEntityWrapperData* apData=NULL);
	void RemoveEmitter(iEntityWrapper* apEmitter);
	bool GetEmittersUpdated() { return mbEmittersUpdated; }
	void SetEmittersUpdated(bool abX) { mbEmittersUpdated = abX; }

	void UpdateParticleSystem();

	void SetShowWalls(bool abX);
	void SetShowFloor(bool abX);
	void SetBGColor(const cColor& aCol);

	bool GetShowFloor() { return mbShowFloor; }
	bool GetShowWalls() { return mbShowWalls; }
	cColor GetBGColor();
protected:
	bool CheckDataIsValid();

	void OnEditorUpdate(float afTimeStep);

	bool mbEmittersUpdated;
	bool mbPSDataUpdated;
	iPhysicsWorld* mpPhysicsWorld;
	tinyxml2::XMLDocument* mpTestData;
	cParticleSystem* mpTestPS;

	cColor mBGCol;

	bool mbShowFloor;
	bool mbShowWalls;

	
	std::vector<cMeshEntity*> mvWalls;
	std::vector<iPhysicsBody*> mvWallBodies;
};

//---------------------------------------------------------------

#endif // HPLEDITOR_PARTICLE_EDITOR_WORLD_H

