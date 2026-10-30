#include <bit>
#include <filesystem>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "catch2/catch_all.hpp"
#include "podio/FrameCategories.h"
#include "podio/Writer.h"

#include "FWCore/ParameterSet/interface/ParameterSet.h"
#include "FWCore/ParameterSet/interface/Registry.h"
#include "FWCore/Utilities/interface/Digest.h"
#include "FWCore/Utilities/interface/FriendlyName.h"

#include "../bin/ProvDump.h"
#include "../plugins/ParameterSetMetadata.h"
#include "ParameterSetMetadataTestHelpers.h"
#include "../plugins/ProcessHistoryMetadata.h"
#include "../plugins/ProductDescriptionMetadata.h"
#include "../plugins/ParentageMetadata.h"

namespace {
  constexpr auto tag = "[ProvDump]";

  struct Result {
    int status;
    std::string out;
    std::string err;
  };

  Result run(std::vector<std::string> const& args) {
    std::vector<char const*> argv{"c4hProvDump"};
    for (auto const& arg : args)
      argv.push_back(arg.c_str());
    std::ostringstream out, err;
    int const status = c4h::provDump(static_cast<int>(argv.size()), argv.data(), out, err);
    return {status, out.str(), err.str()};
  }

  struct File {
    std::string name = "provDumpMetadata.root";
    ~File() { std::filesystem::remove(name); }

    void write(podio::Frame const& metadata, int count = 1) const {
      auto writer = podio::makeWriter(name, "ROOT");
      for (int i = 0; i != count; ++i)
        writer.writeFrame(metadata, podio::Category::Metadata);
      if (count == 1) {
        auto dictionary = c4h::ParentageDictionary::restore(metadata);
        if (dictionary) {
          podio::Frame event;
          c4h::saveEventParentage(event, c4h::EventParentage{}, *dictionary);
          writer.writeEvent(event);
        }
      }
      if (count == 0) {
        podio::Frame event;
        event.putParameter("ordinary", 1);
        writer.writeFrame(event, podio::Category::Event);
      }
      writer.finish();
    }
  };

  void addProduct(podio::Frame& metadata, std::string const& label = "producer") {
    c4h::ParentageDictionary{}.save(metadata);
    // Deliberately use a class without a converter: inspection needs no plugin.
    std::string const type = "example::StoredCollection";
    auto const friendly = edm::friendlyname::friendlyName(type);
    edm::BranchID const id(friendly + "_" + label + "_items_SIM.");
    auto const text = std::to_string(id.id());
    auto const key = c4h::productDescriptionPrefix + text;
    metadata.putParameter(c4h::productDescriptionVersionKey, 1);
    auto ids = metadata.getParameter<std::vector<std::string>>(c4h::productDescriptionIDsKey)
                   .value_or(std::vector<std::string>{});
    ids.push_back(text);
    metadata.putParameter(c4h::productDescriptionIDsKey, ids);
    metadata.putParameter(key, std::vector<std::string>{"0", label, "SIM", "items", type, friendly, "0",
                                                       label + "items"});
    metadata.putParameter(key + ".aliases", std::vector<std::string>{"publicItems"});
  }
}

TEST_CASE("Provenance dump validates command line before opening files", tag) {
  auto help = run({"--help"});
  REQUIRE(help.status == 0);
  REQUIRE(help.out.find("--dumpPSetID") != std::string::npos);
  REQUIRE(help.out.find("--showAllModules") != std::string::npos);
  REQUIRE(help.out.find("AND matching") != std::string::npos);
  REQUIRE(help.out.find(c4h::noRecordedParents) != std::string::npos);
  REQUIRE(help.err.empty());
  REQUIRE(run({}).status == 1);
  REQUIRE(run({"--unknown"}).status == 1);
  REQUIRE(run({"one.root", "two.root"}).status == 1);
  for (auto const& id : {std::string(16, 'a'), std::string(31, 'a'), std::string(32, 'g')}) {
    auto result = run({"missing.root", "-i", id});
    REQUIRE(result.status == 1);
    REQUIRE(result.err.find("32 hexadecimal") != std::string::npos);
  }
}

