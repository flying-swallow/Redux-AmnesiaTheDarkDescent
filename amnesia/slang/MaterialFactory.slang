#pragma once

// MaterialFactory — Falcor MaterialSystem.getMaterial analogue. Reads the
// fixed-size MaterialDataBlob at `materialID`, dispatches on its type, and
// reinterprets it into the concrete material struct, returned boxed as IMaterial
// (Slang existential dispatch, mirroring Scene.getLight / ILight).
//
// Falcor uses createDynamicObject<IMaterial, MaterialDataBlob>(int(type), blob),
// which requires host-side type-conformance registration (MaterialSystem::
// getTypeConformances). Our CLI slangc build has no such registration, so we
// dispatch with an explicit branch instead — functionally identical, and the
// reinterpret<T> blob cast is the same idiom Falcor's concrete materials use.

#include "HostDefinitions.h"
import SceneTypes;       // MaterialDataBlob / MaterialHeader / concrete structs
import bindless;        // gMaterials
import IMaterial;       // IMaterial interface + per-struct conformances

// Resolve a materialID to its polymorphic IMaterial view.
public IMaterial getMaterial(uint materialID)
{
    MaterialDataBlob blob = gMaterials[materialID];
    uint type = reinterpret<MaterialHeader, MaterialDataBlob>(blob).type;

    if (type == MATERIAL_TYPE_TRANSLUCENT)
        return reinterpret<TranslucentMaterial, MaterialDataBlob>(blob);
    if (type == MATERIAL_TYPE_WATER)
        return reinterpret<WaterMaterial, MaterialDataBlob>(blob);
    // MATERIAL_TYPE_DIFFUSE (and decals, which share the diffuse layout)
    return reinterpret<DiffuseMaterial, MaterialDataBlob>(blob);
}
