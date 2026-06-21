#pragma once

// Slang-side IMaterial interface over the host/device-shared material structs in
// SceneTypes.slang — the Falcor MaterialSystem `IMaterial` analogue, trimmed to
// the queries this renderer's shared paths need (it is NOT Falcor's IBSDF/
// IMaterialInstance eval/sample layer; Amnesia keeps its demodulated-diffuse +
// surfel-GI shading). Conformances are added via `extension` so the shared header
// stays a pure data-layout file consumed by both C++ and shader.
//
// Each material struct stores its discriminator in `type` at offset 0 (the
// contract documented in SceneTypes.slang). MaterialFactory.getMaterial reads
// that type and reinterprets the MaterialDataBlob into the concrete struct, then
// hands it back boxed as IMaterial (Slang existential dispatch, like ILight).

#include "HostDefinitions.h"
import SceneTypes;            // material struct fields + kInvalidTextureIndex sentinel
#include "Constants.h"        // kMaterialFlag* bits

// -----------------------------------------------------------------------------
// IMaterial — common, type-erased view of a material. getType() returns the
// MATERIAL_TYPE_* discriminator; getConfig() the shared kMaterialFlag* word.
// getTexture(slot) indexes the material's tex[] table (0..7); getCubeMap()
// returns the reflection cubemap slot (or kInvalidTextureIndex). isReflective /
// isRefractive / getRefractionScale expose the cross-pass material queries.
// -----------------------------------------------------------------------------
public interface IMaterial
{
    public uint  getType();
    public uint  getConfig();
    public uint  getTexture(uint slot);
    public uint  getCubeMap();
    public bool  isReflective();      // carries a reflection cubemap
    public bool  isRefractive();      // glass HasRefraction flag
    public float getRefractionScale();
};

public extension DiffuseMaterial : IMaterial
{
    public uint  getType()        { return type; }
    public uint  getConfig()      { return materialConfig; }
    public uint  getTexture(uint slot) { return tex[slot]; }
    public uint  getCubeMap()     { return cubeMapTextureIndex; }
    public bool  isReflective()   { return cubeMapTextureIndex != kInvalidTextureIndex; }
    public bool  isRefractive()   { return false; }
    public float getRefractionScale() { return 0.0f; }
};

public extension TranslucentMaterial : IMaterial
{
    public uint  getType()        { return type; }
    public uint  getConfig()      { return materialConfig; }
    public uint  getTexture(uint slot) { return tex[slot]; }
    public uint  getCubeMap()     { return cubeMapTextureIndex; }
    public bool  isReflective()   { return cubeMapTextureIndex != kInvalidTextureIndex; }
    public bool  isRefractive()   { return (materialConfig & kMaterialFlagHasRefraction) != 0u; }
    public float getRefractionScale() { return refractionScale; }
};

public extension WaterMaterial : IMaterial
{
    public uint  getType()        { return type; }
    public uint  getConfig()      { return materialConfig; }
    public uint  getTexture(uint slot) { return tex[slot]; }
    public uint  getCubeMap()     { return kInvalidTextureIndex; }  // water has no reflection cubemap slot
    public bool  isReflective()   { return false; }
    public bool  isRefractive()   { return false; }                 // water refraction handled by the water branch
    public float getRefractionScale() { return refractionScale; }
};
