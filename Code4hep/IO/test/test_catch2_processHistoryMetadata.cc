#include "../plugins/ProcessHistoryMetadata.h"
#include "catch2/catch_all.hpp"

#include <algorithm>
#include <iterator>
#include <map>
#include <string>
#include <utility>
#include <vector>

static constexpr auto s_tag = "[ProcessHistoryMetadata]";

namespace {
  edm::ProcessHistory makeHistory(std::string const& name) {
    edm::HardwareResourcesDescription hardware;
    hardware.microarchitecture = "x86-64-v3";
    hardware.cpuModels = {"CPU model A", "CPU model B"};
    hardware.selectedAccelerators = {"cpu", "cuda"};
    hardware.gpuModels = {"GPU model A"};
    edm::ProcessHistory history;
    history.emplace_back(name, edm::ParameterSetID(std::string(32, '1')), "CMSSW_14_2_1", hardware);
    history.emplace_back("NEXT", edm::ParameterSetID(std::string(32, '2')), "CMSSW_14_2_2",
                         edm::HardwareResourcesDescription{});
    return history;
  }

  std::string historyKey(std::string const& id) { return std::string(c4h::processHistoriesPrefix) + id; }

  template <typename F>
  void requireFormatError(F&& action) {
    try {
      action();
    } catch (edm::Exception const& ex) {
      REQUIRE(ex.categoryCode() == edm::errors::FormatIncompatibility);
      return;
    }
    FAIL("Malformed ProcessHistory metadata was accepted");
  }
}  // namespace

TEST_CASE("Multiple ProcessHistories preserve configuration order, hardware and empty history", s_tag) {
  auto const first = makeHistory("FIRST");
  auto const second = makeHistory("SECOND");
  edm::ProcessHistory const empty;
  std::map<edm::ProcessHistoryID, edm::ProcessHistory> histories{
      {first.id(), first}, {second.id(), second}, {empty.id(), empty}};
  podio::Frame frame;
  c4h::putProcessHistories(frame, histories);
  REQUIRE(frame.getAvailableCollections().empty());
  REQUIRE(frame.getParameter<std::vector<int>>(c4h::processHistoriesVersionKey) == std::vector<int>{1});
  std::vector<std::string> expectedIDs;
  for (auto const& [id, history] : histories) {
    expectedIDs.push_back(c4h::historyIDString(id));
    std::vector<std::string> fields;
    for (auto const& config : history) {
      std::string psetID;
      config.parameterSetID().toString(psetID);
      fields.insert(fields.end(), {config.processName(), psetID, config.releaseVersion(),
                                   config.hardwareResourcesDescriptionSerialized()});
    }
    REQUIRE(frame.getParameter<std::vector<std::string>>(historyKey(expectedIDs.back())) == fields);
  }
  REQUIRE(frame.getParameter<std::vector<std::string>>(c4h::processHistoriesIDsKey) == expectedIDs);
  edm::ProcessHistoryRegistry registry;
  REQUIRE(c4h::restoreProcessHistories(frame, registry));
  REQUIRE(std::distance(registry.begin(), registry.end()) == 3);
  for (auto const& [id, history] : histories) {
    auto const* restored = registry.getMapped(id);
    REQUIRE(restored != nullptr);
    REQUIRE(*restored == history);
    REQUIRE(restored->id() == id);
  }
  REQUIRE(c4h::restoreProcessHistories(frame, registry));
  REQUIRE(std::distance(registry.begin(), registry.end()) == 3);
}

TEST_CASE("Default ProcessHistory ID encodes the computed empty history ID", s_tag) {
  edm::ProcessHistory const empty;
  REQUIRE(c4h::historyIDString(edm::ProcessHistoryID{}) == c4h::historyIDString(empty.id()));
  REQUIRE(c4h::historyIDString(empty.id()) == "d41d8cd98f00b204e9800998ecf8427e");
  podio::Frame frame;
  c4h::putProcessHistories(frame, {{edm::ProcessHistoryID{}, empty}});
  REQUIRE(frame.getParameter<std::vector<std::string>>(historyKey(c4h::historyIDString(empty.id()))) ==
          std::vector<std::string>{});
  edm::ProcessHistoryRegistry registry;
  REQUIRE(c4h::restoreProcessHistories(frame, registry));
  REQUIRE(registry.getMapped(empty.id()) != nullptr);
  frame.putParameter(c4h::eventProcessHistoryKey, c4h::historyIDString(edm::ProcessHistoryID{}));
  REQUIRE(c4h::readHistoryID(frame, c4h::eventProcessHistoryKey, registry) == empty.id());
}

