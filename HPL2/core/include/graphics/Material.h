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

#ifndef HPL_MATERIAL_H
#define HPL_MATERIAL_H

#include "system/SystemTypes.h"
#include "math/MathTypes.h"
#include "graphics/GraphicsTypes.h"
#include "graphics/ImageResourceWrapper.h"
#include "graphics/IndexPool.h"
#include "resources/ResourceBase.h"

#include <array>
#include <cstdint>

namespace hpl {

	//---------------------------------------------------

	class cGraphics;
	class cResources;
	class Image;
	class iMaterialType;
	class cResourceVarsObject;

	//---------------------------------------------------
	// Bindless material descriptor data. Lives next to legacy fields during
	// the Vulkan transition; renderers pack these into a per-frame SSBO indexed
	// by cMaterial::Index().

	struct MaterialDecal final {
		eMaterialBlendMode m_blend;
	};

	struct MaterialDiffuseSolid final {
		float m_heightMapScale;
		float m_heightMapBias;
		float m_frenselBias;
		float m_frenselPow;
		bool m_alphaDissolveFilter;
	};

	struct MaterialTranslucent final {
		eMaterialBlendMode m_blend;

		bool m_isAffectedByLightLevel;
		bool m_hasRefraction;
		bool m_refractionEdgeCheck;
		bool m_refractionNormals;

		float m_refractionScale;
		float m_frenselBias;
		float m_frenselPow;
		float m_rimLightMul;
		float m_rimLightPow;
	};

	struct MaterialWater final {
		bool m_hasReflection;
		bool m_isLargeSurface;
		bool m_worldReflectionOcclusionTest;

		float m_refractionScale;
		float m_frenselBias;
		float m_frenselPow;
		float m_reflectionFadeStart;
		float m_reflectionFadeEnd;
		float m_waveSpeed;
		float m_waveAmplitude;
		float m_waveFreq;
	};

	enum class MaterialID : uint8_t {
		Unknown = 0,
		SolidDiffuse,
		Translucent,
		Water,
		Decal,
		MaterialIDCount
	};

	struct ShaderMaterialData final {
		MaterialID m_id = MaterialID::Unknown;
		union {
			MaterialDecal m_decal;
			MaterialDiffuseSolid m_solid;
			MaterialTranslucent m_translucent;
			MaterialWater m_water;
		};
	};

	//---------------------------------------------------
	
	class iMaterialVars
	{
	public:
		virtual ~iMaterialVars(){}
	};

	//---------------------------------------------------
	
	class cMaterialUvAnimation
	{
	public:
		cMaterialUvAnimation(eMaterialUvAnimation aType, float afSpeed, float afAmp, eMaterialAnimationAxis aAxis) :
							  mType(aType), mfSpeed(afSpeed), mfAmp(afAmp), mAxis(aAxis) {}

	    eMaterialUvAnimation mType;
		
		float mfSpeed;
		float mfAmp;

		eMaterialAnimationAxis mAxis;
	};

	//---------------------------------------------------
	
	class cMaterial : public iResourceBase
	{
	friend class iMaterialType;
	public:
		static constexpr uint32_t MaxMaterialID = 2048;
		static constexpr bool IsTranslucent(MaterialID id) {
			return id == MaterialID::Water ||
				id == MaterialID::Translucent ||
				id == MaterialID::Decal;
		}

		cMaterial(const tString& asName, const tWString& asFullPath, cGraphics *apGraphics, cResources *apResources, iMaterialType *apType);
		virtual ~cMaterial();

		// Bindless descriptor (coexists with legacy mb*/mf* fields during transition).
		// Built by iMaterialType::CompileMaterialSpecifics; uploaded by the renderer
		// to a single SSBO indexed by Index(). Bumps the generation counter so the
		// renderer's dirty-check fires on next frame.
		inline void SetDescriptor(const ShaderMaterialData& desc) {
			m_descriptor = desc;
			IncreaseGeneration();
		}
		inline const ShaderMaterialData& Descriptor() const { return m_descriptor; }

		inline uint32_t Generation() const { return m_generation; }
		inline void IncreaseGeneration() { m_generation++; }

		void SetType(iMaterialType* apType);
		iMaterialType * GetType(){ return mpType; }

		void Compile();

		// Image* binding API. Storage owns the Image (auto-destroyed when the
		// material is destroyed), unless SetAutoDestroyTextures(false) was set.
		void SetImage(eMaterialTexture aType, Image* apImage);
		Image* GetImage(eMaterialTexture aType) const;

		// Per-material sampler state. Applied uniformly to all texture bindings
		// at draw time by the Forward+ shader's static samplers.
		inline eTextureWrap GetTextureWrap() const { return m_textureWrap; }
		inline eTextureFilter GetTextureFilter() const { return m_textureFilter; }
		inline float GetTextureAnisotropy() const { return m_textureAnisotropy; }
		void setTextureWrap(eTextureWrap aWrap);
		void setTextureFilter(eTextureFilter aFilter);
		void SetTextureAnisotropy(float afX);

		void SetVars(iMaterialVars *apVars){ mpVars = apVars;}
		iMaterialVars* GetVars(){ return mpVars;}
		cResourceVarsObject* GetVarsObject();
		void LoadVariablesFromVarsObject(cResourceVarsObject* apVarsObject);

		void SetAutoDestroyTextures(bool abX){ mbAutoDestroyTextures = abX;}

		void SetBlendMode(eMaterialBlendMode aBlendMode);
		void SetAlphaMode(eMaterialAlphaMode aAlphaMode);
		void SetDepthTest(bool abDepthTest);

