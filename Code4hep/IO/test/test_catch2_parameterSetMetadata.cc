#include <algorithm>
#include <bit>
#include <filesystem>
#include <iterator>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include "../plugins/ParameterSetMetadata.h"
#include "ParameterSetMetadataTestHelpers.h"
#include "FWCore/ParameterSet/interface/ParameterSet.h"
#include "FWCore/ParameterSet/interface/Registry.h"
#include "FWCore/Utilities/interface/EDMException.h"
#include "FWCore/Utilities/interface/Digest.h"
#include "podio/Reader.h"
#include "podio/Writer.h"
#include "podio/FrameCategories.h"
#include "catch2/catch_all.hpp"

static constexpr auto s_tag = "[ParameterSetMetadata]";

namespace {
  std::string blobID(std::string const& blob) {
    cms::Digest digest(blob);
    return digest.digest().compactForm();
  }

  using c4h::test::metadataFrame;
  using c4h::test::packedWords;

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
  std::vector<int> expectedLengths;
  for (auto const& [id, blob] : persistentSets) {
    expectedIDs.push_back(id.compactForm());
    REQUIRE(expectedIDs.back().size() == 16);
    expectedBlobs.push_back(blob.pset());
    expectedLengths.push_back(static_cast<int>(blob.pset().size()));
  }
  REQUIRE(persistentSets.at(pset.id()).pset() == pset.toString());
  REQUIRE(persistentSets.at(pset.id()).pset().find("notPersisted") == std::string::npos);

