#pragma once

#include "../common/plugin_admission_commit.h"

namespace edvr::openxr::native_plugin_admission {

bool readCommitted(edvr::plugin_selection::commit_v1::AdmissionCommitRequestV1* output) noexcept;

} // namespace edvr::openxr::native_plugin_admission