TEST_CASE("Entirely absent ProcessHistory metadata returns false, an empty snapshot returns true", s_tag) {
  podio::Frame frame;
  edm::ProcessHistoryRegistry registry;
  auto const existing = makeHistory("EXISTING");
  registry.registerProcessHistory(existing);
  SECTION("Absent") {}
  SECTION("Unrelated parameters") {
    frame.putParameter("unrelated", 1);
    frame.putParameter(c4h::eventProcessHistoryKey, c4h::historyIDString(existing.id()));
  }
  SECTION("Empty snapshot") { c4h::putProcessHistories(frame, {}); }
  auto const hasSnapshot = frame.getParameter<std::vector<int>>(c4h::processHistoriesVersionKey).has_value();
  REQUIRE(c4h::restoreProcessHistories(frame, registry) == hasSnapshot);
  REQUIRE(std::distance(registry.begin(), registry.end()) == 1);
  REQUIRE(registry.getMapped(existing.id()) != nullptr);
}

TEST_CASE("Malformed ProcessHistory snapshots never partially register histories", s_tag) {
  auto const history = makeHistory("NOT_INSERTED");
  auto const id = c4h::historyIDString(history.id());
  auto const key = historyKey(id);
  podio::Frame frame;
  c4h::putProcessHistories(frame, {{history.id(), history}});
  auto fields = *frame.getParameter<std::vector<std::string>>(key);
  edm::ProcessHistoryRegistry registry;
  auto const existing = makeHistory("KEEP_EXISTING");
  registry.registerProcessHistory(existing);

  SECTION("Missing schema keys") {
    auto const missing = GENERATE(0, 1, 2);
    frame = podio::Frame{};
    if (missing != 0)
      frame.putParameter(c4h::processHistoriesVersionKey, 1);
    if (missing != 1)
      frame.putParameter(c4h::processHistoriesIDsKey, std::vector<std::string>{id});
    if (missing != 2)
      frame.putParameter(key, fields);
  }
  SECTION("Orphan history alone") {
    frame = podio::Frame{};
    frame.putParameter(key, fields);
  }
  SECTION("Unsupported version") { frame.putParameter(c4h::processHistoriesVersionKey, GENERATE(0, 2)); }
  SECTION("Version cardinality") {
    frame.putParameter(c4h::processHistoriesVersionKey, GENERATE(std::vector<int>{}, std::vector<int>{1, 1}));
  }
  SECTION("Wrong types") {
    auto const wrong = GENERATE(0, 1, 2);
    frame = podio::Frame{};
    if (wrong == 0)
      frame.putParameter(c4h::processHistoriesVersionKey, "1");
    else
      frame.putParameter(c4h::processHistoriesVersionKey, 1);
    if (wrong == 1)
      frame.putParameter(c4h::processHistoriesIDsKey, 1.f);
    else
      frame.putParameter(c4h::processHistoriesIDsKey, std::vector<std::string>{id});
    if (wrong == 2)
      frame.putParameter(key, 1.);
    else
      frame.putParameter(key, fields);
  }
  SECTION("Flattened field lengths") {
    fields.resize(GENERATE(1, 2, 3, 5, 7));
    frame.putParameter(key, fields);
  }
  SECTION("Malformed history ID after a valid entry") {
    auto const invalid = GENERATE(std::string{}, std::string(31, 'a'), std::string(33, 'a'), std::string(32, 'g'),
                                  std::string(32, '0'));
    frame.putParameter(c4h::processHistoriesIDsKey, std::vector<std::string>{id, invalid});
  }
  SECTION("Missing history after a valid entry") {
    frame.putParameter(c4h::processHistoriesIDsKey, std::vector<std::string>{id, std::string(32, '3')});
  }
  SECTION("ID mismatch after a valid entry") {
    auto const otherID = std::string(32, '3');
    frame.putParameter(c4h::processHistoriesIDsKey, std::vector<std::string>{id, otherID});
    frame.putParameter(historyKey(otherID), fields);
  }
  SECTION("Configuration order changes ID") {
    std::rotate(fields.begin(), fields.begin() + 4, fields.end());
    frame.putParameter(key, fields);
  }
  SECTION("Invalid ParameterSetID") {
    fields[1] = GENERATE(std::string{}, std::string(31, '1'), std::string(32, 'z'));
    frame.putParameter(key, fields);
  }
  SECTION("Malformed or non-round-trippable hardware") {
    auto const trailing = GENERATE(false, true);
    fields[3] = trailing ? fields[3] + "trailing data" : "not hardware";
    frame.putParameter(key, fields);
  }
  SECTION("Malformed hardware after a valid history") {
    auto const other = makeHistory("BAD_HARDWARE");
    auto const otherID = c4h::historyIDString(other.id());
    frame.putParameter(c4h::processHistoriesIDsKey, std::vector<std::string>{id, otherID});
    fields[0] = "BAD_HARDWARE";
    fields[3] += "trailing data";
    frame.putParameter(historyKey(otherID), fields);
  }
  SECTION("Duplicate IDs") {
    frame.putParameter(c4h::processHistoriesIDsKey, std::vector<std::string>{id, id});
  }
  SECTION("Unlisted history") { frame.putParameter(historyKey(std::string(32, '3')), fields); }
  SECTION("Conflicting key types") { frame.putParameter(key, 1); }

  requireFormatError([&] { c4h::restoreProcessHistories(frame, registry); });
  REQUIRE(std::distance(registry.begin(), registry.end()) == 1);
  REQUIRE(registry.getMapped(existing.id()) != nullptr);
  REQUIRE(registry.getMapped(history.id()) == nullptr);
}

