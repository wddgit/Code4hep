#include "../plugins/ParameterSetMetadata.h"
#include "FWCore/Utilities/interface/Digest.h"
#include "podio/Reader.h"
#include "podio/Writer.h"
#include "podio/FrameCategories.h"
#include "catch2/catch_all.hpp"

#include <filesystem>
#include <string>
#include <utility>
#include <vector>

static constexpr auto s_tag = "[ParameterSetMetadata]";

namespace {
  std::string blobID(std::string const& blob) {
    cms::Digest digest(blob);
    return digest.digest().toString();
  }

  podio::Frame metadataFrame(std::vector<std::string> ids, std::vector<std::string> blobs) {
    podio::Frame metadata;
    metadata.putParameter(c4h::parameterSetVersionKey, 1);
    metadata.putParameter(c4h::parameterSetIDsKey, std::move(ids));
    metadata.putParameter(c4h::parameterSetBlobsKey, std::move(blobs));
    return metadata;
  }

  void requireFormatError(podio::Frame const& metadata) {
    try {
      c4h::restoreParameterSets(metadata);
    } catch (edm::Exception const& ex) {
      REQUIRE(ex.categoryCode() == edm::errors::FormatIncompatibility);
      return;
    }
    FAIL("Malformed ParameterSet metadata was accepted");
  }
}  // namespace

TEST_CASE("ParameterSet registry snapshot and podio metadata round trip", s_tag) {
  edm::ParameterSet pset;
  pset.addParameter<int>("parameterSetMetadataSnapshotValue", 314159);
  pset.addUntrackedParameter<int>("notPersisted", 17);
  pset.registerIt();

  edm::pset::Registry::regmap_type persistentSets;
  edm::pset::Registry::instance()->fillMap(persistentSets);
  std::vector<std::string> expectedIDs;
  std::vector<std::string> expectedBlobs;
  for (auto const& [id, blob] : persistentSets) {
    expectedIDs.emplace_back();
    id.toString(expectedIDs.back());
    expectedBlobs.push_back(blob.pset());
  }
  REQUIRE(persistentSets.at(pset.id()).pset() == pset.toString());
  REQUIRE(persistentSets.at(pset.id()).pset().find("notPersisted") == std::string::npos);

  auto metadata = c4h::parameterSetMetadata();
  auto const& parameters = metadata.getParameters();
  REQUIRE(metadata.getAvailableCollections().empty());
  REQUIRE(parameters.getMap<int>().size() == 1);
  REQUIRE(parameters.getMap<int>().at(c4h::parameterSetVersionKey) == std::vector<int>{1});
  REQUIRE(parameters.getMap<std::string>().size() == 2);
  REQUIRE(parameters.getMap<std::string>().at(c4h::parameterSetIDsKey) == expectedIDs);
  REQUIRE(parameters.getMap<std::string>().at(c4h::parameterSetBlobsKey) == expectedBlobs);
  REQUIRE(parameters.getMap<float>().empty());
  REQUIRE(parameters.getMap<double>().empty());

  const std::string fileName = "parameterSetMetadata.root";
  {
    auto writer = podio::makeWriter(fileName, "ROOT");
    writer.writeFrame(metadata, podio::Category::Metadata);
    writer.finish();
  }
  {
    auto reader = podio::makeReader(fileName);
    REQUIRE(reader.getEntries(podio::Category::Metadata) == 1);
    auto restored = reader.readFrame(podio::Category::Metadata, 0);
    REQUIRE(restored.getAvailableCollections().empty());
    REQUIRE(restored.getParameters().getMap<int>() == parameters.getMap<int>());
    REQUIRE(restored.getParameters().getMap<std::string>() == parameters.getMap<std::string>());
    REQUIRE(restored.getParameters().getMap<float>() == parameters.getMap<float>());
    REQUIRE(restored.getParameters().getMap<double>() == parameters.getMap<double>());
    auto const size = edm::pset::Registry::instance()->size();
    REQUIRE_NOTHROW(c4h::restoreParameterSets(restored));
    REQUIRE(edm::pset::Registry::instance()->size() == size);
    REQUIRE(edm::getParameterSet(pset.id()).getParameter<int>("parameterSetMetadataSnapshotValue") == 314159);
  }
  std::filesystem::remove(fileName);
}

