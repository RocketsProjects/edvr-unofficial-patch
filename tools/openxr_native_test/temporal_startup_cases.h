namespace edvr::openxr::test {
template<class Check> void runTemporalStartupCases(Check check) {
  using namespace treatment_fixture;
  const auto provider=GetModuleHandleW(nullptr);
  const auto fakeDevice=reinterpret_cast<ID3D11Device*>(static_cast<uintptr_t>(1));
  const auto scenario=[&](const char* label,State::TemporalAcquireMode temporalMode,
      State::SharpenAcquireMode sharpenMode,HMODULE selectedProvider,bool expected,
      bool temporalAcquired,bool sharpenAcquired,uint64_t expectedCalls) {
    state={};state.thread=GetCurrentThreadId();
    state.temporalAcquireMode=temporalMode;state.sharpenAcquireMode=sharpenMode;
    launch_fixture::Fixture fixture;auto& host=fixture.host;
    check(fixture.initialized,label);
    const bool services=fixture.route.bind()&&fixture.owner.start();
    check(services,"startup provider fixture binds graphics and owner services");
    bool accepted=false;
    const bool invoked=services&&fixture.route.invoke([&]{
      accepted=host.acquireStartupTreatmentProviders(selectedProvider,fakeDevice,host.runtimeGeneration);
    });
    check(invoked,"startup production acquisition seam completes through owner and graphics rendezvous");
    check(accepted==expected,"startup acceptance matches the provider acquisition result");
    check(host.menu.acquired()==(selectedProvider==provider&&expectedCalls>0),
      "mandatory Core menu is acquired before optional TAA providers");
    check(host.temporal.acquired()==temporalAcquired&&host.sharpen.acquired()==sharpenAcquired,
      "startup retains only admitted optional TAA tables");
    check(host.graphicsCalls.calls==expectedCalls&&host.graphicsCalls.thread==state.thread&&
      host.graphicsCalls.wrongThread==0&&host.graphicsCalls.rejected==0,
      "startup provider calls retain the graphics-thread rendezvous");
    if(services)check(fixture.owner.stop(),"startup provider owner stops cleanly");
    fixture.dispatcher.close();host.temporal.close();host.sharpen.close();host.menu.close();
  };

  scenario("startup accepts admitted temporal and sharpen tables",State::TemporalAcquireMode::admitted,
      State::SharpenAcquireMode::admitted,provider,true,true,true,3);
  scenario("startup accepts exact S_FALSE absence for both optional TAA providers",
      State::TemporalAcquireMode::ownerAbsent,State::SharpenAcquireMode::ownerAbsent,
      provider,true,false,false,3);
  scenario("startup continues to sharpen when temporal is absent",State::TemporalAcquireMode::ownerAbsent,
      State::SharpenAcquireMode::admitted,provider,true,false,true,3);
  scenario("startup continues after temporal when sharpen is absent",State::TemporalAcquireMode::admitted,
      State::SharpenAcquireMode::ownerAbsent,provider,true,true,false,3);
  for(const auto mode:{State::TemporalAcquireMode::malformedAbsent,State::TemporalAcquireMode::failure,
      State::TemporalAcquireMode::pending,State::TemporalAcquireMode::otherPositive})
    scenario("startup rejects malformed absence, failure, E_PENDING, and non-S_OK positive temporal results",
      mode,State::SharpenAcquireMode::admitted,provider,false,false,false,2);
  for(const auto mode:{State::SharpenAcquireMode::malformedAbsent,State::SharpenAcquireMode::failure,
      State::SharpenAcquireMode::pending,State::SharpenAcquireMode::otherPositive})
    scenario("startup rejects malformed absence, failure, E_PENDING, and non-S_OK positive sharpen results",
      State::TemporalAcquireMode::admitted,mode,provider,false,true,false,3);
  scenario("startup fails when the mandatory Core menu provider is missing",
      State::TemporalAcquireMode::admitted,State::SharpenAcquireMode::admitted,
      GetModuleHandleW(L"kernel32.dll"),false,false,false,1);
}
} // namespace edvr::openxr::test
