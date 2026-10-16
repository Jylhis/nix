#include <gtest/gtest.h>

#include "nix/flake/flake.hh"
#include "nix/flake/settings.hh"
#include "nix/util/config-global.hh"

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

/* `nixConfig` is applied alphabetically, so `extra-<name>` is applied first
   and discarded by the later `<name>` assignment. */
TEST(ConfigFile, applyExtraBeforeBaseLegacy)
{
    auto & setting = flakeMergeTestSetting();
    applyFlakeConfig();
    ASSERT_EQ(setting.get(), Strings{"a"});
}

} // namespace nix