  auto metadata = c4h::saveParameterSets();
  auto const& parameters = metadata.getParameters();
  REQUIRE(metadata.getAvailableCollections().empty());
  REQUIRE(parameters.getMap<int>().size() == 4);
  REQUIRE(parameters.getMap<int>().at(c4h::parameterSetVersionKey) == std::vector<int>{1});
  REQUIRE(parameters.getMap<std::string>().empty());
  REQUIRE(parameters.getMap<int>().at(c4h::parameterSetIDsKey) == packedWords(expectedIDs));
  REQUIRE(parameters.getMap<int>().at(c4h::parameterSetBlobsKey) == packedWords(expectedBlobs));
  REQUIRE(parameters.getMap<int>().at(c4h::parameterSetLengthsKey) == expectedLengths);
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
  std::string scalarIDText;
  edm::ParameterSetID(scalarID).toString(scalarIDText);
  auto const parentBlob = "<parameterSetMetadataFreshChild=+Q(" + scalarIDText + ")>";
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
  SECTION("IDs and blobs without a version") {
    metadata.putParameter(c4h::parameterSetIDsKey, std::vector<std::string>{"invalid ID"});
    metadata.putParameter(c4h::parameterSetBlobsKey, std::vector<std::string>{"invalid blob"});
  }
  SECTION("Non-integer version") {
    auto const type = GENERATE(0, 1, 2);
    if (type == 0)
      metadata.putParameter(c4h::parameterSetVersionKey, "1");
    else if (type == 1)
      metadata.putParameter(c4h::parameterSetVersionKey, 1.f);
    else
      metadata.putParameter(c4h::parameterSetVersionKey, 1.);
    metadata.putParameter(c4h::parameterSetIDsKey, std::vector<int>{1, 2, 3});
  }
  auto const size = edm::pset::Registry::instance()->size();
  REQUIRE(c4h::readParameterSetIDs(metadata).empty());
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
    auto const missing = GENERATE(1, 2, 3);
    metadata = podio::Frame{};
    metadata.putParameter(c4h::parameterSetVersionKey, 1);
    if (missing != 1)
      metadata.putParameter(c4h::parameterSetIDsKey, packedWords({id}));
    if (missing != 2)
      metadata.putParameter(c4h::parameterSetBlobsKey, packedWords({blob}));
    if (missing != 3)
      metadata.putParameter(c4h::parameterSetLengthsKey, static_cast<int>(blob.size()));
  }
  SECTION("Wrong version") {
    metadata.putParameter(c4h::parameterSetVersionKey, GENERATE(0, 2));
  }
  SECTION("Version is not a single integer") {
    auto const version = GENERATE(std::vector<int>{}, std::vector<int>{1, 1});
    metadata.putParameter(c4h::parameterSetVersionKey, version);
  }
  SECTION("Wrong key types") {
    auto const wrong = GENERATE(1, 2, 3);
    metadata = podio::Frame{};
    metadata.putParameter(c4h::parameterSetVersionKey, 1);
    if (wrong == 1)
      metadata.putParameter(c4h::parameterSetIDsKey, 1.f);
    else
      metadata.putParameter(c4h::parameterSetIDsKey, packedWords({id}));
    if (wrong == 2)
      metadata.putParameter(c4h::parameterSetBlobsKey, 1.);
    else
      metadata.putParameter(c4h::parameterSetBlobsKey, packedWords({blob}));
    if (wrong == 3)
      metadata.putParameter(c4h::parameterSetLengthsKey, "wrong");
    else
      metadata.putParameter(c4h::parameterSetLengthsKey, static_cast<int>(blob.size()));
  }
  SECTION("Old string layout is rejected") {
    metadata = podio::Frame{};
    metadata.putParameter(c4h::parameterSetVersionKey, 1);
    metadata.putParameter(c4h::parameterSetIDsKey, std::vector<std::string>{id});
    metadata.putParameter(c4h::parameterSetBlobsKey, std::vector<std::string>{blob});
    metadata.putParameter(c4h::parameterSetLengthsKey, static_cast<int>(blob.size()));
  }
  SECTION("Mismatched lengths") {
    auto const idsLonger = GENERATE(false, true);
    if (idsLonger)
      metadata.putParameter(c4h::parameterSetLengthsKey, std::vector<int>{});
    else
      metadata.putParameter(c4h::parameterSetIDsKey, std::vector<int>{});
  }
  SECTION("Invalid ID after a valid entry") {
    auto const invalidID = GENERATE(edm::ParameterSetID{}.compactForm(), std::string(12, 'a'), std::string(17, 'a'));
    metadata = metadataFrame({id, invalidID}, {blob, blob});
  }
  SECTION("Truncated ID words") {
    metadata.putParameter(c4h::parameterSetIDsKey, std::vector<int>{1, 2, 3});
  }
  SECTION("Invalid lengths") {
    auto const length = GENERATE(0, 1, -1, std::numeric_limits<int>::min(), std::numeric_limits<int>::max());
    metadata.putParameter(c4h::parameterSetLengthsKey, length);
  }
  SECTION("Length sum exceeds available words") {
    metadata = metadataFrame({id, id}, {blob, blob});
    metadata.putParameter(c4h::parameterSetLengthsKey,
                          std::vector<int>{static_cast<int>(blob.size()), std::numeric_limits<int>::max()});
  }
  SECTION("Extra blob words") {
    auto words = packedWords({blob});
    words.push_back(0);
    metadata.putParameter(c4h::parameterSetBlobsKey, std::move(words));
  }
  SECTION("Truncated blob words") {
    auto words = packedWords({blob});
    words.pop_back();
    metadata.putParameter(c4h::parameterSetBlobsKey, std::move(words));
  }
  SECTION("Nonzero padding") {
    metadata = metadataFrame({id}, {"<>"});
    metadata.putParameter(c4h::parameterSetBlobsKey, std::vector<int>{0x013e3c});
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

TEST_CASE("Compact ParameterSet IDs preserve binary bytes through ROOT metadata", s_tag) {
  std::string id(16, '\0');
  id[3] = std::bit_cast<char>(static_cast<unsigned char>(0x80));
  id[4] = id[5] = id[6] = std::bit_cast<char>(static_cast<unsigned char>(0xff));
  id[7] = 0x7f;
  id[8] = std::bit_cast<char>(static_cast<unsigned char>(0xff));
  auto metadata = metadataFrame({id}, {"<>"});
  REQUIRE(metadata.getParameters().getMap<int>().at(c4h::parameterSetIDsKey) ==
          std::vector<int>{std::numeric_limits<int>::min(), std::numeric_limits<int>::max(), 255, 0});
  const std::string fileName = "parameterSetMetadataBinary.root";
  {
    auto writer = podio::makeWriter(fileName, "ROOT");
    writer.writeFrame(metadata, podio::Category::Metadata);
    writer.finish();
  }
  {
    auto reader = podio::makeReader(fileName);
    auto restored = reader.readFrame(podio::Category::Metadata, 0);
    REQUIRE(restored.getParameters().getMap<int>() == metadata.getParameters().getMap<int>());
    auto const ids = c4h::readParameterSetIDs(restored);
    REQUIRE(ids.size() == 1);
    REQUIRE(ids.front().compactForm() == id);
    REQUIRE_NOTHROW(c4h::restoreParameterSets(restored));
    REQUIRE(edm::getParameterSet(ids.front()).toString() == "<>");
  }
  std::filesystem::remove(fileName);
}

TEST_CASE("Packed blob boundary lengths survive ROOT and validate before parsing", s_tag) {
  auto const length = GENERATE(0, 1, 3, 4, 5);
  std::string const bytes("\0\x80\xff;>", 5);
  auto const blob = bytes.substr(0, length);
  auto const id = blobID("parameterSetMetadataBoundary:" + blob);
  auto metadata = metadataFrame({id}, {blob});
  auto const& ints = metadata.getParameters().getMap<int>();
  REQUIRE(ints.at(c4h::parameterSetLengthsKey) == std::vector<int>{length});
  REQUIRE(ints.at(c4h::parameterSetBlobsKey).size() == static_cast<std::size_t>((length + 3) / 4));
  if (length == 0)
    REQUIRE(ints.at(c4h::parameterSetBlobsKey).empty());
  if (length == 1)
    REQUIRE(ints.at(c4h::parameterSetBlobsKey) == std::vector<int>{0});
  if (length == 3)
    REQUIRE(ints.at(c4h::parameterSetBlobsKey) == std::vector<int>{0x00ff8000});
  if (length == 4)
    REQUIRE(ints.at(c4h::parameterSetBlobsKey) == std::vector<int>{0x3bff8000});
  if (length == 5)
    REQUIRE(ints.at(c4h::parameterSetBlobsKey) == std::vector<int>{0x3bff8000, 0x3e});
  std::string const fileName = "parameterSetMetadataBoundaries.root";
  {
    auto writer = podio::makeWriter(fileName, "ROOT");
    writer.writeFrame(metadata, podio::Category::Metadata);
    writer.finish();
  }
  {
    auto reader = podio::makeReader(fileName);
    auto restored = reader.readFrame(podio::Category::Metadata, 0);
    REQUIRE(restored.getParameters().getMap<int>() == ints);
    REQUIRE(c4h::readParameterSetIDs(restored) == std::vector<edm::ParameterSetID>{edm::ParameterSetID(id)});
    auto const size = edm::pset::Registry::instance()->size();
    requireFormatError(restored);  // These arbitrary bytes are not serialized PSets.
    REQUIRE(edm::pset::Registry::instance()->size() == size);
  }
  std::filesystem::remove(fileName);
}

TEST_CASE("Raw Z ParameterSet strings preserve NULs and delimiters through packed ROOT metadata", s_tag) {
  std::string const value("\0;<>(),=\0\x80\xff", 11);
  edm::ParameterSet pset;
  pset.addParameter<std::string>("parameterSetMetadataBinaryValue", value);
  auto const blob = pset.toString();
  REQUIRE(blob.find("=+Z(") != std::string::npos);
  REQUIRE(blob.find('\0') != std::string::npos);
  auto const id = blobID(blob);
  auto& registry = *edm::pset::Registry::instance();
  REQUIRE(registry.getMapped(edm::ParameterSetID(id)) == nullptr);
  auto metadata = metadataFrame({id}, {blob});
  std::string const fileName = "parameterSetMetadataRawString.root";
  {
    auto writer = podio::makeWriter(fileName, "ROOT");
    writer.writeFrame(metadata, podio::Category::Metadata);
    writer.finish();
  }
  {
    auto reader = podio::makeReader(fileName);
    auto restored = reader.readFrame(podio::Category::Metadata, 0);
    REQUIRE(restored.getParameters().getMap<int>() == metadata.getParameters().getMap<int>());
    REQUIRE_NOTHROW(c4h::restoreParameterSets(restored));
    auto const& result = edm::getParameterSet(edm::ParameterSetID(id));
    REQUIRE(result.toString() == blob);
    REQUIRE(result.getParameter<std::string>("parameterSetMetadataBinaryValue") == value);
  }
  // Exercise the production writer, not just the handcrafted packed fixture.
  auto const saved = c4h::saveParameterSets();
  auto const ids = c4h::readParameterSetIDs(saved);
  auto const found = std::find(ids.begin(), ids.end(), edm::ParameterSetID(id));
  REQUIRE(found != ids.end());
  auto const index = static_cast<std::size_t>(std::distance(ids.begin(), found));
  auto const& ints = saved.getParameters().getMap<int>();
  auto const& lengths = ints.at(c4h::parameterSetLengthsKey);
  REQUIRE(lengths[index] == static_cast<int>(blob.size()));
  std::size_t offset = 0;
  for (std::size_t i = 0; i != index; ++i)
    offset += static_cast<std::size_t>(lengths[i]) / 4 + (lengths[i] % 4 != 0);
  auto const expected = packedWords({blob});
  REQUIRE(std::equal(expected.begin(), expected.end(), ints.at(c4h::parameterSetBlobsKey).begin() + offset));
  {
    auto writer = podio::makeWriter(fileName, "ROOT");
    writer.writeFrame(saved, podio::Category::Metadata);
    writer.finish();
  }
  {
    auto reader = podio::makeReader(fileName);
    auto restored = reader.readFrame(podio::Category::Metadata, 0);
    REQUIRE(restored.getParameters().getMap<int>() == ints);
    REQUIRE_NOTHROW(c4h::restoreParameterSets(restored));
    REQUIRE(edm::getParameterSet(edm::ParameterSetID(id)).getParameter<std::string>(
                "parameterSetMetadataBinaryValue") == value);
  }
  std::filesystem::remove(fileName);
}

TEST_CASE("Empty packed registry metadata is valid", s_tag) {
  auto const metadata = metadataFrame({}, {});
  auto const size = edm::pset::Registry::instance()->size();
  REQUIRE(c4h::readParameterSetIDs(metadata).empty());
  REQUIRE_NOTHROW(c4h::restoreParameterSets(metadata));
  REQUIRE(edm::pset::Registry::instance()->size() == size);
}

TEST_CASE("File PSet membership validates the shared packed schema", s_tag) {
  auto metadata = metadataFrame({blobID("parameterSetMetadataMembership")}, {"<>"});
  SECTION("Missing lengths") {
    metadata = podio::Frame{};
    metadata.putParameter(c4h::parameterSetVersionKey, 1);
    metadata.putParameter(c4h::parameterSetIDsKey, std::vector<int>{});
    metadata.putParameter(c4h::parameterSetBlobsKey, std::vector<int>{});
  }
  SECTION("Blob present only with wrong type") {
    metadata = podio::Frame{};
    metadata.putParameter(c4h::parameterSetVersionKey, 1);
    metadata.putParameter(c4h::parameterSetIDsKey, std::vector<int>{});
    metadata.putParameter(c4h::parameterSetLengthsKey, std::vector<int>{});
    metadata.putParameter(c4h::parameterSetBlobsKey, "<>");
  }
  SECTION("Wrong ID count") {
    metadata.putParameter(c4h::parameterSetIDsKey, std::vector<int>{1, 2, 3});
  }
  SECTION("Negative length") {
    metadata.putParameter(c4h::parameterSetLengthsKey, std::numeric_limits<int>::min());
  }
  SECTION("Too many blob words") {
    metadata.putParameter(c4h::parameterSetBlobsKey, std::vector<int>{0x3e3c, 0});
  }
  SECTION("Invalid ID") {
    metadata.putParameter(c4h::parameterSetIDsKey, packedWords({edm::ParameterSetID{}.compactForm()}));
  }
  auto const size = edm::pset::Registry::instance()->size();
  try {
    c4h::readParameterSetIDs(metadata);
    FAIL("Malformed membership metadata was accepted");
  } catch (edm::Exception const& ex) {
    REQUIRE(ex.categoryCode() == edm::errors::FormatIncompatibility);
  }
  REQUIRE(edm::pset::Registry::instance()->size() == size);
}

TEST_CASE("Parallel noninteger PSet metadata names are ignored", s_tag) {
  auto const key = GENERATE(c4h::parameterSetVersionKey, c4h::parameterSetIDsKey,
                            c4h::parameterSetBlobsKey, c4h::parameterSetLengthsKey);
  auto metadata = metadataFrame({}, {});
  metadata.putParameter(key, "unrelated");
  metadata.putParameter(key, 2.f);
  metadata.putParameter(key, 3.);
  auto const size = edm::pset::Registry::instance()->size();
  REQUIRE(c4h::readParameterSetIDs(metadata).empty());
  REQUIRE_NOTHROW(c4h::restoreParameterSets(metadata));
  REQUIRE(edm::pset::Registry::instance()->size() == size);
}
