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

#ifndef HPL_SUB_MESH_H
#define HPL_SUB_MESH_H

#include "graphics/RITypes.h"
#include "math/MathTypes.h"
#include "graphics/GraphicsTypes.h"
#include "system/SystemTypes.h"
#include "math/MeshTypes.h"
#include "physics/PhysicsTypes.h"
#include <memory>

#define ML_NAMESPACE 
#include <ml.h>

namespace hpl {

	class cMaterial;
	class cVertexBuffer;

	class cMesh;
	class iPhysicsWorld;
	class iCollideShape;

	class cMaterialManager;

	//--------------------------------------------------

	class cMeshCollider
	{
	public:
		tString msGroup; // Only used as temp var when loading!

		eCollideShapeType mType;
		cVector3f mvSize;
		cMatrixf m_mtxOffset;
		bool mbCharCollider;
	};

	//--------------------------------------------------

	class cSubMesh final
	{
	friend class cMesh;
	friend class cSubMeshEntity;
	public:

		cSubMesh(const tString &asName,cMaterialManager* apMaterialManager);
		~cSubMesh();

		void SetMaterial(cMaterial* apMaterial);
		void SetVertexBuffer(cVertexBuffer* apVtxBuffer);

		//Renderable implementation.
		cMaterial *GetMaterial();
		cVertexBuffer* GetVertexBuffer();

		const tString& GetName(){ return msName;}

		//Vertex-Bone pairs
		void ResizeVertexBonePairs(int alSize);
		int GetVertexBonePairNum();
		cVertexBonePair& GetVertexBonePair(int alNum);

		void AddVertexBonePair(const cVertexBonePair &aPair);
		void ClearVertexBonePairs();

		//Colliders
		cMeshCollider* CreateCollider(eCollideShapeType aType);
		cMeshCollider* GetCollider(int alIdx);
		int GetColliderNum();
		iCollideShape* CreateCollideShape(iPhysicsWorld *apWorld);
		static iCollideShape* CreateCollideShapeFromCollider(cMeshCollider *pCollider, iPhysicsWorld *apWorld, const cVector3f& avSizeMul, cMatrixf *apMtxOffset);

		void SetIsCollideShape(bool abX){mbCollideShape = abX;}
		bool IsCollideShape(){ return mbCollideShape;}

		const cTriEdge& GetEdge(int alIndex) const{ return mvEdges[alIndex];}
		int GetEdgeNum(){ return (int)mvEdges.size();}

		tTriEdgeVec* GetEdgeVecPtr(){ return &mvEdges;}

		tTriangleDataVec* GetTriangleVecPtr(){ return &mvTriangles;}

		void SetDoubleSided(bool abX){ mbDoubleSided = abX;}
		bool GetDoubleSided(){ return mbDoubleSided;}

		void SetModelScale(const cVector3f& avScale){ mvModelScale = avScale;}
		cVector3f GetModelScale(){ return mvModelScale;}

		const cMatrixf& GetLocalTransform(){ return m_mtxLocalTransform;}
		void SetLocalTransform(const cMatrixf& a_mtxTrans){ m_mtxLocalTransform = a_mtxTrans;}

		bool GetIsOneSided(){ return mbIsOneSided;}
		const cVector3f& GetOneSidedNormal(){ return mvOneSidedNormal;}
		const cVector3f& GetOneSidedPoint(){ return mvOneSidedPoint;}

		void SetMaterialName(const tString& asName){msMaterialName =asName;}
		const tString& GetMaterialName(){ return msMaterialName;}
		
		void Compile();
	private:
		void CheckOneSided();
		void CompileBonePairs();

		tString msName;

		tString msMaterialName;
		cMaterial* mpMaterial = nullptr;
		cVertexBuffer* mpVtxBuffer = nullptr;

		cMatrixf m_mtxLocalTransform = cMatrixf::Identity;

		tVertexBonePairVec mvVtxBonePairs;

		// Colliders owned by value (RAII); CreateCollider appends and returns an
		// element observer used transiently by loaders.
		std::vector<cMeshCollider> mvColliders;

		// Per-vertex skinning weights/bone indices (4 per vertex), built lazily
		// in CompileBonePairs. Vectors own the storage — no manual free.
		std::vector<float> mvVertexWeights;
		std::vector<unsigned char> mvVertexBones;

		tTriEdgeVec mvEdges;
		tTriangleDataVec mvTriangles;

		cVector3f mvModelScale;

		bool mbDoubleSided = false;

		bool mbCollideShape = false;

		bool mbIsOneSided = false;
		cVector3f mvOneSidedNormal = 0;
		cVector3f mvOneSidedPoint = 0;

		cMaterialManager* mpMaterialManager = nullptr;

		cMesh* mpParent = nullptr;
	};

};
#endif // HPL_SUB_MESH_H
