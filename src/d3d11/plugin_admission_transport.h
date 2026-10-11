#pragma once

#include "../common/plugin_selection_admission.h"

#include <cstdint>
#include <string>

namespace edvr::plugin_admission_transport {

inline constexpr std::uint32_t kQueryVersionV1 =
    plugin_selection::admission_v1::kQueryVersionV1;
using QueryKindV1 = plugin_selection::admission_v1::QueryKindV1;

// Called once from the graphics INIT_ONCE callback after the bounded source
// snapshot has been captured. Native commitment is attempted only for a
// fully admitted VR candidate. Flat remains a graphics-only route.
void publishCandidate(const std::wstring& moduleDirectory, HMODULE graphicsModule,
                      plugin_selection::PluginSelectionProfileV1 activeProfile) noexcept;

} // namespace edvr::plugin_admission_transport

extern "C" __declspec(dllexport) HRESULT WINAPI edvrQueryPluginAdmissionV1(
    std::uint32_t version, std::uint32_t kind, std::uint32_t size,
    void* output) noexcept;