TEST_CASE("Restore previously unregistered scalar and nested ParameterSets by ID", s_tag) {
  edm::ParameterSet scalar;
  scalar.addParameter<int>("parameterSetMetadataFreshScalar", 271828);
  scalar.addParameter<std::string>("label", "fresh metadata scalar");
  auto const scalarBlob = scalar.toString();
  auto const scalarID = blobID(scalarBlob);
  // Encode an ID reference without registering either PSet; decode the parent first.
  auto const parentBlob = "<parameterSetMetadataFreshChild=+Q(" + scalarID + ")>";
  auto const parentID = blobID(parentBlob);
  auto& registry = *edm::pset::Registry::instance();
  REQUIRE(registry.getMapped(edm::ParameterSetID(scalarID)) == nullptr);
  REQUIRE(registry.getMapped(edm::ParameterSetID(parentID)) == nullptr);
  auto const size = registry.size();
  auto metadata = metadataFrame({parentID, scalarID}, {parentBlob, scalarBlob});

  REQUIRE_NOTHROW(c4h::restoreParameterSets(metadata));
  REQUIRE(registry.size() == size + 2);
  auto const& restoredScalar = edm::getParameterSet(edm::ParameterSetID(scalarID));
  REQUIRE(restoredScalar.id() == edm::ParameterSetID(scalarID));
  REQUIRE(restoredScalar.toString() == scalarBlob);
  REQUIRE(restoredScalar.getParameter<int>("parameterSetMetadataFreshScalar") == 271828);
  REQUIRE(restoredScalar.getParameter<std::string>("label") == "fresh metadata scalar");
  auto const& restoredParent = edm::getParameterSet(edm::ParameterSetID(parentID));
  REQUIRE(restoredParent.id() == edm::ParameterSetID(parentID));
  REQUIRE(restoredParent.toString() == parentBlob);
  auto const& child = restoredParent.getParameterSet("parameterSetMetadataFreshChild");
  REQUIRE(child.id() == edm::ParameterSetID(scalarID));
  REQUIRE(child.getParameter<int>("parameterSetMetadataFreshScalar") == 271828);
  REQUIRE(child.getParameter<std::string>("label") == "fresh metadata scalar");
}

TEST_CASE("Absent or unrelated ParameterSet metadata is ignored", s_tag) {
  podio::Frame metadata;
  SECTION("Absent metadata") {}
  SECTION("Unrelated metadata") {
    metadata.putParameter("unrelated.int", 1);
    metadata.putParameter("unrelated.float", 2.f);
    metadata.putParameter("unrelated.double", 3.);
    metadata.putParameter("unrelated.string", "ordinary podio metadata");
  }
  auto const size = edm::pset::Registry::instance()->size();
  REQUIRE_NOTHROW(c4h::restoreParameterSets(metadata));
  REQUIRE(edm::pset::Registry::instance()->size() == size);
}

TEST_CASE("Malformed ParameterSet metadata is rejected without partial insertion", s_tag) {
  edm::ParameterSet pset;
  pset.addParameter<int>("parameterSetMetadataNeverInserted", 161803);
  auto const blob = pset.toString();
  auto const id = blobID(blob);
  auto& registry = *edm::pset::Registry::instance();
  REQUIRE(registry.getMapped(edm::ParameterSetID(id)) == nullptr);
  auto const size = registry.size();
  auto metadata = metadataFrame({id}, {blob});

  SECTION("Missing keys") {
    auto const missing = GENERATE(0, 1, 2);
    metadata = podio::Frame{};
    if (missing != 0)
      metadata.putParameter(c4h::parameterSetVersionKey, 1);
    if (missing != 1)
      metadata.putParameter(c4h::parameterSetIDsKey, std::vector<std::string>{id});
    if (missing != 2)
      metadata.putParameter(c4h::parameterSetBlobsKey, std::vector<std::string>{blob});
  }
  SECTION("Wrong version") {
    metadata.putParameter(c4h::parameterSetVersionKey, GENERATE(0, 2));
  }
  SECTION("Version is not a single integer") {
    auto const version = GENERATE(std::vector<int>{}, std::vector<int>{1, 1});
    metadata.putParameter(c4h::parameterSetVersionKey, version);
  }
  SECTION("Wrong key types") {
    auto const wrong = GENERATE(0, 1, 2);
    metadata = podio::Frame{};
    if (wrong == 0)
      metadata.putParameter(c4h::parameterSetVersionKey, "1");
    else
      metadata.putParameter(c4h::parameterSetVersionKey, 1);
    if (wrong == 1)
      metadata.putParameter(c4h::parameterSetIDsKey, 1.f);
    else
      metadata.putParameter(c4h::parameterSetIDsKey, std::vector<std::string>{id});
    if (wrong == 2)
      metadata.putParameter(c4h::parameterSetBlobsKey, 1.);
    else
      metadata.putParameter(c4h::parameterSetBlobsKey, std::vector<std::string>{blob});
  }
  SECTION("Mismatched lengths") {
    auto const idsLonger = GENERATE(false, true);
    if (idsLonger)
      metadata.putParameter(c4h::parameterSetBlobsKey, std::vector<std::string>{});
    else
      metadata.putParameter(c4h::parameterSetIDsKey, std::vector<std::string>{});
  }
  SECTION("Invalid ID after a valid entry") {
    auto const invalidID = GENERATE(std::string{},
                                    std::string(31, 'a'),
                                    std::string(33, 'a'),
                                    std::string(32, 'g'),
                                    std::string{"d41d8cd98f00b204e9800998ecf8427e"});
    metadata = metadataFrame({id, invalidID}, {blob, blob});
  }
  SECTION("Invalid blob after a valid entry") {
    auto const invalidBlob = GENERATE(std::string{}, std::string{"not a PSet"}, std::string{"<missingSeparator>"});
    auto const invalidID = blobID("parameterSetMetadataInvalidBlob:" + invalidBlob);
    REQUIRE(registry.getMapped(edm::ParameterSetID(invalidID)) == nullptr);
    metadata = metadataFrame({id, invalidID}, {blob, invalidBlob});
  }

  requireFormatError(metadata);
  REQUIRE(registry.size() == size);
  REQUIRE(registry.getMapped(edm::ParameterSetID(id)) == nullptr);
}
