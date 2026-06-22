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

#ifndef HPL_WORLD_H
#define HPL_WORLD_H

#include "resources/ResourceBase.h"
#include "system/SystemTypes.h"
#include "graphics/GraphicsTypes.h"
#include "math/MathTypes.h"
#include "engine/EngineTypes.h"
#include "scene/SceneTypes.h"
#include "graphics/RITypes.h" // RISharedPointer<RIBuffer> decal buffer members

namespace tinyxml2 { class XMLElement; }

namespace hpl {

	class cGraphics;
	class cResources;
	class cDecal;
	class cMaterial;
	class cSound;
	class cPhysics;
	class cScene;
	class cSystem;
	class cAI;
	class cHaptic;

	class iCamera;
	class cCamera;
	class cNode3D;
	class iEntity3D;
	class iLight;
	class cLightSpot;
	class cLightPoint;
	class cLightBox;
	class cLightArea;
	class cImageEntity;
	class cParticleManager;
	class cParticleSystem;
	class iScript;
	class cPortalContainer;
	class iRenderableContainer;
	class cMeshEntity;
	class cMesh;
	class cBillboard;
	class cBeam;
	class cGuiSetEntity;
	class iPhysicsWorld;
	class iPhysicsBody;
	class cSoundEntity;
	class cAINodeContainer;
	class cAStarHandler;
	class cAINodeGeneratorParams;
	class cVertexBuffer;
	class Image;
	class cGuiSet;
	class cRopeEntity;
	class iPhysicsRope;
	class cResourceVarsObject;
	class cFogArea;
	class cEntFile;
	class cDummyRenderable;
	struct SceneConstants; // per-frame UBO (SceneTypes.slang); SubmitToGpu fills the *Count fields
	

	//-------------------------------------------------------------------
	
	typedef std::list<cEntFile*> tEntFileList;
	typedef tEntFileList::iterator tEntFileListIt;

	//-------------------------------------------------------------------
	
	class cTempAiNode
	{
	public:
		cTempAiNode(const cVector3f& avPos, const tString& asName, int alID) : mvPos(avPos),msName(asName), mlID(alID) {}
		cVector3f mvPos;
		tString msName;
		int mlID;
	};
	
	typedef std::list<cTempAiNode> tTempAiNodeList;
	typedef std::list<cTempAiNode>::iterator tTempAiNodeListIt;

	class cTempNodeContainer
	{
	public:
		tString msName;
        tTempAiNodeList mlstNodes;		
	};

	typedef std::map<tString,cTempNodeContainer*> tTempNodeContainerMap;
	typedef std::map<tString,cTempNodeContainer*>::iterator tTempNodeContainerMapIt;

	//-------------------------------------------------------------------

	class cAreaEntity :public iSerializable
	{
		kSerializableClassInit(cAreaEntity)
	public:
		tString msName;
		tString msType;
		cMatrixf m_mtxTransform;
		cVector3f mvSize;
	};
	
	typedef std::map<tString, cAreaEntity*> tAreaEntityMap;
	typedef tAreaEntityMap::iterator tAreaEntityMapIt;

	//-------------------------------------------------------------------

	class cStartPosEntity : public iSerializable
	{
		kSerializableClassInit(cStartPosEntity)
	public:
		cStartPosEntity() {}
		cStartPosEntity(const tString& asName) : msName(asName){}

		cMatrixf& GetWorldMatrix(){ return m_mtxTransform;}
		cMatrixf& GetLocalMatrix(){ return m_mtxTransform;}
		void SetMatrix(const cMatrixf& a_mtxTrans){ m_mtxTransform = a_mtxTrans;}
		
		tString& GetName(){ return msName;}

		cMatrixf m_mtxTransform;
		tString msName;
	};
	
	typedef std::list<cStartPosEntity*> tStartPosEntityList;
	typedef std::list<cStartPosEntity*>::iterator tStartPosEntityListIt;

	//-------------------------------------------------------------------

