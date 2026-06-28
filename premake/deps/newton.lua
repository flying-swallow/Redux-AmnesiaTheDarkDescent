-- Newton (physics) -- explicit source list mirrors extern/CMakeLists.txt.
project "Newton"
    kind "StaticLib"
    language "C++"
    set_output("static")

    local nd = DEPS_SOURCES .. "/Newton/"
    local core = {
        "dg", "dgAABBPolygonSoup", "dgConvexHull3d", "dgConvexHull4d", "dgCRC",
        "dgDebug", "dgDelaunayTetrahedralization", "dgGeneralMatrix",
        "dgGeneralVector", "dgGoogol", "dgIntersections", "dgMatrix", "dgMemory",
        "dgNode", "dgPolygonSoupBuilder", "dgPolyhedra", "dgPolyhedraMassProperties",
        "dgQuaternion", "dgRandom", "dgRef", "dgRefCounter", "dgSmallDeterminant",
        "dgSPDMatrix", "dgSphere", "dgThreads", "dgTree", "dgTypes",
    }
    local physics = {
        "dgBallConstraint", "dgBilateralConstraint", "dgBody", "dgBodyMasterList",
        "dgBroadPhaseCollision", "dgCollision", "dgCollisionBox", "dgCollisionBVH",
        "dgCollisionCapsule", "dgCollisionChamferCylinder", "dgCollisionCompound",
        "dgCollisionCompoundBreakable", "dgCollisionCone", "dgCollisionConvex",
        "dgCollisionConvexHull", "dgCollisionConvexModifier", "dgCollisionCylinder",
        "dgCollisionEllipse", "dgCollisionHeightField", "dgCollisionMesh",
        "dgCollisionNull", "dgCollisionScene", "dgCollisionSphere",
        "dgCollisionUserMesh", "dgConnectorConstraint", "dgConstraint", "dgContact",
        "dgCorkscrewConstraint", "dgHingeConstraint", "dgMeshEffect", "dgMeshEffect2",
        "dgMinkowskiConv", "dgNarrowPhaseCollision", "dgPointToCurveConstraint",
        "dgSlidingConstraint", "dgUniversalConstraint", "dgUpVectorConstraint",
        "dgUserConstraint", "dgWorld", "dgWorldDynamicUpdate",
    }
    for _, n in ipairs(core)    do files { nd .. "core/"    .. n .. ".cpp" } end
    for _, n in ipairs(physics) do files { nd .. "physics/" .. n .. ".cpp" } end
    files { nd .. "newton/Newton.cpp", nd .. "newton/NewtonClass.cpp" }

    includedirs {
        nd,                                 -- public Newton.h
        nd .. "core", nd .. "physics", nd .. "newton",
    }

    filter "system:windows"
        defines { "_NEWTON_USE_LIB=1", "_WIN_64_VER" }
        disablewarnings { "4312" }
    filter "system:linux"
        defines { "_LINUX_VER", "_LINUX_VER_64" }
    filter "toolset:gcc or clang"
        buildoptions { "-Wno-extern-c-compat" }
    filter {}
