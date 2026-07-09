#pragma once

#include <cassert>

namespace hpl {

    // Zero-overhead typed singleton accessor. Each Interface<T> instantiation
    // gets its own inline static — resolved at compile time, compiles to a
    // single pointer load. Safe for statically-linked builds (one copy per
    // process). Register during init, UnRegister during shutdown.
    template<typename T>
    class Interface {
        inline static T* s_instance = nullptr;
    public:
        static void Register(T* instance) {
            assert(instance != nullptr && "registering null instance");
            assert(s_instance == nullptr && "Interface already registered");
            s_instance = instance;
        }
        static void UnRegister(T* instance) {
            assert(s_instance == instance && "unregistering wrong instance");
            s_instance = nullptr;
        }
        static T* Get() { return s_instance; }
    };

} // namespace hpl
