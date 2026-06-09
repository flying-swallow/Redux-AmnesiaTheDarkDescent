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

#ifndef HPL_RENDERER_H
#define HPL_RENDERER_H

#include "graphics/GraphicsTypes.h"
#include "math/MathTypes.h"
#include "scene/SceneTypes.h"

#include "graphics/RenderFunctions.h"
#include "graphics/RIBootstrap.h"

namespace hpl {

	//---------------------------------------------

	class cGraphics;
	class cResources;
	class cEngine;
	class iLowLevelResources;
	class cMeshCreator;
	class cGpuShaderManager;
	class iRenderable;
	class cWorld;
	class cRenderSettings;
	class cRenderList;
	class cProgramComboManager;
	class iLight;
	class cBoundingVolume;
	class iRenderableContainer;
	class iRenderableContainerNode;
	class cVisibleRCNodeTracker;
	class cViewport;

	//---------------------------------------------

	class cRenderSettings
	{
	public:
		cRenderSettings(bool abIsReflection = false);
		~cRenderSettings();

		////////////////////////////
		//Helper methods
		void ResetVariables();

		void SetupReflectionSettings();

		void AddOcclusionPlane(const cPlanef &aPlane);
		void ResetOcclusionPlanes();

		////////////////////////////
		//Data
		cRenderList *mpRenderList;

		cVisibleRCNodeTracker *mpVisibleNodeTracker;

		cRenderSettings *mpReflectionSettings;

        ////////////////////////////
		//General settings
		bool mbLog;

		cColor mClearColor;

		////////////////////////////
		//Render settings
		int mlMinimumObjectsBeforeOcclusionTesting;
		int mlSampleVisiblilityLimit;
		bool mbIsReflection;
		bool mbClipReflectionScreenRect;

		bool mbUseOcclusionCulling;

		bool mbUseEdgeSmooth;

		tPlanefVec mvOcclusionPlanes;

		bool mbUseCallbacks;

		eShadowMapResolution mMaxShadowMapResolution;

		bool mbUseScissorRect;
		cVector2l mvScissorRectPos;
		cVector2l mvScissorRectSize;

		bool mbRenderWorldReflection;

		////////////////////////////
		//Shadow settings
		bool mbRenderShadows;
		float mfShadowMapBias;
		float mfShadowMapSlopeScaleBias;

		////////////////////////////
		//Light settings
		bool mbSSAOActive;

		////////////////////////////
		//Output
		int mlNumberOfLightsRendered;
		int mlNumberOfOcclusionQueries;

	private:

	};

	//---------------------------------------------

	class iRenderer : public iRenderFunctions
	{
	friend class cRendererCallbackFunctions;
	friend class cRenderSettings;
	public:
		iRenderer(const tString& asName, cGraphics *apGraphics,cResources* apResources, int alNumOfProgramComboModes);
		virtual ~iRenderer();

		void Update(float afTimeStep);

    virtual void Draw(
    		RIBootstrap::FrameContext* cntx,
        cViewport* viewport,
        float afFrameTime,
        cFrustum* apFrustum,
        cWorld* apWorld,
        cRenderSettings* apSettings,
        bool abSendFrameBufferToPostEffects) {}

		inline static int GetRenderFrameCount()  { return mlRenderFrameCount;}
		inline static void IncRenderFrameCount() { ++mlRenderFrameCount;}

		float GetTimeCount(){ return mfTimeCount;}

		virtual bool LoadData()=0;
		virtual void DestroyData()=0;

		virtual iTexture* GetRefractionTexture(){ return NULL;}
		virtual iTexture* GetReflectionTexture(){ return NULL;}

		// Legacy material-path accessors — still referenced by the
		// MaterialType_* program setup code; never populated by the Vulkan
		// Draw() path.
		cWorld *GetCurrentWorld(){ return mpCurrentWorld;}
		cFrustum *GetCurrentFrustum(){ return mpCurrentFrustum;}
		cRenderList *GetCurrentRenderList(){ return mpCurrentRenderList;}

		//Temp variables used by material.
		float GetTempAlpha(){ return mfTempAlpha; }

		//Static settings. Must be set before renderer data load.
		static void SetShadowMapQuality(eShadowMapQuality aQuality) { mShadowMapQuality = aQuality;}
		static eShadowMapQuality GetShadowMapQuality(){ return mShadowMapQuality;}

		static void SetShadowMapResolution(eShadowMapResolution aResolution) { mShadowMapResolution = aResolution;}
		static eShadowMapResolution GetShadowMapResolution(){ return mShadowMapResolution;}

		static void SetParallaxQuality(eParallaxQuality aQuality) { mParallaxQuality = aQuality;}
		static eParallaxQuality GetParallaxQuality(){ return mParallaxQuality;}

		static void SetParallaxEnabled(bool abX) { mbParallaxEnabled = abX;}
		static bool GetParallaxEnabled(){ return mbParallaxEnabled;}

		static void SetReflectionSizeDiv(int alX) { mlReflectionSizeDiv = alX;}
		static int GetReflectionSizeDiv(){ return mlReflectionSizeDiv;}

		static void SetRefractionEnabled(bool abX) { mbRefractionEnabled = abX;}
		static bool GetRefractionEnabled(){ return mbRefractionEnabled;}

