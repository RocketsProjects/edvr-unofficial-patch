#pragma once

#include "../common/plugin_selection_snapshot.h"
#include <cstddef>
#include <cstdint>
#include <string>

namespace edvr::plugin_selection_transport {

constexpr std::size_t kMaxRuntimeStateBytes = 1024u * 1024u;

// Pure boundary between Win32 file acquisition and the shared codec. Tests
// pass synthetic read outcomes here; no path or live install state is read.
HRESULT decodeStateFileReadResultV1(
    HRESULT readStatus, bool missing, const char* bytes, std::size_t byteCount,
    plugin_selection::PluginSelectionProfileV1 installedProfile,
    plugin_selection::PluginSelectionSnapshotV1* out) noexcept;

HRESULT captureFromModuleDirectory(const std::wstring& moduleDirectory) noexcept;

}  // namespace edvr::plugin_selection_transport

extern "C" __declspec(dllexport) HRESULT WINAPI edvrQueryPluginSelectionV1(
    std::uint32_t version, std::uint32_t size, void* output) noexcept;