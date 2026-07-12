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

#ifndef HPL_RENDERABLE_CONTAINER_H
#define HPL_RENDERABLE_CONTAINER_H

#include "math/MathTypes.h"
#include "graphics/GraphicsTypes.h"
#include "system/SystemTypes.h"
#include "scene/SceneTypes.h"
#include <span>

namespace hpl {

	//-------------------------------------------

	class iRenderable;

	//-------------------------------------------

	// NOTE: this box-tree container survives ONLY as a load-time helper —
	// cWorldLoaderHplMap builds a temporary cRenderableContainer_BoxTree to
	// spatially group static meshes for batch merging. Runtime culling uses
	// the flat SIMD-scanned cRenderableSet arrays (scene/RenderableSet.h).

	class iRenderableContainerNode
	{
	friend class iRenderableContainer;
	public:
		iRenderableContainerNode();
		virtual ~iRenderableContainerNode(){}

		inline tRenderableContainerNodeList* GetChildNodeList(){ return &mlstChildNodes; }
		inline std::span<iRenderableContainerNode*> GetChildNodes(){ return mlstChildNodes; }
		inline bool HasChildNodes(){ return mlstChildNodes.empty() == false; }

		inline tRenderableList* GetObjectList() { return &mlstObjects; }
		inline bool HasObjects() { return mlstObjects.empty() == false; }
		inline std::span<iRenderable*> GetObjects() { return mlstObjects; }

		inline iRenderableContainerNode* GetParent(){ return mpParent;}

		inline int GetObjectNum(){ return (int)mlstObjects.size();}

		inline const cVector3f& GetMin() const{ return mvMin;}
		inline const cVector3f& GetMax() const{ return mvMax;}

		inline const cVector3f GetCenter() const{ return mvCenter;}
		inline float GetRadius() const { return mfRadius;}

	protected:
		cVector3f mvMin;
		cVector3f mvMax;
		float mfRadius;
		cVector3f mvCenter;

		iRenderableContainerNode *mpParent;
		tRenderableContainerNodeList mlstChildNodes;
		tRenderableList mlstObjects;
	};

	//-------------------------------------------

	class iRenderableContainer
	{
	public:
		virtual ~iRenderableContainer(){}

		virtual void Add(iRenderable *apRenderable)=0;
		virtual void Remove(iRenderable *apRenderable)=0;

		virtual iRenderableContainerNode* GetRoot()=0;

        /**
         * This compiles the container. Even if the container is static, it should be possible to change orientation (scale, pos, rotation,radius etc) of added
		 * objects before this method is called. After compile is called, objects orientation can not be changed!
         */
        virtual void Compile()=0;
	};

	//-------------------------------------------
};
#endif // RENDERABLE_CONTAINER