	protected:
        cResources* mpResources;
		cGpuShaderManager *mpShaderManager;

		cProgramComboManager* mpProgramManager;

		tString msName;

		cWorld *mpCurrentWorld;
		cRenderSettings *mpCurrentSettings;
		cRenderList *mpCurrentRenderList;

		float mfTempAlpha;

		static int mlRenderFrameCount;
		float mfTimeCount;

        //Static variables
		static eShadowMapQuality mShadowMapQuality;
		static eShadowMapResolution mShadowMapResolution;
		static eParallaxQuality mParallaxQuality;
		static bool mbParallaxEnabled;
		static int mlReflectionSizeDiv;
		static bool mbRefractionEnabled;
	};

	//---------------------------------------------

	class cRendererCallbackFunctions
	{
	public:
		cRendererCallbackFunctions(iRenderer *apRenderer) : mpRenderer(apRenderer) {}

		cRenderSettings* GetSettings(){ return mpRenderer->mpCurrentSettings;}
		cFrustum* GetFrustum(){ return mpRenderer->mpCurrentFrustum;}

		inline void SetFlatProjection(const cVector2f &avSize=1,float afMin=-100,float afMax=100) { mpRenderer->SetFlatProjection(avSize, afMin,afMax); }
		inline void SetFlatProjectionMinMax(const cVector3f &avMin,const cVector3f &avMax) { mpRenderer->SetFlatProjectionMinMax(avMin,avMax);}
		inline void SetNormalFrustumProjection() { mpRenderer->SetNormalFrustumProjection(); }

		inline void SetFrameBuffer(iFrameBuffer *apFrameBuffer, bool abUsePosAndSize=false){ mpRenderer->SetFrameBuffer(apFrameBuffer,abUsePosAndSize); }
		inline void ClearFrameBuffer(tClearFrameBufferFlag aFlags, bool abUsePosAndSize){ mpRenderer->ClearFrameBuffer(aFlags, abUsePosAndSize); }

		inline void DrawQuad(	const cVector3f& aPos, const cVector2f& avSize, const cVector2f& avMinUV=0, const cVector2f& avMaxUV=1,
								bool abInvertY=false, const cColor& aColor=cColor(1,1) )
							{ mpRenderer->DrawQuad(aPos,avSize,avMinUV,avMaxUV,abInvertY,aColor); }

		inline bool SetDepthTest(bool abX){ return mpRenderer->SetDepthTest(abX); }
		inline bool SetDepthWrite(bool abX){ return mpRenderer->SetDepthWrite(abX); }
		inline bool SetDepthTestFunc(eDepthTestFunc aFunc){ return mpRenderer->SetDepthTestFunc(aFunc); }
		inline bool SetCullActive(bool abX){ return mpRenderer->SetCullActive(abX); }
		inline bool SetCullMode(eCullMode aMode){ return mpRenderer->SetCullMode(aMode); }
		inline bool SetStencilActive(bool abX){ return mpRenderer->SetStencilActive(abX); }
		inline bool SetScissorActive(bool abX){ return mpRenderer->SetScissorActive(abX); }
		inline bool SetScissorRect(const cVector2l& avPos, const cVector2l& avSize, bool abAutoEnabling){return mpRenderer->SetScissorRect(avPos, avSize, abAutoEnabling);}
		inline bool SetScissorRect(const cRect2l& aClipRect, bool abAutoEnabling){return mpRenderer->SetScissorRect(aClipRect,abAutoEnabling);}
		inline bool SetChannelMode(eMaterialChannelMode aMode){ return mpRenderer->SetChannelMode(aMode); }
		inline bool SetAlphaMode(eMaterialAlphaMode aMode){ return mpRenderer->SetAlphaMode(aMode); }
		inline bool SetBlendMode(eMaterialBlendMode aMode){ return mpRenderer->SetBlendMode(aMode); }
		inline bool SetProgram(iGpuProgram *apProgram){ return mpRenderer->SetProgram(apProgram); }
		inline void SetTexture(int alUnit, iTexture *apTexture){ mpRenderer->SetTexture(alUnit, apTexture); }
		inline void SetTextureRange(iTexture *apTexture, int alFirstUnit, int alLastUnit = kMaxTextureUnits-1){ mpRenderer->SetTextureRange(apTexture,alFirstUnit,alLastUnit); }
		inline void SetVertexBuffer(iVertexBuffer *apVtxBuffer){ mpRenderer->SetVertexBuffer(apVtxBuffer); }
		inline void SetMatrix(cMatrixf *apMatrix){ mpRenderer->SetMatrix(apMatrix); }
		inline void SetModelViewMatrix(const cMatrixf& a_mtxModelView){ mpRenderer->SetModelViewMatrix(a_mtxModelView); }

		inline void DrawCurrent(eVertexBufferDrawType aDrawType = eVertexBufferDrawType_LastEnum){ mpRenderer->DrawCurrent(aDrawType); }

		void DrawWireFrame(iVertexBuffer *apVtxBuffer, const cColor &aColor){ mpRenderer->DrawWireFrame(apVtxBuffer, aColor);}

		iLowLevelGraphics *GetLowLevelGfx(){ return mpRenderer->mpLowLevelGraphics;}

	private:
		iRenderer *mpRenderer;
	};

	//---------------------------------------------

};
#endif // HPL_RENDERER_H
