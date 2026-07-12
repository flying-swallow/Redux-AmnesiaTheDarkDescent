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

#include "impl/MeshLoaderGLTF.h"

#include "system/LowLevelSystem.h"
#include "system/String.h"
#include "system/System.h"
#include "system/Platform.h"

#include "graphics/Mesh.h"
#include "graphics/SubMesh.h"
#include "graphics/Material.h"
#include "graphics/VertexBuffer.h"

#include "resources/MaterialManager.h"
#include "resources/MeshManager.h"
#include "resources/Resources.h"

#include "impl/MeshLoaderMSH.h"

#include "math/Math.h"
#include "math/BoundingVolume.h"

#include <cstring>
#include <vector>

// Exactly one translation unit in the whole project defines the cgltf
// implementation; this is it. cgltf is pure C99 and does not throw, so it is
// safe to compile into HPL2 core (which builds with -fno-exceptions).
#define CGLTF_IMPLEMENTATION
#include "cgltf.h"

namespace hpl {

	//////////////////////////////////////////////////////////////////////////
	// CONSTRUCTORS
	//////////////////////////////////////////////////////////////////////////

	//-----------------------------------------------------------------------

	cMeshLoaderGLTF::cMeshLoaderGLTF(cMeshLoaderMSH *apMeshLoaderMSH, bool abLoadAndSaveMSHFormat)
	{
		mpMeshLoaderMSH = apMeshLoaderMSH;
		mbLoadAndSaveMSHFormat = abLoadAndSaveMSHFormat;

		AddSupportedExtension("gltf");
		AddSupportedExtension("glb");
	}

	//-----------------------------------------------------------------------

	cMeshLoaderGLTF::~cMeshLoaderGLTF()
	{
	}

	//-----------------------------------------------------------------------

	//////////////////////////////////////////////////////////////////////////
	// FILE-LOCAL HELPERS
	//////////////////////////////////////////////////////////////////////////

	//-----------------------------------------------------------------------

	// Find the accessor for a given attribute (set 0) on a primitive.
	static const cgltf_accessor* GetAttributeAccessor(const cgltf_primitive* apPrim,
													cgltf_attribute_type aType)
	{
		for(cgltf_size i=0; i<apPrim->attributes_count; ++i)
		{
			if(apPrim->attributes[i].type == aType && apPrim->attributes[i].index == 0)
				return apPrim->attributes[i].data;
		}
		return NULL;
	}

	//-----------------------------------------------------------------------

	// Map a glTF material to an HPL material *name* using the same convention as
	// the COLLADA loader: the base-colour texture image basename (falling back to
	// the material name) is what a sibling .mat is expected to be named after.
	static tString GetMaterialBaseName(const cgltf_material* apMat)
	{
		if(apMat == NULL) return "";

		if(apMat->has_pbr_metallic_roughness &&
			apMat->pbr_metallic_roughness.base_color_texture.texture &&
			apMat->pbr_metallic_roughness.base_color_texture.texture->image &&
			apMat->pbr_metallic_roughness.base_color_texture.texture->image->uri)
		{
			return cString::GetFileName(apMat->pbr_metallic_roughness.base_color_texture.texture->image->uri);
		}

		if(apMat->name && apMat->name[0] != '\0')
			return apMat->name;

		return "";
	}

	//-----------------------------------------------------------------------

	// glTF stores node/local matrices column-major (OpenGL convention). HPL's
	// cMatrixf is row-major with a column-vector convention (translation in the
	// last column), so FromTranspose() is exactly the right conversion.
	static cMatrixf GetNodeWorldMatrix(const cgltf_node* apNode)
	{
		cgltf_float m[16];
		cgltf_node_transform_world(apNode, m);
		cMatrixf mtx;
		mtx.FromTranspose(m);
		return mtx;
	}

	//-----------------------------------------------------------------------

	//////////////////////////////////////////////////////////////////////////
	// PUBLIC METHODS
	//////////////////////////////////////////////////////////////////////////

	//-----------------------------------------------------------------------

