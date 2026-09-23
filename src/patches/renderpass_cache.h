#pragma once
#include <atomic>
#include <cstring>
#include <new>
#include <mimalloc.h>
#include "render_pass_retirement.h"

// RenderPass replacement adapted from aers/EngineFixesSkyrim64 (MIT), with
// alandtse's deferred-free policy. Installed before game render pools exist.
namespace Patches::RenderPassCache
{
    inline RetirementQueue<RE::BSRenderPass> retired;
    inline std::atomic<std::uint64_t> allocations{ 0 };
    inline bool installed = false;

    inline void* AllocateBytes(std::size_t bytes)
    {
        auto* p = mi_zalloc_aligned(bytes, 16);
        if (!p) throw std::bad_alloc();
        return p;
    }

    inline void SetLights(RE::BSRenderPass* pass, std::uint8_t count, RE::BSLight** lights)
    {
        if (count != pass->numLights) {
            mi_free(pass->sceneLights);
            pass->sceneLights = count ? static_cast<RE::BSLight**>(AllocateBytes(sizeof(RE::BSLight*) * count)) : nullptr;
            pass->numLights = count;
        }
        for (std::uint32_t i = 0; i < count; ++i) pass->sceneLights[i] = lights[i];
    }

    inline void Set(RE::BSRenderPass* pass, RE::BSShader* shader, RE::BSShaderProperty* property,
        RE::BSGeometry* geometry, std::uint32_t technique, std::uint8_t count, RE::BSLight** lights)
    {
        pass->shader = shader;
        pass->shaderProperty = property;
        pass->geometry = geometry;
        pass->passEnum = technique;
        pass->accumulationHint = 0;
        SetLights(pass, count, lights);
    }

    inline RE::BSRenderPass* Allocate(RE::BSShader* shader, RE::BSShaderProperty* property,
        RE::BSGeometry* geometry, std::uint32_t technique, std::uint8_t count, RE::BSLight** lights)
    {
        auto* pass = static_cast<RE::BSRenderPass*>(AllocateBytes(sizeof(RE::BSRenderPass)));
        pass->LODMode.index = 3;
        pass->cachePoolId = 0xFEFEDEAD;
        Set(pass, shader, property, geometry, technique, count, lights);
        allocations.fetch_add(1, std::memory_order_relaxed);
        return pass;
    }

    inline void Deallocate(RE::BSRenderPass* pass)
    {
        retired.Retire(pass, [] {
            const auto* state = RE::BSGraphics::State::GetSingleton();
            return state ? state->frameCount : 0u;
        }, [](RE::BSRenderPass* old) {
            mi_free(old->sceneLights);
            mi_free(old);
        });
    }

    inline void LogStats()
    {
        if (!installed) return;
        const auto s = retired.Snapshot();
        logger::info("RenderPassCache v1: allocations={} retired={} freed={} pending={} peak={} duplicateRetires={}",
            allocations.load(std::memory_order_relaxed), s.retired, s.freed, s.pending, s.peak, s.duplicateRetires);
    }

    inline void Install()
    {
        if (!Settings::Memory::bOverrideRenderPassCache.GetValue()) return;
        // The captured deadlock and entry lengths are verified for this build.
        if (REL::Module::get().version() != SKSE::RUNTIME_SSE_1_5_97) {
            logger::error("RenderPassCache replacement supports SE 1.5.97 only");
            return;
        }
        REL::Relocation allocate{ REL::ID(100717) };
        REL::Relocation deallocate{ REL::ID(100718) };
        REL::Relocation setlights{ REL::ID(100711) };
        REL::Relocation set{ REL::ID(100710) };
        REL::Relocation init{ REL::ID(100720) };
        REL::Relocation kill{ REL::ID(100721) };
        REL::Relocation clear{ REL::ID(100722) };
        allocate.replace_func(0x9A, Allocate);
        deallocate.replace_func(0x60, Deallocate);
        setlights.replace_func(0x69, SetLights);
        set.replace_func(0x90, Set);
        init.write_fill(REL::INT3, 0x1BD); init.write(REL::RET);
        kill.write_fill(REL::INT3, 0xAB); kill.write(REL::RET);
        clear.write_fill(REL::INT3, 0xE7); clear.write(REL::RET);
        installed = true;
        logger::info("installed RenderPassCache v1: mimalloc; 3-frame retirement; no capacity-forced free; Allocate={:X} Deallocate={:X} Clear={:X}",
            allocate.address(), deallocate.address(), clear.address());
    }
}
