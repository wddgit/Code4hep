#include <map>
#include <string>
#include <vector>

#include "FWCore/Utilities/interface/EDMException.h"
#include "FWCore/TestProcessor/interface/TestProcessor.h"
#include "catch2/catch_all.hpp"
#include "edm4hep/MCParticleCollection.h"

#include "../plugins/ProductDescriptionMetadata.h"

namespace {
  constexpr auto tag = "[ProductDescriptionMetadata]";

  struct PluginManagerFixture {
    PluginManagerFixture() {
      // Construction performs TestProcessor's shared once-only plugin setup.
      edm::test::TestProcessor setup(edm::test::TestProcessor::Config{R"_(
from FWCore.TestProcessor.TestProcess import *
process = TestProcess()
process.toTest = cms.EDProducer('c4h::TestTracksProducer')
process.moduleToTest(process.toTest)
)_"});
    }
  };

  edm::ProductDescription originalDescription() {
    return edm::ProductDescription(edm::InEvent, "generator", "SIM", "particles",
                                   edm::TypeID(typeid(edm4hep::MCParticleCollection)), true, false,
                                   {"truth", "particles"});
  }

  std::string entryKey(edm::ProductDescription const& description) {
    return c4h::productDescriptionPrefix + std::to_string(description.branchID().id());
  }