	cMesh* cMeshLoaderGLTF::LoadMesh(const tWString& asFile, tMeshLoadFlag aFlags)
	{
		/////////////////////////////////////////////////
		// TRY USING MSH LOADER (transparent .msh cache, mirrors cMeshLoaderCollada)
		if(mbLoadAndSaveMSHFormat)
		{
			tWString sMSHFile = cString::SetFileExtW(asFile, _W("msh"));
			cDate currentDate = cPlatform::FileModifiedDate(asFile);
			cDate mshDate = cPlatform::FileModifiedDate(sMSHFile);

			if(cResources::GetForceCacheLoadingAndSkipSaving() || mshDate > currentDate || cPlatform::FileExists(asFile)==false)
			{
				cMesh *pMesh = mpMeshLoaderMSH->LoadMesh(sMSHFile, aFlags);
				if(pMesh)
				{
					pMesh->SetFullPath(asFile); //Use gltf as full path so it is not loaded twice.
					return pMesh;
				}
			}
		}

		tString sFile = cString::To8Char(asFile);

		/////////////////////////////////////////////////
		// PARSE
		cgltf_options options;
		memset(&options, 0, sizeof(options));

		cgltf_data* pData = NULL;
		cgltf_result result = cgltf_parse_file(&options, sFile.c_str(), &pData);
		if(result != cgltf_result_success)
		{
			Error("glTF: failed to parse '%s' (error %d)\n", sFile.c_str(), (int)result);
			return NULL;
		}

		result = cgltf_load_buffers(&options, pData, sFile.c_str());
		if(result != cgltf_result_success)
		{
			Error("glTF: failed to load buffers for '%s' (error %d)\n", sFile.c_str(), (int)result);
			cgltf_free(pData);
			return NULL;
		}

		if(cgltf_validate(pData) != cgltf_result_success)
			Warning("glTF: validation reported issues for '%s' (continuing)\n", sFile.c_str());

		/////////////////////////////////////////////////
		// CREATE MESH
		cMesh *pMesh = hplNew( cMesh, (sFile, asFile, mpMaterialManager, mpAnimationManager) );

		// Path of the model relative to the working dir, prepended to material
		// names so the .mat is resolved next to the model (same as COLLADA).
		tWString sRelativePathW = cString::GetRelativePathW(cString::GetFilePathW(asFile), cPlatform::GetWorkingDir());
		tString sModelPath = cString::To8Char(sRelativePathW);

		std::vector<cMeshCollider> vMeshColliders;

		/////////////////////////////////////////////////
		// ITERATE NODES
		// Walk the flat node list; cgltf_node_transform_world resolves the full
		// parent chain so hierarchy is handled without manual recursion. The node
		// world transform is baked into the vertices (single-object exports are
		// identity; multi-object files assemble correctly).
		for(cgltf_size n=0; n<pData->nodes_count; ++n)
		{
			cgltf_node* pNode = &pData->nodes[n];
			if(pNode->mesh == NULL) continue;

			tString sNodeName = (pNode->name && pNode->name[0] != '\0') ? tString(pNode->name) : tString("");

			cMatrixf mtxWorld = GetNodeWorldMatrix(pNode);
			cMatrixf mtxNormal = cMath::MatrixInverse(mtxWorld.GetRotation()).GetTranspose();

			///////////////////////////////////////////////////////
			// SPECIAL NODES (name starts with '_'): colliders / collide-shape meshes
			bool bCollideMeshShape = false;
			if(sNodeName.length()>0 && sNodeName[0]=='_')
			{
				tStringVec vStrings;
				tString sSepp = "_";
				cString::GetStringVec(sNodeName, vStrings, &sSepp);

				tString sSpecialName = vStrings.empty() ? "" : cString::ToLowerCase(vStrings[0]);
				tString sTypeName    = vStrings.size()<=1 ? "" : cString::ToLowerCase(vStrings[1]);

				bCollideMeshShape = true; // any '_' object is at least a collide shape

				///////////////////////////////////
				// PRIMITIVE COLLIDER (box/sphere/capsule/cylinder)
				if((sSpecialName=="collider" || sSpecialName=="charcollider") &&
					vStrings.size()>1 && sTypeName!="mesh")
				{
					// Build a bounding volume from the collider primitive's
					// positions in baked (world) space.
					const cgltf_primitive* pPrim = &pNode->mesh->primitives[0];
					const cgltf_accessor* pPos = GetAttributeAccessor(pPrim, cgltf_attribute_type_position);
					if(pPos == NULL)
					{
						Warning("glTF: collider node '%s' has no POSITION; skipping.\n", sNodeName.c_str());
						continue;
					}

					std::vector<float> vWorldPos(pPos->count*3);
					for(cgltf_size v=0; v<pPos->count; ++v)
					{
						float p[3] = {0,0,0};
						cgltf_accessor_read_float(pPos, v, p, 3);
						cVector3f wp = cMath::MatrixMul(mtxWorld, cVector3f(p[0],p[1],p[2]));
						vWorldPos[v*3+0]=wp.x; vWorldPos[v*3+1]=wp.y; vWorldPos[v*3+2]=wp.z;
					}

					cBoundingVolume bv;
					bv.AddArrayPoints(&vWorldPos[0], (int)pPos->count);
					bv.CreateFromPoints(3);

					eCollideShapeType shapeType = eCollideShapeType_Box;
					cVector3f vShapeSize = bv.GetSize();
					if(sTypeName=="box"){ shapeType = eCollideShapeType_Box; }
					else if(sTypeName=="sphere"){ shapeType = eCollideShapeType_Sphere; vShapeSize *= cVector3f(0.5f); }
					else if(sTypeName=="capsule"){ shapeType = eCollideShapeType_Capsule; vShapeSize.x *= 0.5f; }
					else if(sTypeName=="cylinder"){ shapeType = eCollideShapeType_Cylinder; vShapeSize.x *= 0.5f; }

					cMeshCollider meshCollider;
					meshCollider.mType = shapeType;
					meshCollider.mvSize = vShapeSize;
					meshCollider.mbCharCollider = (sSpecialName=="charcollider");
					meshCollider.msGroup = (pNode->parent && pNode->parent->name) ? tString(pNode->parent->name) : tString("");

					// Offset is the collider centre (in baked/mesh-local space).
					meshCollider.m_mtxOffset = cMatrixf::Identity;
					meshCollider.m_mtxOffset.SetTranslation(bv.GetWorldCenter());

					// Orient cylinder/capsule along Y instead of X (matches COLLADA).
					if(shapeType==eCollideShapeType_Cylinder || shapeType==eCollideShapeType_Capsule)
					{
						meshCollider.m_mtxOffset = cMath::MatrixMul(meshCollider.m_mtxOffset,
															cMath::MatrixRotateZ(cMath::ToRad(90)));
					}

					vMeshColliders.push_back(meshCollider);
					continue; // do not create a renderable submesh for the collider
				}
			}

			///////////////////////////////////////////////////////
			// RENDERABLE SUBMESHES (one per primitive)
			cgltf_mesh* pGltfMesh = pNode->mesh;
			for(cgltf_size prim=0; prim<pGltfMesh->primitives_count; ++prim)
			{
				const cgltf_primitive* pPrim = &pGltfMesh->primitives[prim];
				if(pPrim->type != cgltf_primitive_type_triangles)
				{
					Warning("glTF: node '%s' primitive %d is not a triangle list; skipping.\n", sNodeName.c_str(), (int)prim);
					continue;
				}

				const cgltf_accessor* pPos = GetAttributeAccessor(pPrim, cgltf_attribute_type_position);
				if(pPos == NULL)
				{
					Warning("glTF: node '%s' primitive %d has no POSITION; skipping.\n", sNodeName.c_str(), (int)prim);
					continue;
				}
				const cgltf_accessor* pNrm = GetAttributeAccessor(pPrim, cgltf_attribute_type_normal);
				const cgltf_accessor* pUV  = GetAttributeAccessor(pPrim, cgltf_attribute_type_texcoord);

				// Sub mesh name: node name (+ primitive index when >1 primitive).
				tString sSubName = sNodeName;
				if(sSubName == "")
					sSubName = "gltf_node" + cString::ToString((int)n);
				if(pGltfMesh->primitives_count > 1)
					sSubName += "_" + cString::ToString((int)prim);

				cSubMesh* pSubMesh = pMesh->CreateSubMesh(sSubName);
				pSubMesh->SetIsCollideShape(bCollideMeshShape);
				pSubMesh->SetModelScale(cVector3f(1,1,1));

				//////////////////////////////
				// VERTEX BUFFER — standard HPL layout the whole engine expects.
				// Tangents are intentionally NOT read from glTF; Compile() derives
				// them from position+normal+UV0 (matches MSH / COLLADA main path).
				const cgltf_size lVtxNum = pPos->count;
				const cgltf_size lIdxNum = pPrim->indices ? pPrim->indices->count : pPos->count;

				cVertexBuffer* pVtxBuffer = new cVertexBuffer(
					eVertexBufferType_Hardware,
					eVertexBufferDrawType_Tri, eVertexBufferUsageType_Static,
					(int)lVtxNum, (int)lIdxNum);

				pVtxBuffer->CreateElementArray(eVertexBufferElement_Position, eVertexBufferElementFormat_Float, 4);
				pVtxBuffer->CreateElementArray(eVertexBufferElement_Normal,   eVertexBufferElementFormat_Float, 3);
				pVtxBuffer->CreateElementArray(eVertexBufferElement_Texture0, eVertexBufferElementFormat_Float, 3);
				pVtxBuffer->CreateElementArray(eVertexBufferElement_Color0,   eVertexBufferElementFormat_Float, 4);

				for(cgltf_size v=0; v<lVtxNum; ++v)
				{
					float p[3] = {0,0,0};
					cgltf_accessor_read_float(pPos, v, p, 3);
					cVector3f vPos = cMath::MatrixMul(mtxWorld, cVector3f(p[0],p[1],p[2]));
					pVtxBuffer->AddVertexVec3f(eVertexBufferElement_Position, vPos);

					cVector3f vNrm(0,1,0);
					if(pNrm)
					{
						float nn[3] = {0,1,0};
						cgltf_accessor_read_float(pNrm, v, nn, 3);
						vNrm = cMath::MatrixMul3x3(mtxNormal, cVector3f(nn[0],nn[1],nn[2]));
						vNrm.Normalize();
					}
					pVtxBuffer->AddVertexVec3f(eVertexBufferElement_Normal, vNrm);

					cVector3f vTex(0,0,0);
					if(pUV)
					{
						float uv[2] = {0,0};
						cgltf_accessor_read_float(pUV, v, uv, 2);
						vTex.x = uv[0];
						vTex.y = uv[1];
					}
					pVtxBuffer->AddVertexVec3f(eVertexBufferElement_Texture0, vTex);

					pVtxBuffer->AddVertexColor(eVertexBufferElement_Color0, cColor(1,1));
				}

				//////////////////////////////
				// INDICES — flip winding per triangle (glTF is CCW-front, engine is CW-front).
				for(cgltf_size j=0; j<lIdxNum; ++j)
				{
					cgltf_size srcTri = (j/3)*3 + (2-(j%3));
					unsigned int idx = pPrim->indices
						? (unsigned int)cgltf_accessor_read_index(pPrim->indices, srcTri)
						: (unsigned int)srcTri;
					pVtxBuffer->AddIndex(idx);
				}

				// Derive tangents (creates + fills the Texture1Tangent stream).
				pVtxBuffer->Compile(eVertexCompileFlag_CreateTangents);
				pSubMesh->SetVertexBuffer(pVtxBuffer);

				//////////////////////////////
				// MATERIAL — resolve base-colour texture basename to a sibling .mat.
				tString sMatBase = GetMaterialBaseName(pPrim->material);
				if(sMatBase != "")
				{
					tString sMatName = cString::SetFilePath(sMatBase, sModelPath);
					pSubMesh->SetMaterialName(cString::SetFileExt(sMatName, "mat"));

					if((aFlags & eMeshLoadFlag_NoMaterial) == 0)
					{
						SharedResourceHandle<cMaterial> pMaterial = mpMaterialManager->CreateMaterial(cString::SetFileExt(sMatName, "mat"));
						if(!pMaterial)
							Error("glTF: couldn't create material '%s' for object '%s'\n", sMatName.c_str(), sSubName.c_str());
						pSubMesh->SetMaterial(std::move(pMaterial));
					}
				}
				else
				{
					pSubMesh->SetMaterialName("");
				}

				pSubMesh->Compile();
			}
		}

		/////////////////////////////////////////////////
		// COMPILE
		pMesh->CompileBonesAndSubMeshes();

		/////////////////////////////////////////////////
		// ATTACH COLLIDERS TO SUBMESHES (by group name, else submesh 0)
		if(vMeshColliders.empty()==false && pMesh->GetSubMeshNum()>0)
		{
			for(size_t i=0; i<vMeshColliders.size(); ++i)
			{
				cMeshCollider& meshCollider = vMeshColliders[i];
				cSubMesh* pSubMesh = NULL;
				if(meshCollider.msGroup != "")
				{
					pSubMesh = pMesh->GetSubMeshName(meshCollider.msGroup);
					if(pSubMesh==NULL)
					{
						Log("glTF: sub mesh '%s' for collider was not found; using submesh 0.\n", meshCollider.msGroup.c_str());
						pSubMesh = pMesh->GetSubMesh(0);
					}
				}
				else
				{
					pSubMesh = pMesh->GetSubMesh(0);
				}

				cMeshCollider* pCollider = pSubMesh->CreateCollider(meshCollider.mType);
				*pCollider = meshCollider;
			}
		}

		cgltf_free(pData);

		/////////////////////////////////////////////////
		// SAVE MSH FORMAT (cache)
		if(cResources::GetForceCacheLoadingAndSkipSaving()==false && mbLoadAndSaveMSHFormat)
		{
			tWString sMSHFile = cString::SetFileExtW(asFile, _W("msh"));
			mpMeshLoaderMSH->SaveMesh(pMesh, sMSHFile);
		}

		return pMesh;
	}

	//-----------------------------------------------------------------------
}