	class cWorld
	{
	public:
		cWorld(tString asName,cGraphics *apGraphics,cResources *apResources,cSound* apSound,
					cPhysics *apPhysics, cScene *apScene,cSystem *apSystem, cAI *apAI,
					cHaptic *apHaptic);
		~cWorld();

		void DestroyAllEntities(tWorldDestroyAllFlag aFlags);

		tString GetName(){ return msName;}

		bool CreateFromFile(tString asFile);

		void SetFilePath(const tWString& asFile){ msFilePath = asFile;}
		const tWString& GetFilePath(){ return msFilePath;}

		void SetActive(bool abX) {mbActive = abX;}
		inline bool IsActive()  const { return mbActive;}

		void Update(float afTimeStep);

		void PreUpdate(float afTotalTime, float afTimeStep);

		cVector3f GetWorldSize(){ return mvWorldSize;}

		void SetIsSoundEmitter(bool abX){ mbIsSoundEmitter = abX;}
		bool IsSoundEmitter(){ return mbIsSoundEmitter;}

		iRenderableContainer* GetRenderableContainer(eWorldContainerType aType);
		
		cPhysics* GetPhysics(){ return mpPhysics;}
		cResources* GetResources(){ return mpResources;}
		cSound* GetSound(){ return mpSound;}
		cSystem* GetSystem(){ return mpSystem;}
		cHaptic* GetHaptic(){ return mpHaptic;}

		iEntity3D* CreateEntity(const tString& asName, const cMatrixf &a_mtxTransform, 
								const tString& asFile, int alID = -1, bool abActive=true,
								const cVector3f &avScale=cVector3f(1),
								cResourceVarsObject *apInstanceVars=NULL,
								bool abSkipNonStaticEntity=false);
								
		/**
		 * Call this when all things have been added to set up things like physics world size.
		 **/
		void Compile(bool abCalcPhysicsWorldSize);


		///// PHYSICS ////////////////////////////////

		void SetPhysicsWorld(iPhysicsWorld *apWorld, bool abAutoDelete=true);
		iPhysicsWorld* GetPhysicsWorld();

		///// SKYBOX ////////////////////////////////

		void SetSkyBox(Image *apImage, bool abAutoDestroy);
		void SetSkyBoxActive(bool abX);
		void SetSkyBoxColor(const cColor& aColor);

		Image* GetSkyBoxImage() const { return mpSkyBoxImage; }
		cVertexBuffer *GetSkyBoxVertexBuffer(){ return mpSkyBoxVtxBuffer;}
		bool GetSkyBoxActive(){ return mbSkyBoxActive;}
		cColor GetSkyBoxColor(){ return mSkyBoxColor;}
        
		///// FOG ////////////////////////////////

		void SetFogActive(bool abX){ mbFogActive = abX;}
		void SetFogStart(float afX){ mfFogStart = afX;}
		void SetFogEnd(float afX){ mfFogEnd = afX;}
		void SetFogFalloffExp(float afX){ mfFogFalloffExp = afX;}
		void SetFogColor(const cColor& aCol){ mFogColor = aCol; }
		void SetFogCulling(bool abX) {mbFogCulling=abX;}

		bool GetFogActive(){ return mbFogActive;}
		float GetFogStart(){ return mfFogStart;}
		float GetFogEnd(){ return mfFogEnd;}
		float GetFogFalloffExp(){ return mfFogFalloffExp;}
		const cColor& GetFogColor(){ return mFogColor; }
		bool GetFogCulling() { return mbFogCulling;}

		///// AREA ////////////////////////////////
		
		cAreaEntity* CreateAreaEntity(const tString &asName);
		cAreaEntity* GetAreaEntity(const tString &asName);
		tAreaEntityMap* GetAreaEntityMap(){return &m_mapAreaEntities;}

		///// MESH ENTITY METHODS ////////////////////
		