  void requireFormatError(podio::Frame const& metadata) {
    std::map<edm::BranchID, std::string> names{{edm::BranchID(17), "unchanged"}};
    auto const expected = names;
    try {
      c4h::restoreProductDescriptions(metadata, names);
    } catch (edm::Exception const& ex) {
      REQUIRE(ex.categoryCode() == edm::errors::FormatIncompatibility);
      REQUIRE(names == expected);
      return;
    }
    FAIL("Malformed ProductDescription metadata was accepted");
  }
}  // namespace

TEST_CASE_METHOD(PluginManagerFixture, "ProductDescription metadata preserves original identities and string branch aliases", tag) {
  auto original = originalDescription();
  std::map<edm::BranchID, edm::ProductDescription> descriptions{{original.branchID(), original}};
  std::map<edm::BranchID, std::string> names{{original.branchID(), "generatorparticles"}};
  podio::Frame metadata;
  metadata.putParameter("unrelated", "preserved");
  c4h::saveProductDescriptions(metadata, descriptions, names);

  REQUIRE(metadata.getAvailableCollections().empty());
  REQUIRE(metadata.getParameters().getMap<std::string>().at("unrelated") == std::vector<std::string>{"preserved"});
  REQUIRE(metadata.getParameters().getMap<int>().at(c4h::productDescriptionVersionKey) == std::vector<int>{1});
  std::vector<std::string> ids;
  for (auto const& [id, description] : descriptions) {
    ids.push_back(std::to_string(id.id()));
    auto const& values = metadata.getParameters().getMap<std::string>();
    REQUIRE(values.at(entryKey(description)) ==
            std::vector<std::string>{"0", description.moduleLabel(), "SIM", description.productInstanceName(),
                                     description.fullClassName(), description.friendlyClassName(),
                                      "0", "generatorparticles"});
    REQUIRE(values.at(entryKey(description) + ".aliases") == std::vector<std::string>{"particles", "truth"});
  }
  REQUIRE(metadata.getParameters().getMap<std::string>().at(c4h::productDescriptionIDsKey) == ids);
  REQUIRE(c4h::productCollectionName(original, names) == "generatorparticles");
  REQUIRE(c4h::productCollectionName(original, {}) == "generator");
  REQUIRE_THROWS_AS(c4h::productCollectionName(original, {{edm::BranchID(17), "Other"}}), edm::Exception);
  std::map<edm::BranchID, std::string> restoredNames;
  auto registry = c4h::restoreProductDescriptions(metadata, restoredNames);
  REQUIRE(registry);
  REQUIRE(restoredNames == names);
  REQUIRE(registry->size() == 1);
  auto const& restored = registry->productList().begin()->second;
  REQUIRE(restored.branchID() == original.branchID());
  REQUIRE_FALSE(restored.aliasForBranchID().isValid());
  REQUIRE(restored.branchAliases() == original.branchAliases());
}

TEST_CASE_METHOD(PluginManagerFixture, "Absent ProductDescription metadata differs from a valid empty registry", tag) {
  podio::Frame metadata;
  std::map<edm::BranchID, std::string> names{{edm::BranchID(17), "unchanged"}};
  SECTION("Entirely absent") {
    SECTION("Empty frame") {}
    SECTION("Unrelated parameters") { metadata.putParameter("unrelated", 1); }
    SECTION("Wrong version type") {
      metadata.putParameter(c4h::productDescriptionVersionKey, "1");
      metadata.putParameter(c4h::productDescriptionIDsKey, std::vector<std::string>{});
    }
    SECTION("Orphan metadata") {
      metadata.putParameter(std::string(c4h::productDescriptionPrefix) + "17.aliases", std::vector<std::string>{});
    }
    REQUIRE(c4h::restoreProductDescriptions(metadata, names) == nullptr);
    REQUIRE(names.at(edm::BranchID(17)) == "unchanged");
  }
  SECTION("Explicit empty registry") {
    c4h::saveProductDescriptions(metadata, {}, {});
    SECTION("Additional keys and types are ignored") {
      metadata.putParameter(std::string(c4h::productDescriptionPrefix) + "extra", 1);
      metadata.putParameter(c4h::productDescriptionVersionKey, "ignored");
      metadata.putParameter(c4h::productDescriptionIDsKey, 1.f);
    }
    auto registry = c4h::restoreProductDescriptions(metadata, names);
    REQUIRE(registry != nullptr);
    REQUIRE(registry->size() == 0);
    REQUIRE(names.empty());
  }
}

TEST_CASE_METHOD(PluginManagerFixture, "Malformed ProductDescription schema is rejected before converter lookup", tag) {
  auto original = originalDescription();
  podio::Frame metadata;
  c4h::saveProductDescriptions(metadata, {{original.branchID(), original}}, {{original.branchID(), "generatorparticles"}});
  auto const key = entryKey(original);
  auto fields = metadata.getParameters().getMap<std::string>().at(key);

  SECTION("Unsupported version") { metadata.putParameter(c4h::productDescriptionVersionKey, 2); }
  SECTION("Non scalar version") { metadata.putParameter(c4h::productDescriptionVersionKey, std::vector<int>{1, 1}); }
  SECTION("Missing IDs") {
    metadata = podio::Frame{};
    metadata.putParameter(c4h::productDescriptionVersionKey, 1);
  }
  SECTION("Noncanonical or invalid IDs") {
    auto const id = GENERATE(std::string{"0"}, std::string{"01"}, std::string{"-1"}, std::string{"+1"},
                             std::string{"4294967296"}, std::string{"1x"}, std::string{" 1"}, std::string{});
    metadata.putParameter(c4h::productDescriptionIDsKey, std::vector<std::string>{id});
  }
  SECTION("Duplicate IDs") {
    auto const id = std::to_string(original.branchID().id());
    metadata.putParameter(c4h::productDescriptionIDsKey, std::vector<std::string>{id, id});
  }
  SECTION("Missing aliases") {
    metadata = podio::Frame{};
    metadata.putParameter(c4h::productDescriptionVersionKey, 1);
    metadata.putParameter(c4h::productDescriptionIDsKey, std::vector<std::string>{std::to_string(original.branchID().id())});
    metadata.putParameter(key, fields);
  }
  SECTION("Wrong field count") { fields.pop_back(); metadata.putParameter(key, fields); }
  SECTION("Wrong field type") {
    metadata = podio::Frame{};
    metadata.putParameter(c4h::productDescriptionVersionKey, 1);
    metadata.putParameter(c4h::productDescriptionIDsKey, std::vector<std::string>{std::to_string(original.branchID().id())});
    metadata.putParameter(key, std::vector<int>{1});
    metadata.putParameter(key + ".aliases", std::vector<std::string>{});
  }
  SECTION("Empty identity fields") {
    auto const index = GENERATE(1, 2, 4, 5);
    fields[index].clear();
    metadata.putParameter(key, fields);
  }
  SECTION("Wrong branch type") { fields[0] = "4"; metadata.putParameter(key, fields); }
  SECTION("Stored non event product") { fields[0] = "1"; metadata.putParameter(key, fields); }
  SECTION("Changed branch identity") { fields[1] = "other"; metadata.putParameter(key, fields); }
  SECTION("Inconsistent class names") { fields[4] = "other::Collection"; metadata.putParameter(key, fields); }
  SECTION("Noncanonical alias target") { fields[6] = "00"; metadata.putParameter(key, fields); }
  SECTION("Nonzero EDAlias target") { fields[6] = "17"; metadata.putParameter(key, fields); }
  SECTION("Self EDAlias target") {
    fields[6] = std::to_string(original.branchID().id());
    metadata.putParameter(key, fields);
  }
  SECTION("Duplicate branch aliases") {
    metadata.putParameter(key + ".aliases", std::vector<std::string>{"truth", "truth"});
  }
  SECTION("Empty branch alias") { metadata.putParameter(key + ".aliases", std::vector<std::string>{""}); }
  requireFormatError(metadata);
}

TEST_CASE_METHOD(PluginManagerFixture, "ProductDescription serialization validates stored products", tag) {
  auto original = originalDescription();
  edm::ProductDescription other(edm::InEvent, "generatorparticles", "SIM", "",
                               edm::TypeID(typeid(edm4hep::MCParticleCollection)), true);
  std::map<edm::BranchID, edm::ProductDescription> descriptions{
      {original.branchID(), original}, {other.branchID(), other}};
  std::map<edm::BranchID, std::string> names{{original.branchID(), "generatorparticles"}};
  SECTION("Mismatched map ID") { descriptions.emplace(edm::BranchID(17), original); }
  SECTION("Unknown stored product") { names.emplace(edm::BranchID(17), "Unknown"); }
  SECTION("Empty stored collection") { names[original.branchID()] = ""; }
  SECTION("Duplicate collection names") { names.emplace(other.branchID(), "generatorparticles"); }
  podio::Frame metadata;
  REQUIRE_THROWS_AS(c4h::saveProductDescriptions(metadata, descriptions, names), edm::Exception);
  REQUIRE(metadata.getParameters().getMap<int>().empty());
  REQUIRE(metadata.getParameters().getMap<std::string>().empty());
}

TEST_CASE_METHOD(PluginManagerFixture, "ProductDescription metadata rejects inconsistent stored collection names without partial updates", tag) {
  auto original = originalDescription();
  std::map<edm::BranchID, std::string> names{{original.branchID(), "StoredTruth"}};
  auto const expectedNames = names;
  podio::Frame metadata;
  bool restoring = false;
  SECTION("Save rejects a mismatched collection name") {}
  SECTION("Restore rejects a tampered collection name") {
    restoring = true;
    c4h::saveProductDescriptions(metadata, {{original.branchID(), original}},
                                {{original.branchID(), "generatorparticles"}});
    auto fields = metadata.getParameters().getMap<std::string>().at(entryKey(original));
    fields[7] = "StoredTruth";
    metadata.putParameter(entryKey(original), fields);
    REQUIRE_THROWS_WITH(c4h::restoreStoredProductDescriptions(metadata),
                        Catch::Matchers::ContainsSubstring("stored collection name 'StoredTruth'"));
  }
  try {
    if (restoring)
      c4h::restoreProductDescriptions(metadata, names);
    else
      c4h::saveProductDescriptions(metadata, {{original.branchID(), original}}, names);
    FAIL("Inconsistent stored collection name was accepted");
  } catch (edm::Exception const& ex) {
    REQUIRE(ex.categoryCode() == edm::errors::FormatIncompatibility);
    std::string const message = ex.what();
    for (auto const& text : {"stored collection name 'StoredTruth'", "expected collection name 'generatorparticles'",
                             "module 'generator'", "process 'SIM'", "instance 'particles'",
                             "preserves the original BranchID and podio collection IDs", "use a consistent",
                             "moduleLabel + productInstanceName collection mapping"})
      REQUIRE(message.find(text) != std::string::npos);
    REQUIRE(message.find("BranchID " + std::to_string(original.branchID().id())) != std::string::npos);
    REQUIRE(names == expectedNames);
    if (!restoring) {
      REQUIRE(metadata.getParameters().getMap<int>().empty());
      REQUIRE(metadata.getParameters().getMap<std::string>().empty());
    }
  }
}

TEST_CASE_METHOD(PluginManagerFixture, "ProductDescription metadata accepts metadata-only products with empty collection names", tag) {
  auto original = originalDescription();
  podio::Frame metadata;
  c4h::saveProductDescriptions(metadata, {{original.branchID(), original}}, {});
  auto stored = c4h::restoreStoredProductDescriptions(metadata);
  REQUIRE(stored);
  REQUIRE(stored->at(original.branchID()).fields[7].empty());
  std::map<edm::BranchID, std::string> names{{edm::BranchID(17), "unchanged"}};
  auto registry = c4h::restoreProductDescriptions(metadata, names);
  REQUIRE(registry);
  REQUIRE(names.empty());
  REQUIRE(registry->size() == 1);
  auto const& restored = registry->productList().begin()->second;
  REQUIRE(restored.branchID() == original.branchID());
  REQUIRE(restored.dropped());
  REQUIRE(restored.branchAliases() == original.branchAliases());
}

TEST_CASE_METHOD(PluginManagerFixture, "EDAlias metadata is stored under the original collection name", tag) {
  auto original = originalDescription();
  edm::ProductDescription alias(original, "selected", "truth");
  // Current-process aliases are produced; aliases read from a file are not.
  alias.setProduced(GENERATE(true, false));
  REQUIRE(alias.aliasForBranchID() == original.branchID());
  REQUIRE(alias.originalBranchID() == original.branchID());
  std::string const storedName = original.moduleLabel() + original.productInstanceName();
  std::map<edm::BranchID, edm::ProductDescription> const descriptions{{original.branchID(), original},
                                                                     {alias.branchID(), alias}};
  std::map<edm::BranchID, std::string> const names{{alias.branchID(), storedName}};
  podio::Frame metadata;
  c4h::saveProductDescriptions(metadata, descriptions, names);
  auto const& values = metadata.getParameters().getMap<std::string>();
  auto const& aliasFields = values.at(entryKey(alias));
  REQUIRE(aliasFields[1] == "selected");
  REQUIRE(aliasFields[3] == "truth");
  REQUIRE(aliasFields[6] == std::to_string(original.branchID().id()));
  REQUIRE(aliasFields[7] == storedName);
  REQUIRE(values.at(entryKey(original))[7].empty());

  std::map<edm::BranchID, std::string> restoredNames;
  auto registry = c4h::restoreProductDescriptions(metadata, restoredNames);
  REQUIRE(registry);
  REQUIRE(restoredNames == names);
  REQUIRE(registry->size() == 2);
  std::map<edm::BranchID, edm::ProductDescription> restored;
  for (auto const& entry : registry->productList())
    restored.emplace(entry.second.branchID(), entry.second);
  auto const& restoredAlias = restored.at(alias.branchID());
  REQUIRE(restoredAlias.moduleLabel() == "selected");
  REQUIRE(restoredAlias.productInstanceName() == "truth");
  REQUIRE(restoredAlias.aliasForBranchID() == original.branchID());
  REQUIRE(restoredAlias.originalBranchID() == original.branchID());
  REQUIRE_FALSE(restoredAlias.produced());
  REQUIRE_FALSE(restoredAlias.isAlias());
  REQUIRE_FALSE(restoredAlias.dropped());
  REQUIRE(restoredAlias.branchAliases() == original.branchAliases());
  REQUIRE(c4h::productCollectionName(restoredAlias, restoredNames) == storedName);
  auto const& restoredOriginal = restored.at(original.branchID());
  REQUIRE(restoredOriginal.dropped());
  REQUIRE_FALSE(restoredOriginal.aliasForBranchID().isValid());

  // A rewrite saves the restored descriptions unchanged.
  podio::Frame rewritten;
  REQUIRE_NOTHROW(c4h::saveProductDescriptions(rewritten, restored, restoredNames));
  REQUIRE(rewritten.getParameters().getMap<std::string>() == values);
}

TEST_CASE_METHOD(PluginManagerFixture, "Inconsistent EDAlias metadata is rejected", tag) {
  auto original = originalDescription();
  edm::ProductDescription alias(original, "selected", "truth");
  std::string const storedName = original.moduleLabel() + original.productInstanceName();
  std::map<edm::BranchID, edm::ProductDescription> descriptions{{original.branchID(), original},
                                                               {alias.branchID(), alias}};
  std::map<edm::BranchID, std::string> names{{alias.branchID(), storedName}};
  podio::Frame metadata;
  SECTION("Saving") {
    SECTION("Original description missing") { descriptions.erase(original.branchID()); }
    SECTION("Alias stored under its own labels") { names[alias.branchID()] = "selectedtruth"; }
    SECTION("Original also stored") { names.emplace(original.branchID(), storedName); }
    SECTION("Alias not stored") { names.clear(); }
    REQUIRE_THROWS_AS(c4h::saveProductDescriptions(metadata, descriptions, names), edm::Exception);
    REQUIRE(metadata.getParameters().getMap<std::string>().empty());
    return;
  }
  SECTION("Reading") {
    c4h::saveProductDescriptions(metadata, descriptions, names);
    auto fields = metadata.getParameters().getMap<std::string>().at(entryKey(alias));
    SECTION("Unknown target") { fields[6] = "17"; }
    SECTION("Self target") { fields[6] = std::to_string(alias.branchID().id()); }
    SECTION("Alias collection name changed") { fields[7] = "selectedtruth"; }
    SECTION("Alias metadata-only") { fields[7].clear(); }
    SECTION("Original stored") {
      auto originalFields = metadata.getParameters().getMap<std::string>().at(entryKey(original));
      originalFields[7] = storedName;
      metadata.putParameter(entryKey(original), originalFields);
    }
    SECTION("Target with a different process") {
      edm::ProductDescription other(edm::InEvent, "other", "RECO", "particles",
                                    edm::TypeID(typeid(edm4hep::MCParticleCollection)), false);
      c4h::saveProductDescriptions(metadata, {{original.branchID(), original}, {alias.branchID(), alias},
                                              {other.branchID(), other}}, names);
      fields[6] = std::to_string(other.branchID().id());
      fields[7] = "otherparticles";
    }
    metadata.putParameter(entryKey(alias), fields);
    requireFormatError(metadata);
  }
}