TEST_CASE("Plain podio and metadata cardinality are handled explicitly", tag) {
  File file;
  podio::Frame metadata;
  metadata.putParameter("ordinary", "metadata");
  SECTION("No metadata category") { file.write(metadata, 0); }
  SECTION("Unrelated metadata") { file.write(metadata); }
  SECTION("Multiple metadata frames") {
    file.write(metadata, 2);
    auto result = run({file.name});
    REQUIRE(result.status == 1);
    REQUIRE(result.err.find("Expected one") != std::string::npos);
    return;
  }
  auto result = run({file.name});
  REQUIRE(result.status == 0);
  REQUIRE(result.out.find("no persisted Code4hep") != std::string::npos);
  auto missing = run({file.name, "-i", std::string(32, '1')});
  REQUIRE(missing.status == 1);
  REQUIRE(missing.err.find("not available") != std::string::npos);
}

TEST_CASE("Synthetic provenance prints stored identities and ordered recursive configurations", tag) {
  edm::ParameterSet nested;
  nested.addParameter<int>("nestedValue", 731);
  edm::ParameterSet module;
  module.addParameter<std::string>("@module_type", "ExampleProducer");
  module.addParameter<edm::ParameterSet>("nested", nested);
  edm::ParameterSet es;
  es.addParameter<int>("esValue", 19);
  edm::ParameterSet extra;
  extra.addParameter<int>("extraValue", 23);
  edm::ParameterSet process;
  process.addParameter<edm::ParameterSet>("producer", module);
  process.addParameter<edm::ParameterSet>("esProducer", es);
  process.addParameter<edm::ParameterSet>("unused", extra);
  process.addParameter<edm::ParameterSet>("settings", extra);
  process.addParameter<edm::ParameterSet>("@main_input", extra);
  process.addParameter<edm::ParameterSet>("@trigger_paths", extra);
  process.addParameter<edm::ParameterSet>("@internal", extra);
  process.addParameter<std::vector<edm::ParameterSet>>("userVector", {extra});
  process.addParameter<std::vector<edm::ParameterSet>>("@internalVector", {extra});
  for (auto const& name : {"inputComponent", "looperComponent", "preferComponent", "aliasComponent"})
    process.addParameter<edm::ParameterSet>(name, extra);
  process.addParameter<std::vector<std::string>>("@all_sources", {"inputComponent"});
  process.addParameter<std::vector<std::string>>("@all_loopers", {"looperComponent"});
  process.addParameter<std::vector<std::string>>("@all_esprefers", {"preferComponent"});
  process.addParameter<std::vector<std::string>>("@all_aliases", {"aliasComponent"});
  process.addParameter<std::vector<std::string>>("@all_modules", {"producer", "unused"});
  process.addParameter<std::vector<std::string>>("@all_esmodules", {"esProducer"});
  process.addParameter<std::vector<std::string>>("@all_essources", {});
  process.registerIt();
  auto metadata = c4h::saveParameterSets();
  addProduct(metadata);
  addProduct(metadata, "source");
  addProduct(metadata, "TriggerResults");
  edm::HardwareResourcesDescription hardware;
  hardware.microarchitecture = "test-architecture";
  edm::ProcessHistory history;
  history.emplace_back("SIM", process.id(), "release-first", hardware);
  history.emplace_back("NEXT", process.id(), "release-second", edm::HardwareResourcesDescription{});
  c4h::saveProcessHistories(metadata, {{history.id(), history}});
  File file;
  file.write(metadata);
  auto result = run({file.name, "--showHistoryID"});
  REQUIRE(result.status == 0);
  REQUIRE(result.err.empty());
  REQUIRE(result.out.find("produceritems") != std::string::npos);
  REQUIRE(result.out.find("publicItems") != std::string::npos);
  REQUIRE(result.out.find("BranchID:") != std::string::npos);
  REQUIRE(result.out.find("ProcessConfigurationID:") != std::string::npos);
  REQUIRE(result.out.find("release-first") < result.out.find("release-second"));
  REQUIRE(result.out.find("test-architecture") != std::string::npos);
  REQUIRE(result.out.find("nestedValue") != std::string::npos);
  REQUIRE(result.out.find("Module esProducer") != std::string::npos);
  REQUIRE(result.out.find("Module unused") == std::string::npos);
  REQUIRE(result.out.find("Module source") != std::string::npos);
  REQUIRE(result.out.find("Module TriggerResults") != std::string::npos);
  auto dependencies = run({file.name, "-d"});
  REQUIRE(dependencies.status == 0);
  REQUIRE(dependencies.out.find("unknown boundary: no provenance record") != std::string::npos);

  auto all = run({file.name, "-p", "-a", "-e", "-t", "-s"});
  REQUIRE(all.status == 0);
  REQUIRE(all.out.find("BranchID:") == std::string::npos);
  REQUIRE(all.out.find("Module producer") != std::string::npos);
  REQUIRE(all.out.find("Module unused") != std::string::npos);
  REQUIRE(all.out.find("Module esProducer") == std::string::npos);
  REQUIRE(all.out.find("Top-level PSet settings") != std::string::npos);
  REQUIRE(all.out.find("Top-level VPSet userVector") != std::string::npos);
  REQUIRE(all.out.find("Top-level PSet @") == std::string::npos);
  REQUIRE(all.out.find("Top-level VPSet @") == std::string::npos);
  for (auto const& name : {"inputComponent", "looperComponent", "preferComponent", "aliasComponent"})
    REQUIRE(all.out.find(std::string("Top-level PSet ") + name) == std::string::npos);

  auto filtered = run({file.name, "-f", "producer", "-f", "SIM", "-e"});
  REQUIRE(filtered.status == 0);
  REQUIRE(filtered.out.find("produceritems") != std::string::npos);
  REQUIRE(filtered.out.find("nestedValue") != std::string::npos);
  REQUIRE(filtered.out.find("esValue") == std::string::npos);
  REQUIRE(filtered.out.find("Module source") == std::string::npos);
  REQUIRE(filtered.out.find("Module TriggerResults") == std::string::npos);
  auto noMatch = run({file.name, "-f", "produceritems", "-f", "nestedValue", "-e"});
  REQUIRE(noMatch.status == 0);
  REQUIRE(noMatch.out.find("BranchID:") == std::string::npos);
  REQUIRE(noMatch.out.find("Module producer") == std::string::npos);
  std::ostringstream id;
  id << process.id();
  auto dump = run({file.name, "-i", id.str()});
  REQUIRE(dump.status == 0);
  REQUIRE(dump.out.find("nestedValue") != std::string::npos);
  REQUIRE(dump.out.find("BranchID:") == std::string::npos);
}