		bool HasRefraction(){ return mbHasRefraction; }
		bool UseRefractionEdgeCheck(){ return mbUseRefractionEdgeCheck;}
		void SetHasRefraction(bool abX){ mbHasRefraction = abX; }
		void SetUseRefractionEdgeCheck(bool abX){ mbUseRefractionEdgeCheck = abX;}
		
		bool HasWorldReflection(){ return mbHasWorldReflection; }
		void SetHasWorldReflection(bool abX){ mbHasWorldReflection = abX; }
		void  SetWorldReflectionOcclusionTest(bool abX){ mbWorldReflectionOcclusionTest=abX;}
		void SetMaxReflectionDistance(float afX){ mfMaxReflectionDistance = afX;}
		bool  GetWorldReflectionOcclusionTest(){ return mbWorldReflectionOcclusionTest;}
		float GetMaxReflectionDistance(){ return mfMaxReflectionDistance;}

		void SetHasTranslucentIllumination(bool abX){ mbHasTranslucentIllumination = abX;}
		bool HasTranslucentIllumination(){ return mbHasTranslucentIllumination;}

		void SetLargeTransperantSurface(bool abX){ mbLargeTransperantSurface = abX;}
		bool GetLargeTransperantSurface(){ return mbLargeTransperantSurface;}

		bool GetUseAlphaDissolveFilter(){ return mbUseAlphaDissolveFilter;}
		void SetUseAlphaDissolveFilter(bool abX){ mbUseAlphaDissolveFilter = abX;}

		void SetAffectedByFog(bool abX){ mbAffectedByFog = abX;}
		bool GetAffectedByFog(){ return mbAffectedByFog;}
		
		inline eMaterialBlendMode GetBlendMode() const { return mBlendMode; }
		inline eMaterialAlphaMode GetAlphaMode() const { return mAlphaMode; }
		inline bool GetDepthTest() const { return mbDepthTest; }

		void SetPhysicsMaterial(const tString & asPhysicsMaterial){ msPhysicsMaterial = asPhysicsMaterial;}
		const tString& GetPhysicsMaterial(){ return msPhysicsMaterial;}

		void UpdateBeforeRendering(float afTimeStep);

		inline int GetRenderFrameCount() const { return mlRenderFrameCount;}
		inline void SetRenderFrameCount(const int alCount) { mlRenderFrameCount = alCount;}

		inline bool GetHasSpecificSettings(eMaterialRenderMode aMode) const{ return mbHasSpecificSettings[aMode];}
		void SetHasSpecificSettings(eMaterialRenderMode aMode,bool abX){ mbHasSpecificSettings[aMode] = abX;}

		inline bool HasObjectSpecificsSettings(eMaterialRenderMode aMode)const { return  mbHasObjectSpecificsSettings[aMode];}
		void SetHasObjectSpecificsSettings(eMaterialRenderMode aMode,bool abX){ mbHasObjectSpecificsSettings[aMode] = abX;}

		//Animation
		void AddUvAnimation(eMaterialUvAnimation aType, float afSpeed, float afAmp, eMaterialAnimationAxis aAxis);
		int GetUvAnimationNum(){ return (int)mvUvAnimations.size();}
		cMaterialUvAnimation *GetUvAnimation(int alIdx){ return &mvUvAnimations[alIdx]; }
		inline bool HasUvAnimation()const { return mbHasUvAnimation; }
		inline const cMatrixf& GetUvMatrix() const { return m_mtxUV;}
		void ClearUvAnimations();

		/**
		 * This is used so that materials do not call type specific things after types have been destroyed!
		 * Shall only be set by graphics!
		 */
		static void SetDestroyTypeSpecifics(bool abX){ mbDestroyTypeSpecifics = abX; }
		static bool GetDestroyTypeSpecifics(){ return mbDestroyTypeSpecifics; }

		//resources stuff.
		bool Reload(){ return false;}
		void Unload(){}
		void Destroy(){}
		
	private:
		void UpdateAnimations(float afTimeStep);

		cGraphics *mpGraphics;
		cResources *mpResources;

		iMaterialType *mpType;

		iMaterialVars *mpVars;

		bool mbAutoDestroyTextures;

		bool mbHasSpecificSettings[eMaterialRenderMode_LastEnum];
		bool mbHasObjectSpecificsSettings[eMaterialRenderMode_LastEnum];

		eMaterialBlendMode mBlendMode;
		eMaterialAlphaMode mAlphaMode;
		bool mbDepthTest;

		bool mbHasRefraction;
		int mlRefractionTextureUnit;
		bool mbUseRefractionEdgeCheck;

		bool mbHasWorldReflection;
		int mlWorldReflectionTextureUnit;
		bool mbWorldReflectionOcclusionTest;
		float mfMaxReflectionDistance;

		bool mbHasTranslucentIllumination;

		bool mbLargeTransperantSurface;

		bool mbAffectedByFog;

		bool mbUseAlphaDissolveFilter;

		// Image* binding storage, indexed by eMaterialTexture.
		std::array<ImageResourceWrapper, eMaterialTexture_LastEnum> m_image;

		// Per-material sampler state.
		eTextureWrap m_textureWrap = eTextureWrap_Repeat;
		eTextureFilter m_textureFilter = eTextureFilter_Bilinear;
		float m_textureAnisotropy = 1.0f;

		std::vector<cMaterialUvAnimation> mvUvAnimations;
		bool mbHasUvAnimation;
		cMatrixf m_mtxUV;
		float mfAnimTime;
		
		int mlRenderFrameCount;

		tString msPhysicsMaterial;

		// Bindless descriptor + GPU slot (transition: lives alongside legacy fields).
		ShaderMaterialData m_descriptor;
		uint32_t m_generation = 0;

		static bool mbDestroyTypeSpecifics;
	};

	//---------------------------------------------------

};
#endif // HPL_MATERIAL_H
