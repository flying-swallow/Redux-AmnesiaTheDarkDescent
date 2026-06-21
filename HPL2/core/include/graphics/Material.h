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
#include "resources/ResourceBase.h"

#include <cstdint>
#include <variant>

namespace hpl {

	//---------------------------------------------------

	class cGraphics;
	class cResources;
	class Image;
	class iMaterialType;
	class cResourceVarsObject;

	//---------------------------------------------------
	// Per-material-type data blob. Each alternative owns its own texture set
	// (different per type) plus its scalar params; cMaterial stores exactly one of
	// these in the MaterialData variant below. iMaterialType::LoadVariables builds
	// the blob directly from XML (no separate compile pass). The renderer packs
	// each into its per-type GPU material table.
	//
	// Pipeline-side settings (blend/alpha/depth, sampler, refraction/reflection
	// routing, physics material, UV animation) are NOT material data and live on
	// cMaterial — they feed pipeline state, sorting and physics, not the buffer.

	struct MaterialDiffuseSolid final {
		SharedResourceHandle<Image> m_diffuse;
		SharedResourceHandle<Image> m_nmap;
		SharedResourceHandle<Image> m_specular;
		SharedResourceHandle<Image> m_alpha;
		SharedResourceHandle<Image> m_height;
		SharedResourceHandle<Image> m_illumination;
		SharedResourceHandle<Image> m_dissolveAlpha;
		SharedResourceHandle<Image> m_cubeMap;
		SharedResourceHandle<Image> m_cubeMapAlpha;

		float m_heightMapScale = 0.05f;
		float m_heightMapBias = 0.0f;
		float m_frenselBias = 0.2f;
		float m_frenselPow = 8.0f;
		bool m_alphaDissolveFilter = false;
	};

	struct MaterialTranslucent final {
		SharedResourceHandle<Image> m_diffuse;
		SharedResourceHandle<Image> m_nmap;
		SharedResourceHandle<Image> m_cubeMap;
		SharedResourceHandle<Image> m_cubeMapAlpha;

		bool m_refraction = false;          // authored value (round-tripped to XML)
		bool m_refractionEdgeCheck = true;
		bool m_refractionNormals = true;
		bool m_isAffectedByLightLevel = false;

		float m_refractionScale = 0.1f;
		float m_frenselBias = 0.2f;
		float m_frenselPow = 8.0f;
		float m_rimLightMul = 0.0f;
		float m_rimLightPow = 8.0f;
	};

	struct MaterialWater final {
		SharedResourceHandle<Image> m_diffuse;
		SharedResourceHandle<Image> m_nmap;
		SharedResourceHandle<Image> m_cubeMap;

		bool m_hasReflection = true;

		float m_refractionScale = 0.1f;
		float m_frenselBias = 0.2f;
		float m_frenselPow = 8.0f;
		float m_reflectionFadeStart = 0.0f;
		float m_reflectionFadeEnd = 0.0f;
		float m_waveSpeed = 1.0f;
		float m_waveAmplitude = 1.0f;
		float m_waveFreq = 1.0f;
	};

	struct MaterialDecal final {
		SharedResourceHandle<Image> m_diffuse;
	};

	enum class MaterialID : uint8_t {
		Unknown = 0,
		SolidDiffuse,
		Translucent,
		Water,
		Decal,
		MaterialIDCount
	};

	// std::monostate is alternative 0: the blank/unknown state for a freshly
	// constructed material and the fake-material loader path (neither calls
	// LoadVariables). Each texture slot is a refcounted SharedResourceHandle<Image>,
	// so MaterialData is copyable; cMaterial is heap-owned and holds exactly one
	// alternative.
	using MaterialData = std::variant<std::monostate,
		MaterialDiffuseSolid, MaterialTranslucent, MaterialWater, MaterialDecal>;

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
	public:
		static constexpr bool IsTranslucent(MaterialID id) {
			return id == MaterialID::Water ||
				id == MaterialID::Translucent ||
				id == MaterialID::Decal;
		}

		cMaterial(const tString& asName, const tWString& asFullPath, cGraphics *apGraphics, cResources *apResources, iMaterialType *apType);
		virtual ~cMaterial();

		// Per-type material data blob (textures + scalar params). Built directly by
		// iMaterialType::LoadVariables; the renderer reads it via std::visit and
		// packs it into the per-type GPU material table. Mutated in place by the
		// type and by SetImage.
		inline MaterialData& Data() { return m_data; }
		inline const MaterialData& Data() const { return m_data; }

		// Which alternative the variant holds, mapped to the stable MaterialID enum.
		MaterialID GetMaterialID() const;

		// Bumped whenever the blob changes (end of LoadVariables); the renderer's
		// per-material upload cookie folds this in so edits re-upload next frame.
		inline uint32_t Generation() const { return m_generation; }
		inline void IncreaseGeneration() { m_generation++; }