TEST_CASE("Products survive absent process PSets and missing nested PSets are diagnostic", tag) {
  File file;
  podio::Frame metadata;
  SECTION("Absent process PSet") {
    addProduct(metadata);
    edm::ProcessHistory history;
    history.emplace_back("SIM", edm::ParameterSetID(std::string(32, '3')), "test-release", edm::HardwareResourcesDescription{});
    c4h::saveProcessHistories(metadata, {{history.id(), history}});
    file.write(metadata);
    auto result = run({file.name});
    REQUIRE(result.status == 1);
    REQUIRE(result.out.find("produceritems") != std::string::npos);
    REQUIRE(result.err.find("Missing process ParameterSet") != std::string::npos);
  }
  SECTION("Missing nested PSet") {
    std::string const blob = "<child=+Q(55555555555555555555555555555555)>";
    cms::Digest digest(blob);
    auto const id = digest.digest();
    metadata = c4h::test::metadataFrame({id.compactForm()}, {blob});
    file.write(metadata);
    std::ostringstream text;
    text << id;
    auto result = run({file.name, "-i", text.str()});
    REQUIRE(result.status == 1);
    REQUIRE(result.out.find("unavailable") != std::string::npos);
  }
}

TEST_CASE("PSet dump uses packed binary IDs and file membership", tag) {
  File file;
  std::string binaryID(16, '\0');
  binaryID[3] = std::bit_cast<char>(static_cast<unsigned char>(0x80));
  binaryID[7] = std::bit_cast<char>(static_cast<unsigned char>(0xff));
  edm::ParameterSetID const id(binaryID);
  auto const metadata = c4h::test::metadataFrame({id.compactForm()}, {"<binaryDumpValue=+I(+42)>"});
  file.write(metadata);
  std::string text;
  id.toString(text);
  auto const result = run({file.name, "-i", text});
  REQUIRE(result.status == 0);
  REQUIRE(result.out.find("binaryDumpValue") != std::string::npos);

  edm::ParameterSet other;
  other.addParameter<int>("parameterSetMetadataNotInDumpFile", 987654);
  other.registerIt();
  text.clear();
  other.id().toString(text);
  REQUIRE(edm::pset::Registry::instance()->getMapped(other.id()) != nullptr);
  auto const missing = run({file.name, "-i", text});
  REQUIRE(missing.status == 1);
  REQUIRE(missing.err.find("not available in this file") != std::string::npos);
}

