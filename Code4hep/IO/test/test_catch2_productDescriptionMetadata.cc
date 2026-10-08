#include "../plugins/ProductDescriptionMetadata.h"
#include "edm4hep/MCParticleCollection.h"
#include "catch2/catch_all.hpp"

#include <map>
#include <string>
#include <vector>

namespace {
  constexpr auto tag = "[ProductDescriptionMetadata]";

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

TEST_CASE("ProductDescription metadata serializes original identities and auxiliary alias targets", tag) {
  auto original = originalDescription();
  edm::ProductDescription alias(original, "selected", "truth");
  edm::ProductDescription aliasOfAlias(alias, "analysis", "particles");
  std::map<edm::BranchID, edm::ProductDescription> descriptions{
      {original.branchID(), original}, {alias.branchID(), alias}, {aliasOfAlias.branchID(), aliasOfAlias}};
  std::map<edm::BranchID, std::string> names{{aliasOfAlias.branchID(), "StoredTruth"}};
  podio::Frame metadata;
  metadata.putParameter("unrelated", "preserved");
  c4h::putProductDescriptions(metadata, descriptions, names);

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
                                     std::to_string(description.aliasForBranchID().id()),
                                     id == aliasOfAlias.branchID() ? "StoredTruth" : ""});
    REQUIRE(values.at(entryKey(description) + ".aliases") == std::vector<std::string>{"particles", "truth"});
  }
  REQUIRE(metadata.getParameters().getMap<std::string>().at(c4h::productDescriptionIDsKey) == ids);
  REQUIRE(c4h::productCollectionName(aliasOfAlias, names) == "StoredTruth");
  REQUIRE(c4h::productCollectionName(original, {}) == "generator");
  REQUIRE_THROWS_AS(c4h::productCollectionName(original, names), edm::Exception);
}

TEST_CASE("Absent ProductDescription metadata differs from a valid empty registry", tag) {
  podio::Frame metadata;
  std::map<edm::BranchID, std::string> names{{edm::BranchID(17), "unchanged"}};
  SECTION("Entirely absent") {
    SECTION("Empty frame") {}
    SECTION("Unrelated parameters") { metadata.putParameter("unrelated", 1); }
    REQUIRE(c4h::restoreProductDescriptions(metadata, names) == nullptr);
    REQUIRE(names.at(edm::BranchID(17)) == "unchanged");
  }
  SECTION("Explicit empty registry") {
    c4h::putProductDescriptions(metadata, {}, {});
    auto registry = c4h::restoreProductDescriptions(metadata, names);
    REQUIRE(registry != nullptr);
    REQUIRE(registry->size() == 0);
    REQUIRE(names.empty());
  }
}

TEST_CASE("Malformed ProductDescription schema is rejected before converter lookup", tag) {
  auto original = originalDescription();
  podio::Frame metadata;
  c4h::putProductDescriptions(metadata, {{original.branchID(), original}}, {{original.branchID(), "Truth"}});
  auto const key = entryKey(original);
  auto fields = metadata.getParameters().getMap<std::string>().at(key);

  SECTION("Unsupported version") { metadata.putParameter(c4h::productDescriptionVersionKey, 2); }
  SECTION("Non scalar version") { metadata.putParameter(c4h::productDescriptionVersionKey, std::vector<int>{1, 1}); }
  SECTION("Wrong version type") {
    metadata = podio::Frame{};
    metadata.putParameter(c4h::productDescriptionVersionKey, "1");
    metadata.putParameter(c4h::productDescriptionIDsKey, std::vector<std::string>{});
  }
  SECTION("Orphan metadata is not absent") {
    metadata = podio::Frame{};
    metadata.putParameter(key + ".aliases", std::vector<std::string>{});
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
  SECTION("Wrong field type") { metadata.putParameter(key, std::vector<int>{1}); }
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
  SECTION("Missing alias target") { fields[6] = "17"; metadata.putParameter(key, fields); }
  SECTION("Alias cycle") { fields[6] = std::to_string(original.branchID().id()); metadata.putParameter(key, fields); }
  SECTION("Two node alias cycle") {
    edm::ProductDescription alias(original, "selected", "truth");
    c4h::putProductDescriptions(metadata, {{original.branchID(), original}, {alias.branchID(), alias}},
                                {{alias.branchID(), "Truth"}});
    fields[6] = std::to_string(alias.branchID().id());
    fields[7].clear();
    metadata.putParameter(key, fields);
  }
  SECTION("Alias target has another process") {
    edm::ProductDescription target(edm::InEvent, "other", "OTHER", "particles",
                                    edm::TypeID(typeid(edm4hep::MCParticleCollection)), true, false,
                                    original.branchAliases());
    c4h::putProductDescriptions(metadata, {{original.branchID(), original}, {target.branchID(), target}},
                                {{original.branchID(), "Truth"}});
    fields[6] = std::to_string(target.branchID().id());
    metadata.putParameter(key, fields);
  }
  SECTION("Duplicate branch aliases") {
    metadata.putParameter(key + ".aliases", std::vector<std::string>{"truth", "truth"});
  }
  SECTION("Empty branch alias") { metadata.putParameter(key + ".aliases", std::vector<std::string>{""}); }
  SECTION("Unexpected schema key") { metadata.putParameter(std::string(c4h::productDescriptionPrefix) + "extra", 1); }
  requireFormatError(metadata);
}

TEST_CASE("ProductDescription serialization validates stored products and alias graphs", tag) {
  auto original = originalDescription();
  edm::ProductDescription alias(original, "selected", "truth");
  std::map<edm::BranchID, edm::ProductDescription> descriptions{
      {original.branchID(), original}, {alias.branchID(), alias}};
  std::map<edm::BranchID, std::string> names{{alias.branchID(), "Truth"}};
  SECTION("Missing auxiliary target") { descriptions.erase(original.branchID()); }
  SECTION("Mismatched map ID") { descriptions.emplace(edm::BranchID(17), original); }
  SECTION("Unknown stored product") { names.emplace(edm::BranchID(17), "Unknown"); }
  SECTION("Empty stored collection") { names[alias.branchID()] = ""; }
  SECTION("Duplicate collection names") { names.emplace(original.branchID(), "Truth"); }
  SECTION("Alias omits an inherited branch alias") {
    original.insertBranchAlias("extra");
    descriptions.at(original.branchID()) = original;
  }
  podio::Frame metadata;
  REQUIRE_THROWS_AS(c4h::putProductDescriptions(metadata, descriptions, names), edm::Exception);
  REQUIRE(metadata.getParameters().getMap<int>().empty());
  REQUIRE(metadata.getParameters().getMap<std::string>().empty());
}

TEST_CASE("Alias descriptions retain additional branch aliases in metadata", tag) {
  auto original = originalDescription();
  edm::ProductDescription alias(original, "selected", "truth");
  alias.insertBranchAlias("selectedTruth");
  podio::Frame metadata;
  REQUIRE_NOTHROW(c4h::putProductDescriptions(metadata,
                                             {{original.branchID(), original}, {alias.branchID(), alias}},
                                             {{alias.branchID(), "Truth"}}));
  REQUIRE(metadata.getParameters().getMap<std::string>().at(entryKey(alias) + ".aliases") ==
          std::vector<std::string>{"particles", "selectedTruth", "truth"});
}
