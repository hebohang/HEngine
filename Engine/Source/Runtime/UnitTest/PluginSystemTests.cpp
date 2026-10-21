#include "Hepch.h"

#include "Runtime/UnitTest/PluginSystemTests.h"

#include "Runtime/Core/Plugins/PluginManager.h"

#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace HEngine::Tests
{
    namespace
    {
        void Require(bool condition, std::string_view message)
        {
            if (!condition)
                throw std::runtime_error(std::string(message));
        }

        template<typename Exception, typename Callable>
        void RequireThrows(Callable&& callable, std::string_view message)
        {
            bool bCaughtExpectedException = false;
            try
            {
                std::forward<Callable>(callable)();
            }
            catch (const Exception&)
            {
                bCaughtExpectedException = true;
            }
            catch (...)
            {
            }
            Require(bCaughtExpectedException, message);
        }

        struct TestService
        {
            int Value = 0;
        };

        struct OtherService
        {
            int Value = 0;
        };

        struct PluginBehavior
        {
            bool bFailStart = false;
            bool bFailStop = false;
            bool bPublishService = false;
        };

        struct PluginFixture
        {
            std::vector<std::string> Events;
            std::unordered_map<std::string, PluginBehavior> Behaviors;
            std::unordered_map<std::string, uint32_t> DestroyedModules;
            PluginManager* Manager = nullptr;
            std::vector<std::pair<std::string, PluginState>> ObservedStates;
        };

        PluginFixture* sFixture = nullptr;

        class FixtureScope final
        {
        public:
            explicit FixtureScope(PluginFixture& fixture)
            {
                if (sFixture)
                    throw std::logic_error("Plugin fixture is already active");
                sFixture = &fixture;
            }

            ~FixtureScope()
            {
                sFixture = nullptr;
            }

            FixtureScope(const FixtureScope&) = delete;
            FixtureScope& operator=(const FixtureScope&) = delete;
        };

        class TestPluginModule final : public IPluginModule
        {
        public:
            explicit TestPluginModule(std::string id)
                : mId(std::move(id))
            {
            }

            ~TestPluginModule() override
            {
                if (sFixture)
                    ++sFixture->DestroyedModules[mId];
            }

            void Start(PluginContext& context) override
            {
                Require(sFixture != nullptr, "Plugin anchor has no active fixture");
                Require(context.GetDescriptor().Id == mId,
                    "PluginContext exposed the wrong descriptor");
                if (sFixture->Manager)
                {
                    sFixture->ObservedStates.emplace_back(
                        "start:" + mId,
                        sFixture->Manager->GetStatus(mId).State);
                }
                sFixture->Events.push_back("start:" + mId);
                const PluginBehavior behavior = sFixture->Behaviors[mId];
                if (behavior.bPublishService)
                {
                    mService = context.RegisterService<TestService>(
                        "service." + mId,
                        CreateRef<TestService>(TestService{ .Value = 17 }));
                }
                if (behavior.bFailStart)
                    throw std::runtime_error("expected start failure: " + mId);
            }

            void Stop(PluginContext& context) override
            {
                Require(sFixture != nullptr, "Plugin Stop has no active fixture");
                Require(context.GetEnvironment().Host == PluginHost::Tests,
                    "PluginContext exposed the wrong host");
                sFixture->Events.push_back("stop:" + mId);
                if (sFixture->Behaviors[mId].bFailStop)
                    throw std::runtime_error("expected stop failure: " + mId);
                mService.Revoke();
            }

        private:
            std::string mId;
            PluginServiceHandle mService;
        };

        [[nodiscard]] Scope<IPluginModule> CreateFixtureModule(std::string id)
        {
            if (sFixture && sFixture->Manager)
            {
                sFixture->ObservedStates.emplace_back(
                    "factory:" + id,
                    sFixture->Manager->GetStatus(id).State);
            }
            return CreateScope<TestPluginModule>(std::move(id));
        }

        [[nodiscard]] Scope<IPluginModule> AlphaAnchor()
        {
            return CreateFixtureModule("alpha");
        }

        [[nodiscard]] Scope<IPluginModule> BetaAnchor()
        {
            return CreateFixtureModule("beta");
        }

        [[nodiscard]] Scope<IPluginModule> GammaAnchor()
        {
            return CreateFixtureModule("gamma");
        }

        [[nodiscard]] PluginHostEnvironment MakeEnvironment(
            PluginBuildDistribution distribution =
                PluginBuildDistribution::Development)
        {
            return {
                .Host = PluginHost::Tests,
                .BuildDistribution = distribution,
                .Platform = PluginPlatform::Windows,
                .Configuration = PluginConfiguration::Debug,
                .ApiVersion = { 1, 2, 0 },
            };
        }

        [[nodiscard]] PluginDependency Dependency(
            std::string id,
            PluginVersion minimum = { 1, 0, 0 },
            std::optional<PluginVersion> maximum = std::nullopt,
            PluginApiVersion minimumApi = { 1, 0, 0 })
        {
            return {
                .Id = std::move(id),
                .VersionRange = {
                    .MinimumInclusive = minimum,
                    .MaximumExclusive = maximum,
                },
                .MinimumApiVersion = minimumApi,
            };
        }

        [[nodiscard]] PluginDescriptor MakeDescriptor(
            std::string id,
            PluginLoadPhase phase = PluginLoadPhase::PostDevice,
            std::vector<PluginDependency> dependencies = {})
        {
            return {
                .Id = id,
                .DisplayName = "Test " + id,
                .ModuleName = "PluginSystemTestModule",
                .EntrySymbol = "CreatePluginSystemTestModule",
                .Version = { 1, 0, 0 },
                .ApiVersion = { 1, 0, 0 },
                .RequiredHostApiVersion = { 1, 0, 0 },
                .SupportedHosts = PluginHost::Tests,
                .Distribution = PluginDistribution::Runtime,
                .LoadPhase = phase,
                .PackagePolicy = PluginPackagePolicy::ProjectSelectable,
                .Platforms = PluginPlatform::Windows,
                .Configurations = PluginConfiguration::Debug |
                    PluginConfiguration::RelWithDebInfo |
                    PluginConfiguration::Release |
                    PluginConfiguration::MinSizeRel,
                .ShaderRoots = { "Shaders/" + id },
                .ContentRoots = { "Content/" + id },
                .Dependencies = std::move(dependencies),
            };
        }

        [[nodiscard]] StaticPluginRegistration Registration(
            PluginDescriptor descriptor,
            PluginModuleFactory factory)
        {
            return {
                .Descriptor = std::move(descriptor),
                .Factory = factory,
            };
        }

        void TestDescriptorAndApiValidation()
        {
            PluginManager manager(MakeEnvironment());
            PluginDescriptor invalidVersion = MakeDescriptor("invalid-version");
            invalidVersion.Version.Major = 0;
            RequireThrows<std::invalid_argument>(
                [&]()
                {
                    manager.Register(Registration(
                        std::move(invalidVersion),
                        &AlphaAnchor));
                },
                "PluginManager accepted an invalid semantic version");

            PluginDescriptor incompatibleApi = MakeDescriptor("incompatible-api");
            incompatibleApi.RequiredHostApiVersion = { 2, 0, 0 };
            RequireThrows<std::invalid_argument>(
                [&]()
                {
                    manager.Register(Registration(
                        std::move(incompatibleApi),
                        &AlphaAnchor));
                },
                "PluginManager accepted an incompatible API major");
            Require(manager.Size() == 0,
                "Rejected descriptors mutated the plugin registry");

            PluginManager shipping(MakeEnvironment(
                PluginBuildDistribution::Shipping));
            PluginDescriptor packageNever = MakeDescriptor("developer-only");
            packageNever.PackagePolicy = PluginPackagePolicy::Never;
            RequireThrows<std::invalid_argument>(
                [&]()
                {
                    shipping.Register(Registration(
                        std::move(packageNever),
                        &AlphaAnchor));
                },
                "Shipping host accepted a PackagePolicy Never plugin");

            PluginDescriptor developerOnly = MakeDescriptor("developer-distribution");
            developerOnly.Distribution = PluginDistribution::DeveloperOnly;
            developerOnly.PackagePolicy = PluginPackagePolicy::Never;
            RequireThrows<std::invalid_argument>(
                [&]()
                {
                    shipping.Register(Registration(
                        std::move(developerOnly),
                        &AlphaAnchor));
                },
                "Shipping host accepted a DeveloperOnly distribution");
        }

        void TestStableRegistrationAndCompleteUnregister()
        {
            PluginFixture fixture;
            FixtureScope fixtureScope(fixture);
            PluginManager manager(MakeEnvironment());
            manager.Register(Registration(MakeDescriptor("beta"), &BetaAnchor));
            manager.Register(Registration(MakeDescriptor("alpha"), &AlphaAnchor));
            const std::vector<PluginDescriptor> descriptors =
                manager.EnumerateDescriptors();
            Require(descriptors.size() == 2 &&
                    descriptors[0].Id == "beta" &&
                    descriptors[1].Id == "alpha" &&
                    manager.GetStatus("beta").State == PluginState::Registered &&
                    manager.GetStatus("alpha").State == PluginState::Registered,
                "Plugin registration order was not stable");
            RequireThrows<std::logic_error>(
                [&]()
                {
                    manager.Register(Registration(
                        MakeDescriptor("alpha"),
                        &AlphaAnchor));
                },
                "Duplicate plugin registration was accepted");

            manager.StartThroughPhase(PluginLoadPhase::PostDevice);
            RequireThrows<std::logic_error>(
                [&]() { (void)manager.Unregister("alpha"); },
                "An active plugin was unregistered");
            manager.StopAll();
            Require(manager.Unregister("alpha") &&
                    !manager.Contains("alpha") &&
                    !manager.Unregister("alpha") &&
                    manager.Size() == 1 &&
                    manager.EnumerateDescriptors()[0].Id == "beta",
                "Plugin unregister was incomplete or reordered survivors");
        }

        void TestMissingDependencyAndCycle()
        {
            PluginFixture fixture;
            FixtureScope fixtureScope(fixture);
            PluginManager missing(MakeEnvironment());
            missing.Register(Registration(
                MakeDescriptor(
                    "alpha",
                    PluginLoadPhase::PostDevice,
                    { Dependency("missing") }),
                &AlphaAnchor));
            RequireThrows<PluginLifecycleError>(
                [&]() { missing.StartThroughPhase(PluginLoadPhase::PostDevice); },
                "Missing plugin dependency was not rejected");
            Require(missing.ActiveCount() == 0 &&
                    missing.GetStatus("alpha").Diagnostic.find("Missing") !=
                        std::string::npos,
                "Missing dependency did not preserve a diagnostic");

            PluginManager cycle(MakeEnvironment());
            cycle.Register(Registration(
                MakeDescriptor(
                    "alpha",
                    PluginLoadPhase::PostDevice,
                    { Dependency("beta") }),
                &AlphaAnchor));
            cycle.Register(Registration(
                MakeDescriptor(
                    "beta",
                    PluginLoadPhase::PostDevice,
                    { Dependency("alpha") }),
                &BetaAnchor));
            RequireThrows<PluginLifecycleError>(
                [&]() { cycle.StartThroughPhase(PluginLoadPhase::PostDevice); },
                "Plugin dependency cycle was not rejected");
            Require(cycle.ActiveCount() == 0 && fixture.Events.empty(),
                "Cycle validation started a partial plugin prefix");

            PluginManager incompatibleVersion(MakeEnvironment());
            incompatibleVersion.Register(Registration(
                MakeDescriptor("alpha"),
                &AlphaAnchor));
            incompatibleVersion.Register(Registration(
                MakeDescriptor(
                    "beta",
                    PluginLoadPhase::PostDevice,
                    { Dependency("alpha", { 2, 0, 0 }) }),
                &BetaAnchor));
            RequireThrows<PluginLifecycleError>(
                [&]()
                {
                    incompatibleVersion.StartThroughPhase(
                        PluginLoadPhase::PostDevice);
                },
                "Dependency version range was not enforced");

            PluginManager incompatibleApi(MakeEnvironment());
            incompatibleApi.Register(Registration(
                MakeDescriptor("alpha"),
                &AlphaAnchor));
            incompatibleApi.Register(Registration(
                MakeDescriptor(
                    "beta",
                    PluginLoadPhase::PostDevice,
                    { Dependency(
                        "alpha",
                        { 1, 0, 0 },
                        std::nullopt,
                        { 1, 1, 0 }) }),
                &BetaAnchor));
            RequireThrows<PluginLifecycleError>(
                [&]()
                {
                    incompatibleApi.StartThroughPhase(
                        PluginLoadPhase::PostDevice);
                },
                "Dependency API requirement was not enforced");
        }

        void TestStableTopologyPhaseAndReverseStop()
        {
            PluginFixture fixture;
            FixtureScope fixtureScope(fixture);
            PluginManager manager(MakeEnvironment());
            fixture.Manager = &manager;
            PluginDescriptor gamma = MakeDescriptor(
                "gamma",
                PluginLoadPhase::PostEngine,
                { Dependency("alpha") });
            gamma.OptionalDependencies.push_back(
                Dependency("optional-not-present"));
            manager.Register(Registration(std::move(gamma), &GammaAnchor));
            manager.Register(Registration(
                MakeDescriptor("beta", PluginLoadPhase::PreDevice),
                &BetaAnchor));
            manager.Register(Registration(
                MakeDescriptor("alpha", PluginLoadPhase::PostDevice),
                &AlphaAnchor));

            manager.StartThroughPhase(PluginLoadPhase::PreDevice);
            manager.StartThroughPhase(PluginLoadPhase::PostDevice);
            manager.StartThroughPhase(PluginLoadPhase::PostEngine);
            manager.StartThroughPhase(PluginLoadPhase::PostEngine);
            Require(fixture.Events == std::vector<std::string>{
                    "start:beta",
                    "start:alpha",
                    "start:gamma",
                } && manager.ActiveCount() == 3 &&
                    manager.GetStatus("beta").State == PluginState::Started &&
                    manager.GetStatus("alpha").State == PluginState::Started &&
                    manager.GetStatus("gamma").State == PluginState::Started &&
                    fixture.ObservedStates ==
                        std::vector<std::pair<std::string, PluginState>>{
                            { "factory:beta", PluginState::Loading },
                            { "start:beta", PluginState::Loaded },
                            { "factory:alpha", PluginState::Loading },
                            { "start:alpha", PluginState::Loaded },
                            { "factory:gamma", PluginState::Loading },
                            { "start:gamma", PluginState::Loaded },
                        },
                "Phase startup was not stable, dependency-safe, or idempotent");

            manager.StopAll();
            manager.StopAll();
            Require(fixture.Events == std::vector<std::string>{
                    "start:beta",
                    "start:alpha",
                    "start:gamma",
                    "stop:gamma",
                    "stop:alpha",
                    "stop:beta",
                } && manager.ActiveCount() == 0 &&
                    manager.GetStatus("beta").State == PluginState::Unloaded &&
                    manager.GetStatus("alpha").State == PluginState::Unloaded &&
                    manager.GetStatus("gamma").State == PluginState::Unloaded,
                "Plugins did not stop once in reverse activation order");
        }

        void TestStartupRollback()
        {
            PluginFixture fixture;
            fixture.Behaviors["alpha"].bPublishService = true;
            fixture.Behaviors["beta"] = {
                .bFailStart = true,
                .bPublishService = true,
            };
            FixtureScope fixtureScope(fixture);
            PluginManager manager(MakeEnvironment());
            manager.Register(Registration(MakeDescriptor("alpha"), &AlphaAnchor));
            manager.Register(Registration(
                MakeDescriptor(
                    "beta",
                    PluginLoadPhase::PostDevice,
                    { Dependency("alpha") }),
                &BetaAnchor));
            RequireThrows<PluginLifecycleError>(
                [&]() { manager.StartThroughPhase(PluginLoadPhase::PostDevice); },
                "Plugin start failure did not propagate");
            Require(fixture.Events == std::vector<std::string>{
                    "start:alpha",
                    "start:beta",
                    "stop:beta",
                    "stop:alpha",
                } &&
                    manager.ActiveCount() == 0 &&
                    manager.GetStatus("alpha").State == PluginState::Unloaded &&
                    manager.GetStatus("beta").State == PluginState::Failed &&
                    manager.GetStatus("beta").Diagnostic.find(
                        "expected start failure") != std::string::npos &&
                    manager.GetServices().Size() == 0 &&
                    fixture.DestroyedModules["alpha"] == 1 &&
                    fixture.DestroyedModules["beta"] == 1,
                "Failed startup did not roll back current/prefix state and services");
            manager.StopAll();
        }

        void TestStopFailureStillStopsEverything()
        {
            PluginFixture fixture;
            fixture.Behaviors["beta"].bFailStop = true;
            FixtureScope fixtureScope(fixture);
            PluginManager manager(MakeEnvironment());
            manager.Register(Registration(MakeDescriptor("alpha"), &AlphaAnchor));
            manager.Register(Registration(
                MakeDescriptor(
                    "beta",
                    PluginLoadPhase::PostDevice,
                    { Dependency("alpha") }),
                &BetaAnchor));
            manager.StartThroughPhase(PluginLoadPhase::PostDevice);
            RequireThrows<PluginLifecycleError>(
                [&]() { manager.StopAll(); },
                "Plugin Stop failure was swallowed");
            Require(manager.ActiveCount() == 0 &&
                    manager.GetStatus("beta").State == PluginState::Failed &&
                    manager.GetStatus("alpha").State == PluginState::Unloaded &&
                    fixture.Events == std::vector<std::string>{
                        "start:alpha",
                        "start:beta",
                        "stop:beta",
                        "stop:alpha",
                    },
                "A Stop failure skipped reverse cleanup of another plugin");
            manager.StopAll();
        }

        void TestServiceHandlesAndOwnerRevocation()
        {
            PluginServiceRegistry registry;
            const Ref<TestService> service =
                CreateRef<TestService>(TestService{ .Value = 9 });
            PluginServiceHandle handle = registry.RegisterErased(
                "test.service",
                "owner",
                std::type_index(typeid(TestService)),
                service);
            Require(handle.IsActive() && registry.Size() == 1 &&
                    registry.TryGet<TestService>("test.service") == service &&
                    registry.Find("test.service").As<TestService>() == service &&
                    !registry.TryGet<OtherService>("test.service"),
                "Typed/type-erased service lookup lost type identity");
            RequireThrows<std::logic_error>(
                [&]()
                {
                    (void)registry.Register<TestService>(
                        "test.service",
                        "owner",
                        service);
                },
                "Duplicate service registration was accepted");

            PluginServiceHandle moved = std::move(handle);
            Require(!handle.IsActive() && moved.IsActive(),
                "Moving a service handle revoked or duplicated its token");
            moved.Revoke();
            moved.Revoke();
            Require(!registry.Contains("test.service") && registry.Size() == 0,
                "RAII service revocation was incomplete or non-idempotent");

            PluginServiceHandle ownerHandle = registry.Register<TestService>(
                "test.owner-service",
                "owner",
                service);
            Require(registry.RevokeOwner("owner") == 1 &&
                    !ownerHandle.IsActive() && registry.Size() == 0,
                "Owner service revocation did not invalidate outstanding handles");

            PluginFixture fixture;
            fixture.Behaviors["alpha"].bPublishService = true;
            FixtureScope fixtureScope(fixture);
            PluginManager manager(MakeEnvironment());
            manager.Register(Registration(MakeDescriptor("alpha"), &AlphaAnchor));
            manager.StartThroughPhase(PluginLoadPhase::PostDevice);
            Require(manager.GetServices().TryGet<TestService>("service.alpha") !=
                    nullptr,
                "PluginContext did not publish a typed service");
            manager.StopAll();
            Require(!manager.GetServices().Contains("service.alpha"),
                "Stopping a plugin did not revoke its owned service");
        }
    }

    void PluginSystemContract()
    {
        TestDescriptorAndApiValidation();
        TestStableRegistrationAndCompleteUnregister();
        TestMissingDependencyAndCycle();
        TestStableTopologyPhaseAndReverseStop();
        TestStartupRollback();
        TestStopFailureStillStopsEverything();
        TestServiceHandlesAndOwnerRevocation();
    }
}