TEST_CASE("Stored product decoding rejects malformed identities without converters", tag) {
  podio::Frame metadata;
  REQUIRE_FALSE(c4h::restoreStoredProductDescriptions(metadata));
  addProduct(metadata);
  auto stored = c4h::restoreStoredProductDescriptions(metadata);
  REQUIRE(stored.has_value());
  REQUIRE(stored->size() == 1);
  SECTION("Invalid numeric ID") {
    metadata.putParameter(c4h::productDescriptionIDsKey, std::vector<std::string>{"01"});
  }
  SECTION("Invalid shape") {
    auto const key = c4h::productDescriptionPrefix + std::to_string(stored->begin()->first.id());
    metadata.putParameter(key, std::vector<std::string>{"0"});
  }
  REQUIRE_THROWS(c4h::restoreStoredProductDescriptions(metadata));
}

TEST_CASE("Dependency dump is event scoped and distinguishes absent records from no recorded parents", tag) {
  File file;
  podio::Frame metadata;
  addProduct(metadata);
  auto products = *c4h::restoreStoredProductDescriptions(metadata);
  auto root = products.begin()->first;
  edm::BranchID const ancestor(123), boundary(456);
  edm::Parentage rootParents(std::vector<edm::BranchID>{ancestor, boundary});
  c4h::ParentageDictionary dictionary;
  dictionary.insert(rootParents.id(), rootParents);
  dictionary.save(metadata);
  c4h::EventParentage first, second, absent;
  first.records = {edm::ProductProvenance(root, rootParents.id()), edm::ProductProvenance(ancestor, edm::Parentage{}.id())};
  second.records = {edm::ProductProvenance(root)};
  {
    auto writer = podio::makeWriter(file.name, "ROOT");
    writer.writeFrame(metadata, podio::Category::Metadata);
    for (auto const& snapshot : {first, second, absent}) {
      podio::Frame event;
      c4h::saveEventParentage(event, snapshot, dictionary);
      writer.writeEvent(event);
    }
    // Keep the parameter layout while making only this event's schema invalid.
    // File-only and event-0 reads must not scan it.
    podio::Frame malformed;
    c4h::saveEventParentage(malformed, absent, dictionary);
    malformed.putParameter(c4h::eventParentageVersionKey, 2);
    writer.writeEvent(malformed);
    writer.finish();
  }
  auto immediate = run({file.name, "-d"});
  REQUIRE(immediate.status == 0);
  REQUIRE(immediate.out.find("event index 0") != std::string::npos);
  REQUIRE(immediate.out.find(c4h::noRecordedParents) != std::string::npos);
  REQUIRE(immediate.out.find("unknown boundary: no provenance record") != std::string::npos);
  auto extended = run({file.name, "-x", "-c"});
  REQUIRE(extended.status == 0);
  REQUIRE(extended.out.find("Ancestors:") != std::string::npos);
  REQUIRE(extended.out.find("Descendants") != std::string::npos);
  auto noParents = run({file.name, "-d", "--event", "1"});
  REQUIRE(noParents.status == 0);
  REQUIRE(noParents.out.find(c4h::noRecordedParents) != std::string::npos);
  REQUIRE(noParents.out.find("BranchID 123") == std::string::npos);
  auto noRecord = run({file.name, "-d", "--event", "2"});
  REQUIRE(noRecord.status == 0);
  REQUIRE(noRecord.out.find("unknown boundary: no provenance record") != std::string::npos);
  REQUIRE(noRecord.out.find(c4h::noRecordedParents) == std::string::npos);
  REQUIRE(run({file.name}).status == 0);
  REQUIRE(run({file.name, "-d", "--event", "3"}).status == 1);
  REQUIRE(run({file.name, "-d", "--event", "4"}).status == 1);
}

TEST_CASE("Code4hep product metadata requires the parentage schema", tag) {
  File file;
  podio::Frame metadata;
  metadata.putParameter(c4h::productDescriptionVersionKey, 1);
  metadata.putParameter(c4h::productDescriptionIDsKey, std::vector<std::string>{});
  file.write(metadata);
  auto result = run({file.name});
  REQUIRE(result.status == 1);
  REQUIRE(result.err.find("Missing Code4hep parentage dictionary") != std::string::npos);
}