		cMeshEntity* CreateMeshEntity(const tString &asName,cMesh *apMesh, bool abStatic=false);
		void DestroyMeshEntity(cMeshEntity* apMesh);

		///// DECAL METHODS //////////////////////////
		// Creates an oriented-box decal (no geometry). The world owns it and the
		// material; both are freed in DestroyAllEntities. Caller sets the box
		// transform via SetWorldMatrix/SetPosition.
		cDecal* CreateDecal(const tString& asName, const tString& asMaterial,
							const cColor& aColor, const cVector2l& avSubDiv);

		// All decals, in stable order (== gDecals[] upload order + the index space
		// of GetDecalObjectIndices()).
		const std::vector<cDecal*>& GetDecals() const { return mvDecals; }
		// Flat pool of stable decal indices; each renderable's [offset,count)
		// (iRenderable::GetDecalList*) addresses a run here. Built by Compile().
		const std::vector<uint32_t>& GetDecalObjectIndices() const { return mvDecalObjectIndices; }

		// Static per-world decal GPU buffers, baked once by Compile() and bound on
		// the per-world descriptor set (kWorldDecalSet). Null until baked (or when
		// the world has no decals / no hybrid renderer is active). The renderer
		// binds these on the composite pass; cWorld owns + disposes them.
		RIBuffer* GetDecalBuffer() const { return mpDecalBuffer.Get(); }
		RIBuffer* GetDecalObjectIndexBuffer() const { return mpDecalObjectIndexBuffer.Get(); }
		uint32_t GetDecalCount() const { return (uint32_t)mvDecals.size(); }

		// Single per-frame world→GPU submission, called once by the renderer before
		// any pass: bakes-if-dirty + uploads the light/fog/decal buffers, fills the
		// perFrame *Count fields. It binds no descriptor set — the light/fog
		// buffers ride the per-world set kWorldSet, bound by each consuming pass;
		// the set-2 decal buffers are bound at the composite pass.
		void SubmitToGpu(SceneConstants& perFrame);

		// Persistent per-world fog-area buffer (FogAreaParams[], bound on the
		// per-world set kWorldSet by each consuming pass). Baked once by Compile()
		// over ALL fog
		// areas (no frustum cull — the per-pixel Fog.slang loop is bounded by the
		// count); RefreshFogAreas() patches only entries whose GPU bytes changed
		// (color/transform/start/end/...), so dynamic fog updates without a full
		// re-upload. Null until baked (no fog / no hybrid renderer).
		RIBuffer* GetFogAreaBuffer() const { return mpFogAreaBuffer.Get(); }
		uint32_t GetFogAreaCount() const { return (uint32_t)mvFogAreas.size(); }

		// Persistent per-world light buffers (PointLight[]/SpotLight[]/RectLight[],
		// bound into set-0 kBinding{Point,Spot,Area}Lights). Hold ALL world lights
		// of each type at stable slots (slot = index), sized to the world's light
		// count (no 256/256/64 cap). Counts become world totals, which the GPU
		// light grid + Scene.slang decode use directly. RefreshLights() patches only
		// changed entries (flicker/move); invisible lights get radius 0 so the grid
		// skips them. Null until baked (no hybrid renderer).
		RIBuffer* GetPointLightBuffer() const { return mpPointLightBuffer.Get(); }
		uint32_t  GetPointLightCount()  const { return (uint32_t)mvPointLights.size(); }
		RIBuffer* GetSpotLightBuffer()  const { return mpSpotLightBuffer.Get(); }
		uint32_t  GetSpotLightCount()   const { return (uint32_t)mvSpotLights.size(); }
		RIBuffer* GetAreaLightBuffer()  const { return mpAreaLightBuffer.Get(); }
		uint32_t  GetAreaLightCount()   const { return (uint32_t)mvAreaLights.size(); }

