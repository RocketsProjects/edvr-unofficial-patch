HRESULT STDMETHODCALLTYPE hookedCreateSamplerState(ID3D11Device* self,
                                                   const D3D11_SAMPLER_DESC* desc,
                                                   ID3D11SamplerState** out) {
    if (!g_state || !g_state->realCreateSamplerState) {
        return E_FAIL;
    }
    State& s = *g_state;
    if (!desc || self != s.device) return s.realCreateSamplerState(self, desc, out);
    D3D11_SAMPLER_DESC d = *desc;
    guardedBudget(g_createBudget, [&] {
        const uint32_t filter = static_cast<uint32_t>(desc->Filter);
        ++s.samplerCreates;
        if (!s.samplerFirstMs) s.samplerFirstMs = GetTickCount64();
        bool found = false;
        for (uint32_t i = 0; i < s.samplerShapeCount; ++i) {
            State::SamplerShape& sh = s.samplerShapes[i];
            if (sh.filter == filter && sh.aniso == desc->MaxAnisotropy &&
                sh.bias == desc->MipLODBias) {
                ++sh.count;
                found = true;
                break;
            }
        }
        if (!found) {
            const uint32_t n = sizeof(s.samplerShapes) / sizeof(s.samplerShapes[0]);
            if (s.samplerShapeCount < n) {
                State::SamplerShape& sh = s.samplerShapes[s.samplerShapeCount++];
                sh.filter = filter;
                sh.aniso = desc->MaxAnisotropy;
                sh.bias = desc->MipLODBias;
                sh.count = 1;
            } else {
                ++s.samplerOther;
            }
        }
        // The Performance callback is absent-safe. The host constructs a
        // fixed-width copy only when the selected policy is present; the
        // original descriptor and its one real COM call stay in core.
        if (s.samplerSettings.rewriteEnabled && s.samplerPolicy &&
            s.samplerPolicy->structSize ==
                                   sizeof(*s.samplerPolicy) &&
            s.samplerPolicy->version ==
                plugins::performance::kTextureSamplerPolicyOpsVersion1 &&
            s.samplerPolicy->rewrite) {
            plugins::performance::TextureSamplerDescV1 input{};
            input.filter = filter;
            input.addressU = static_cast<uint32_t>(desc->AddressU);
            input.addressV = static_cast<uint32_t>(desc->AddressV);
            input.addressW = static_cast<uint32_t>(desc->AddressW);
            input.mipLodBias = desc->MipLODBias;
            input.maxAnisotropy = desc->MaxAnisotropy;
            input.comparisonFunc = static_cast<uint32_t>(desc->ComparisonFunc);
            for (size_t i = 0; i < 4; ++i) input.borderColor[i] = desc->BorderColor[i];
            input.minLod = desc->MinLOD;
            input.maxLod = desc->MaxLOD;
            plugins::performance::TextureSamplerDescV1 output{};
            const bool rewritten = s.samplerPolicy->rewrite(
                s.samplerPolicy->context, &s.samplerSettings, &input, &output);
            if (rewritten) {
                d.Filter = static_cast<D3D11_FILTER>(output.filter);
                d.MaxAnisotropy = output.maxAnisotropy;
                d.MipLODBias = output.mipLodBias;
            }
            if (rewritten && !s.samplerForceNoted) {
                s.samplerForceNoted = true;
                Log::get().note(
                    "texture filtering: the game's linear and anisotropic samplers are "
                    "being created with anisotropy %d and %+.2f added to their mip bias "
                    "(%s). Point, comparison, minimum and maximum filters are left alone. A "
                    "positive bias trades sharpness for quiet on distant repeating "
                    "detail; a negative one does the reverse.",
                    s.samplerSettings.anisotropy,
                    static_cast<double>(s.samplerSettings.bias),
                    s.samplerSettings.biasWhy[0] ? s.samplerSettings.biasWhy :
                                                   "no bias asked for");
            }
        }
        // Once the creates have settled: a renderer makes its samplers up
        // front, so ten seconds after the first is past the burst. Printed
        // again only if a shape appears that the first line did not carry.
        const bool settled = GetTickCount64() - s.samplerFirstMs > 10000;
        if (settled && (s.samplerCensusPrints == 0 ||
                        (s.samplerCensusPrints < 3 &&
                         s.samplerShapeCount > s.samplerPrintedShapes))) {
            samplerCensusNote(s);
        }
    });
    return s.realCreateSamplerState(self, &d, out);
}
