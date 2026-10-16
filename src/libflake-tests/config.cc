#include <gtest/gtest.h>

#include <algorithm>

#include "nix/flake/flake.hh"
#include "nix/flake/settings.hh"
#include "nix/util/config-global.hh"
#include "nix/util/environment-variables.hh"
#include "nix/util/file-system.hh"
#include "nix/util/finally.hh"
#include "nix/util/logging.hh"
#include "nix/util/tests/experimental-features.hh"

namespace nix {

/* A list setting registered with `globalConfig`, which is what
   `ConfigFile::apply` sets. Registrations cannot be undone, so it is
   registered once and reset by each test. */
static Setting<Strings> & flakeMergeTestSetting()
{
    static Config config;
    static Setting<Strings> setting{&config, {}, "flake-merge-test-list", "description"};
    static GlobalConfig::Register r(&config);
    setting.assign({});
    return setting;
}

static void applyFlakeConfig()
{
    flake::ConfigFile configFile;
    configFile.settings.emplace("flake-merge-test-list", std::vector<std::string>{"a"});
    configFile.settings.emplace("extra-flake-merge-test-list", std::vector<std::string>{"b"});
    flake::Settings flakeSettings;
    flakeSettings.acceptFlakeConfig = true;
    configFile.apply(flakeSettings);
}

/* `nixConfig` is applied alphabetically, so by default `extra-<name>` is
   applied first and discarded by the later `<name>` assignment. */
TEST(ConfigFile, applyExtraBeforeBaseLegacy)
{
    auto & setting = flakeMergeTestSetting();
    applyFlakeConfig();
    ASSERT_EQ(setting.get(), Strings{"a"});
}

TEST(ConfigFile, applyExtraBeforeBaseDeterministic)
{
    EnableExperimentalFeature feature{"deterministic-config-merge"};
    auto & setting = flakeMergeTestSetting();
    applyFlakeConfig();
    ASSERT_EQ(setting.get(), (Strings{"a", "b"}));
}

/* Says yes only when asked to allow `accept-flake-config`, and records every
   prompt. */
class AllowAcceptFlakeConfigLogger : public Logger
{
public:
    std::vector<std::string> prompts;

    void log(Verbosity lvl, std::string_view s) noexcept override {}

    void logEI(const ErrorInfo & ei) noexcept override {}

    std::optional<char> ask(std::string_view s) override
    {
        prompts.emplace_back(s);
        return s.find("'accept-flake-config'") != s.npos ? 'y' : 'n';
    }
};

/* Accepting a flake's own `accept-flake-config = true` must not trust the
   flake's other settings: each is still prompted for. */
TEST(ConfigFile, applyAcceptFlakeConfigDoesNotTrustOtherSettings)
{
    EnableExperimentalFeature flakes{"flakes"};
    auto & setting = flakeMergeTestSetting();

    /* `ConfigFile::apply` sets `accept-flake-config` via `globalConfig`, so
       it has to reach the settings object passed in. */
    static flake::Settings flakeSettings;
    static GlobalConfig::Register r(&flakeSettings);
    Finally resetAccept([] { flakeSettings.acceptFlakeConfig.assign(false); });

    /* Keep the user's trusted-settings.json out of the test. */
    AutoDelete dataDir(createTempDir());
    setEnv("NIX_DATA_HOME", dataDir.path().string().c_str());
    Finally unsetDataDir([] { unsetenv("NIX_DATA_HOME"); });

    AllowAcceptFlakeConfigLogger testLogger;
    Finally restoreLogger([oldLogger = logger] { logger = oldLogger; });
    logger = &testLogger;

    flake::ConfigFile configFile;
    configFile.settings.emplace("accept-flake-config", Explicit<bool>{true});
    configFile.settings.emplace("flake-merge-test-list", std::vector<std::string>{"a"});
    configFile.apply(flakeSettings);

    ASSERT_TRUE(flakeSettings.acceptFlakeConfig.get());
    ASSERT_EQ(setting.get(), Strings{});
    ASSERT_TRUE(std::ranges::any_of(testLogger.prompts, [](const auto & p) {
        return p.find("'flake-merge-test-list'") != p.npos;
    }));
}

} // namespace nix