		// Notify that the world's light / fog / decal *membership* changed, so the
		// corresponding persistent GPU buffer is re-baked on the next Refresh*.
		// Set by the Create*/Destroy* methods (which the editor also goes through),
		// so live edits work without Compile; init true so an uncompiled world
		// (level editor) bakes on its first Draw. Property changes (move/colour/
		// flicker) don't need this — RefreshLights/RefreshFogAreas re-upload content
		// every frame.
		void MarkLightBuffersDirty() { mbLightBuffersDirty = true; }
		void MarkFogBufferDirty()    { mbFogBufferDirty = true; }
		void MarkDecalBuffersDirty() { mbDecalBuffersDirty = true; }
		cMeshEntity* GetDynamicMeshEntity(const tString& asName);
		
		cMeshEntityIterator GetDynamicMeshEntityIterator();
		cMeshEntityIterator GetStaticMeshEntityIterator();
		
		void DrawMeshBoundingBoxes(const cColor& aColor, bool abStatic) {} // STUB
		
		///// LIGHT METHODS ////////////////////

		cLightPoint* CreateLightPoint(const tString &asName="",const tString &asGobo="", bool abStatic=false);
		cLightSpot* CreateLightSpot(const tString &asName="", const tString &asGobo="", bool abStatic=false);
		cLightBox* CreateLightBox(const tString &asName="", bool abStatic=false);
		cLightArea* CreateLightArea(const tString &asName="", bool abStatic=false);
		void DestroyLight(iLight* apLight);
		iLight* GetLight(const tString& asName);
		iLight* GetLightFromUniqueID(int alID);

		tLightList * GetLightList(){ return &mlstLights;}

		cLightListIterator GetLightIterator(){ return cLightListIterator(&mlstLights);}

		///// BILLBOARD METHODS ////////////////////

		cBillboard* CreateBillboard(const tString& asName, const cVector2f& avSize,eBillboardType aType,const tString& asMaterial="",bool abStatic=false);
		void DestroyBillboard(cBillboard* apObject);
		cBillboard* GetBillboard(const tString& asName);
		cBillboard* GetBillboardFromUniqueID(int alID);
		cBillboardIterator GetBillboardIterator();

		///// BEAM METHODS ////////////////////

		cBeam* CreateBeam(const tString& asName, bool abStatic=false);
		void DestroyBeam(cBeam* apObject);
		cBeam* GetBeam(const tString& asName);
		cBeam* GetBeamFromUniqueID(int alID);
		cBeamIterator GetBeamIterator();

		///// PARTICLE METHODS ////////////////////

		cParticleSystem* CreateParticleSystem(	const tString& asName,const tString& asType, const cVector3f& avSize, bool abRemoveWhenDead=true);
		cParticleSystem* CreateParticleSystem(	const tString& asName,const tString& asDataName, tinyxml2::XMLElement* apElement, const cVector3f& avSize);
		void DestroyParticleSystem(cParticleSystem* apPS);
		cParticleSystem* GetParticleSystem(const tString& asName);
		cParticleSystem* GetParticleSystemFromUniqueID(int alID);
		bool ParticleSystemExists(cParticleSystem* apPS);

		void DestroyAllParticleSystems();

		cParticleSystemIterator GetParticleSystemIterator(){ return cParticleSystemIterator(&mlstParticleSystems);}

		///// GUISET ENTITY METHODS ////////////////////
		
		cGuiSetEntity* CreateGuiSetEntity(const tString& asName, cGuiSet *apSet, bool abStatic=false);
		void DestroyGuiSetEntity(cGuiSetEntity* apObject);
		cGuiSetEntity* GetGuiSetEntity(const tString& asName);
		cGuiSetEntity* GetGuiSetEntityFromUniqueID(int alID);
		cGuiSetEntityIterator GetGuiSetEntityIterator();

		///// ROPE ENTITY METHODS ////////////////////
		
		cRopeEntity* CreateRopeEntity(const tString& asName, iPhysicsRope *apRope, int alMaxSegments);
		void DestroyRopeEntity(cRopeEntity* apRope);
		cRopeEntity* GetRopeEntity(const tString& asName);
		cRopeEntity* GetRopeEntityFromUniqueID(int alID);
		cRopeEntityIterator GetRopeEntityIterator();