TEST_CASE("Writing verifies map IDs and recomputes cached history IDs", s_tag) {
  auto history = makeHistory("WRITE_VALIDATION");
  auto const id = history.setProcessHistoryID();
  podio::Frame frame;
  SECTION("Mismatched map ID") {
    requireFormatError([&] { c4h::putProcessHistories(frame, {{edm::ProcessHistoryID(std::string(32, '3')), history}}); });
  }
  SECTION("Stale cached ID") {
    history[0] = edm::ProcessConfiguration("CHANGED", edm::ParameterSetID(std::string(32, '1')), "CMSSW_14_2_1",
                                           edm::HardwareResourcesDescription{});
    REQUIRE(history.id() == id);
    requireFormatError([&] { c4h::putProcessHistories(frame, {{id, history}}); });
  }
  SECTION("Default ID with nonempty history") {
    requireFormatError([&] { c4h::putProcessHistories(frame, {{edm::ProcessHistoryID{}, history}}); });
  }
  REQUIRE(frame.getParameters().empty());
}

TEST_CASE("ProcessHistory references require exactly one hexadecimal registered ID", s_tag) {
  REQUIRE(std::string(c4h::eventProcessHistoryKey) == "Code4hep.edm.Event.processHistoryID");
  REQUIRE(std::string(c4h::runProcessHistoryKey) == "Code4hep.edm.Event.runProcessHistoryID");
  REQUIRE(std::string(c4h::lumiProcessHistoryKey) == "Code4hep.edm.Event.lumiProcessHistoryID");
  REQUIRE(std::string(c4h::luminosityBlockKey) == "Code4hep.edm.Event.luminosityBlock");
  auto const key = GENERATE(std::string(c4h::eventProcessHistoryKey), std::string(c4h::runProcessHistoryKey),
                            std::string(c4h::lumiProcessHistoryKey));
  auto const history = makeHistory("REFERENCE");
  auto const id = c4h::historyIDString(history.id());
  edm::ProcessHistoryRegistry registry;
  registry.registerProcessHistory(history);
  podio::Frame frame;
  SECTION("Valid reference") {
    frame.putParameter(key, id);
    REQUIRE(c4h::readHistoryID(frame, key, registry) == history.id());
    return;
  }
  SECTION("Missing key") {}
  SECTION("Wrong type") { frame.putParameter(key, 1); }
  SECTION("Empty vector") { frame.putParameter(key, std::vector<std::string>{}); }
  SECTION("Multiple IDs") { frame.putParameter(key, std::vector<std::string>{id, id}); }
  SECTION("Invalid hex") { frame.putParameter(key, GENERATE(std::string(31, 'a'), std::string(32, 'g'))); }
  SECTION("Default invalid hash") { frame.putParameter(key, std::string(32, '0')); }
  SECTION("Missing registered history") { frame.putParameter(key, std::string(32, '3')); }
  SECTION("Computed empty history is not registered") {
    frame.putParameter(key, c4h::historyIDString(edm::ProcessHistoryID{}));
  }
  SECTION("Conflicting types") {
    frame.putParameter(key, id);
    frame.putParameter(key, 1.f);
  }
  requireFormatError([&] { c4h::readHistoryID(frame, key, registry); });
  REQUIRE(std::distance(registry.begin(), registry.end()) == 1);
}