		void SetType(iMaterialType* apType);
		iMaterialType * GetType(){ return mpType; }

		// Image* binding API. Storage owns the Image (auto-destroyed when the
		// material is destroyed), unless SetAutoDestroyTextures(false) was set.
		void SetImage(eMaterialTexture aType, Image* apImage);
		Image* GetImage(eMaterialTexture aType) const;

		// Storage slot for aType in the variant alternative currently held, or
		// nullptr if this material type has no such slot. Assign a
		// SharedResourceHandle<Image> to bind/own an image directly (this is what
		// SetImage does); callers loading textures use it to skip the SetImage hop.
		SharedResourceHandle<Image>* SlotForTexture(eMaterialTexture aType);

		// Per-material sampler state. Applied uniformly to all texture bindings
		// at draw time by the Forward+ shader's static samplers.
		inline eTextureWrap GetTextureWrap() const { return m_textureWrap; }
		inline eTextureFilter GetTextureFilter() const { return m_textureFilter; }
		inline float GetTextureAnisotropy() const { return m_textureAnisotropy; }
		void setTextureWrap(eTextureWrap aWrap);
		void setTextureFilter(eTextureFilter aFilter);
		void SetTextureAnisotropy(float afX);

		cResourceVarsObject* GetVarsObject();
		void LoadVariablesFromVarsObject(cResourceVarsObject* apVarsObject);

		void SetAutoDestroyTextures(bool abX){ mbAutoDestroyTextures = abX;}

		void SetBlendMode(eMaterialBlendMode aBlendMode);
		void SetDepthTest(bool abDepthTest);

		// Derived from the MaterialData blob (no stored flag): translucent uses its
		// authored m_refraction, water always refracts; water reflects when authored
		// and no cubemap is bound. The global refraction toggle is treated as on.
		bool HasRefraction() const;
		bool HasWorldReflection() const;

		void  SetWorldReflectionOcclusionTest(bool abX){ mbWorldReflectionOcclusionTest=abX;}
		bool  GetWorldReflectionOcclusionTest(){ return mbWorldReflectionOcclusionTest;}

		void SetLargeTransperantSurface(bool abX){ mbLargeTransperantSurface = abX;}
		bool GetLargeTransperantSurface(){ return mbLargeTransperantSurface;}

		// SolidDiffuse-only var, read from the variant blob; used by the material
		// editor UI and the RT path. False for every other material type.
		bool GetUseAlphaDissolveFilter() const;

		inline eMaterialBlendMode GetBlendMode() const { return mBlendMode; }
		// Derived: SolidDiffuse with a bound alpha texture is Trans, else Solid.
		eMaterialAlphaMode GetAlphaMode() const;
		inline bool GetDepthTest() const { return mbDepthTest; }

		void SetPhysicsMaterial(const tString & asPhysicsMaterial){ msPhysicsMaterial = asPhysicsMaterial;}
		const tString& GetPhysicsMaterial(){ return msPhysicsMaterial;}

		void UpdateBeforeRendering(float afTimeStep);

		inline int GetRenderFrameCount() const { return mlRenderFrameCount;}
		inline void SetRenderFrameCount(const int alCount) { mlRenderFrameCount = alCount;}

		//Animation
		void AddUvAnimation(eMaterialUvAnimation aType, float afSpeed, float afAmp, eMaterialAnimationAxis aAxis);
		bool HasUvAnimation() const { return !mvUvAnimations.empty(); }
		int GetUvAnimationNum(){ return (int)mvUvAnimations.size();}
		cMaterialUvAnimation *GetUvAnimation(int alIdx){ return &mvUvAnimations[alIdx]; }
		inline const cMatrixf& GetUvMatrix() const { return m_mtxUV;}
		void ClearUvAnimations();

		//resources stuff.
		bool Reload(){ return false;}
		void Unload(){}
		void Destroy(){}

	private:
		void UpdateAnimations(float afTimeStep);

		cGraphics *mpGraphics;
		cResources *mpResources;

		iMaterialType *mpType;

		bool mbAutoDestroyTextures;

		eMaterialBlendMode mBlendMode;
		bool mbDepthTest;

		bool mbWorldReflectionOcclusionTest;

		bool mbLargeTransperantSurface;

		// Per-type material data blob (textures + scalar params).
		MaterialData m_data;

		// Per-material sampler state.
		eTextureWrap m_textureWrap = eTextureWrap_Repeat;
		eTextureFilter m_textureFilter = eTextureFilter_Bilinear;
		float m_textureAnisotropy = 1.0f;

		std::vector<cMaterialUvAnimation> mvUvAnimations;
		cMatrixf m_mtxUV;
		float mfAnimTime;

		int mlRenderFrameCount;

		tString msPhysicsMaterial;

		uint32_t m_generation = 0;
	};

	//---------------------------------------------------

};
#endif // HPL_MATERIAL_H