		///// FOG AREA METHODS ////////////////////
		cFogArea* CreateFogArea(const tString& asName, bool abStatic=false);
		void DestroyFogArea(cFogArea* apRope);
		cFogArea* GetFogArea(const tString& asName);
		cFogArea* GetFogAreaFromUniqueID(int alID);
		cFogAreaIterator GetFogAreaIterator();

		///// SOUND ENTITY METHODS ////////////////////

		cSoundEntity* CreateSoundEntity(const tString &asName,const tString &asSoundEntity, 
										bool abRemoveWhenOver);
		void DestroySoundEntity(cSoundEntity* apEntity);
		cSoundEntity* GetSoundEntity(const tString& asName);
		cSoundEntity* GetSoundEntityFromUniqueID(int alID);
		void DestroyAllSoundEntities();
		bool SoundEntityExists(cSoundEntity* apEntity, int alCreationID);

		cSoundEntityIterator GetSoundEntityIterator(){ return cSoundEntityIterator(&mlstSoundEntities);}

		///// START POS ENTITY METHODS ////////////////
		
		cStartPosEntity* CreateStartPos(const tString &asName);
		cStartPosEntity* GetStartPosEntity(const tString &asName);
		cStartPosEntity* GetFirstStartPosEntity();

		///// AI NODE METHODS ////////////////

		void GenerateAINodes(cAINodeGeneratorParams *apParams);

		cAINodeContainer* CreateAINodeContainer(const tString &asName, 
											const tString &asNodeName, 
											const cVector3f &avSize,
											bool abNodeIsAtCenter, 
											int alMinEdges, int alMaxEdges, float afMaxEdgeDistance,
											float afMaxHeight);

		cAStarHandler* CreateAStarHandler(cAINodeContainer* apContainer);
		void DestroyAStarHandler(cAStarHandler* apHandler);
        
		void AddAINode(const tString &asName, int alID, const tString &asType, const cVector3f &avPosition);
		tTempAiNodeList* GetAINodeList(const tString &asType);


		/// NODE METHODS //////////////////////
		// Remove this for the time being, not need it seems.
		//cNode3D* GetRootNode(){ return mpRootNode; }

		///// DUMMY RENDERABLE METHODS ////////////////////
		cDummyRenderable* CreateDummyRenderable(const tString& asName, bool abStatic=false);
		void DestroyDummyRenderable(cDummyRenderable* apDummy);
		cDummyRenderable* GetDummyRenderable(const tString& asName);
		cDummyRenderable* GetDummyRenderableFromUniqueID(int alID);
		cDummyRenderableIterator GetDummyRenderableIterator();


	private:
		void AddRenderableToContainer(iRenderable *apObject);
		void RemoveRenderableFromContainer(iRenderable *apObject);

		void UpdateEntities(float afTimeStep);
		void UpdateParticles(float afTimeStep);
		void UpdateLights(float afTimeStep);
		void UpdateSoundEntities(float afTimeStep);

		tString msName;
		tWString msFilePath;
		bool mbActive;

		cGraphics *mpGraphics;
		cSound* mpSound;
		cResources *mpResources;
		cPhysics *mpPhysics;
		cScene *mpScene;
		cSystem *mpSystem;
		cAI *mpAI;
		cHaptic *mpHaptic;

		iPhysicsWorld *mpPhysicsWorld;
		bool mbAutoDeletePhysicsWorld;

		bool mbIsSoundEmitter;
		
		cVector3f mvWorldSize;

		iRenderableContainer* mpRenderableContainer[2];

		cVertexBuffer* mpSkyBoxVtxBuffer;
		Image* mpSkyBoxImage = nullptr;
		bool mbAutoDestroySkybox;
		bool mbSkyBoxActive;
		cColor mSkyBoxColor;

		bool mbFogActive;
		bool mbFogCulling;
		float mfFogStart;
		float mfFogEnd;
		float mfFogFalloffExp;
		cColor mFogColor;

		tLightList mlstLights;
		tMeshEntityList mlstDynamicMeshEntities;
		tMeshEntityList mlstStaticMeshEntities;
		std::vector<cDecal*> mvDecals;
		std::vector<uint32_t> mvDecalObjectIndices;   // flat per-object decal-index pool (Compile())
		// Baked static decal GPU state (owned). Decal geometry is baked once; the
		// diffuse texture slot is NOT pinned — mvDecalTexSlots caches the current
		// textures_2d[] slot per decal, re-resolved each frame from the normal LRU
		// by RefreshDecalTextures (which keeps the texture MRU so the slot stays
		// stable), and the GPU diffuseTexIndex is patched only when a slot changes.
		RISharedPointer<RIBuffer> mpDecalBuffer;
		RISharedPointer<RIBuffer> mpDecalObjectIndexBuffer;

		std::vector<uint32_t> mvDecalTexSlots;
		void BakeDecalBuffers();      // build/upload the per-world decal geometry (Compile)
		void DisposeDecalBuffers();   // defer-dispose the per-world decal buffers

		// Per-type per-frame GPU sync (bake-if-dirty + upload). Internal steps of
		// SubmitToGpu(); not called directly by the renderer.
		void RefreshLights();
		void RefreshFogAreas();
		void RefreshDecalTextures();

		// Persistent per-world fog-area GPU buffer. mvFogAreas is the stable
		// bake-order snapshot (slot = index). Baked once for sizing/membership;
		// RefreshFogAreas re-uploads the whole buffer each frame.
		RISharedPointer<RIBuffer> mpFogAreaBuffer;
		std::vector<cFogArea*> mvFogAreas;
		void BakeFogBuffer();         // build/upload all fog areas (Compile)
		void DisposeFogBuffers();     // defer-dispose the per-world fog buffer

		// Persistent per-world light buffers (point/spot/area). mv*Lights is the
		// stable bake-order snapshot per type (slot = index). Baked once for
		// sizing/membership; RefreshLights re-uploads each whole buffer per frame.
		RISharedPointer<RIBuffer> mpPointLightBuffer;
		RISharedPointer<RIBuffer> mpSpotLightBuffer;
		RISharedPointer<RIBuffer> mpAreaLightBuffer;
		std::vector<iLight*> mvPointLights;
		std::vector<iLight*> mvSpotLights;
		std::vector<iLight*> mvAreaLights;
		void BakeLightBuffers();      // build/upload all world lights by type (Compile)
		void DisposeLightBuffers();   // defer-dispose the per-world light buffers

		// Membership-dirty flags: set by Create*/Destroy* (Mark*Dirty), cleared by
		// the matching Bake*. Init true so an uncompiled world (editor) bakes on its
		// first Refresh*.
		bool mbLightBuffersDirty = true;
		bool mbFogBufferDirty = true;
		bool mbDecalBuffersDirty = true;
		tBillboardList mlstBillboards;
		tBeamList mlstBeams;
		tParticleSystemList mlstParticleSystems;
		tGuiSetEntityList mlstGuiSetEntities;
		tRopeEntityList mlstRopeEntities;
		tSoundEntityList mlstSoundEntities;
		tStartPosEntityList mlstStartPosEntities;
		tAreaEntityMap m_mapAreaEntities;
		tFogAreaList mlstFogAreas;
		tDummyRenderableList mlstDummyRenderables;

		int mlSoundCreationIDCount;
		std::vector<SharedResourceHandle<cEntFile>> entityCache;

		tAINodeContainerList mlstAINodeContainers;
		tAStarHandlerList mlstAStarHandlers;
		tTempNodeContainerMap m_mapTempNodes;

		cNode3D* mpRootNode;

		tString msMapName;
		cColor mAmbientColor;
	};

};
#endif // HPL_WOLRD_H
