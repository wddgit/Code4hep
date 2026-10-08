// -*- C++ -*-
//
// Package:    Code4hep/TestModules
//
/**\class c4h::TestProvenanceAnalyzer

 Description: Tests that the provenance of a product can be reached through its
 Handle, using the same framework calls as in CMSSW:

 - ProductDescription: module label, process name and class name
 - ParameterSet of the producer, with edm::parameterSet and the event's ProcessHistory
 - ProcessHistory of the event
 - Parentage: the recorded parents of the product

 For a product read through an EDAlias, edm::parameterSet returns the EDAlias's
 own ParameterSet, as in CMSSW. The producer's ParameterSet is then found through
 the original product, identified by Provenance::originalBranchID().

 Each failed check throws cms::Exception("TestFailure").

*/

#include <algorithm>
#include <string>
#include <vector>

#include "DataFormats/Common/interface/Handle.h"
#include "DataFormats/Provenance/interface/BranchID.h"
#include "DataFormats/Provenance/interface/ProcessHistory.h"
#include "DataFormats/Provenance/interface/ProductProvenance.h"
#include "DataFormats/Provenance/interface/Provenance.h"
#include "DataFormats/Provenance/interface/StableProvenance.h"
#include "FWCore/Common/interface/Provenance.h"
#include "FWCore/Framework/interface/Event.h"
#include "FWCore/Framework/interface/MakerMacros.h"
#include "FWCore/Framework/interface/global/EDAnalyzer.h"
#include "FWCore/ParameterSet/interface/ConfigurationDescriptions.h"
#include "FWCore/ParameterSet/interface/ParameterSet.h"
#include "FWCore/ParameterSet/interface/ParameterSetDescription.h"
#include "FWCore/Utilities/interface/EDGetToken.h"
#include "FWCore/Utilities/interface/Exception.h"
#include "FWCore/Utilities/interface/InputTag.h"
#include "edm4hep/TrackCollection.h"

namespace c4h {
  class TestProvenanceAnalyzer : public edm::global::EDAnalyzer<> {
  public:
    explicit TestProvenanceAnalyzer(edm::ParameterSet const& config)
        : token_(consumes(config.getUntrackedParameter<edm::InputTag>("tracks"))),
          expectedModuleLabel_(config.getUntrackedParameter<std::string>("expectedModuleLabel")),
          expectedProcessName_(config.getUntrackedParameter<std::string>("expectedProcessName")),
          expectedModuleType_(config.getUntrackedParameter<std::string>("expectedModuleType")),
          expectedOriginalModuleLabel_(config.getUntrackedParameter<std::string>("expectedOriginalModuleLabel")),
          expectedParents_(config.getUntrackedParameter<std::vector<std::string>>("expectedParents")),
          expectedProcessHistory_(config.getUntrackedParameter<std::vector<std::string>>("expectedProcessHistory")) {}

    static void fillDescriptions(edm::ConfigurationDescriptions& descriptions) {
      edm::ParameterSetDescription desc;
      desc.addUntracked<edm::InputTag>("tracks")->setComment("TrackCollection whose provenance is checked.");
      desc.addUntracked<std::string>("expectedModuleLabel")
          ->setComment("Module label of the product, which for an EDAlias is the alias label.");
      desc.addUntracked<std::string>("expectedProcessName");
      desc.addUntracked<std::string>("expectedModuleType")
          ->setComment("@module_type in the ParameterSet of the module that produced the product.");
      desc.addUntracked<std::string>("expectedOriginalModuleLabel", "")
          ->setComment("Nonempty: the product is read through an EDAlias of this original module.");
      desc.addUntracked<std::vector<std::string>>("expectedParents", {})
          ->setComment("Each entry 'moduleLabel:productInstanceName:processName' must be a recorded parent.");
      desc.addUntracked<std::vector<std::string>>("expectedProcessHistory")
          ->setComment("Process names of the event's ProcessHistory, oldest first.");
      descriptions.addDefault(desc);
    }

  private:
    void analyze(edm::StreamID, edm::Event const& event, edm::EventSetup const&) const final {
      // The same lines as in CMSSW: get a Handle, then the producer's ParameterSet.
      // (The isValid check is added only because the test reports failures itself.)
      edm::Handle<edm4hep::TrackCollection> h;
      event.getByToken(token_, h);
      if (!h.isValid())
        fail("the Handle is not valid");
      edm::ParameterSet const& producerPSet = edm::parameterSet(h.provenance()->stable(), event.processHistory());
      std::string type = producerPSet.getParameter<std::string>("@module_type");

      edm::Provenance const& provenance = *h.provenance();

      // ProductDescription
      if (provenance.moduleLabel() != expectedModuleLabel_)
        fail("module label is '" + provenance.moduleLabel() + "', expected '" + expectedModuleLabel_ + "'");
      if (provenance.processName() != expectedProcessName_)
        fail("process name is '" + provenance.processName() + "', expected '" + expectedProcessName_ + "'");
      if (provenance.className() != "edm4hep::TrackCollection")
        fail("class name is '" + provenance.className() + "', expected 'edm4hep::TrackCollection'");

      // ParameterSet of the producer
      if (expectedOriginalModuleLabel_.empty()) {
        if (provenance.originalBranchID() != provenance.branchID())
          fail("the product was read through an EDAlias, which the configuration does not expect");
        checkProducer(producerPSet, type, provenance.moduleLabel());
      } else {
        // For an EDAlias, edm::parameterSet returns the alias's own ParameterSet.
        if (type != "EDAlias")
          fail("@module_type for the EDAlias label is '" + type + "', expected 'EDAlias'");
        if (provenance.originalBranchID() == provenance.branchID())
          fail("the product was not read through an EDAlias");
        // The original product's description is kept, marked dropped, so its
        // provenance identifies the producer.
        edm::StableProvenance const& original = event.getStableProvenance(provenance.originalBranchID());
        if (original.moduleLabel() != expectedOriginalModuleLabel_)
          fail("original module label is '" + original.moduleLabel() + "', expected '" +
               expectedOriginalModuleLabel_ + "'");
        edm::ParameterSet const& originalPSet = edm::parameterSet(original, event.processHistory());
        checkProducer(originalPSet, originalPSet.getParameter<std::string>("@module_type"), original.moduleLabel());
      }

      // ProcessHistory
      std::vector<std::string> processes;
      for (auto const& configuration : event.processHistory())
        processes.push_back(configuration.processName());
      if (processes != expectedProcessHistory_)
        fail("process history is '" + join(processes) + "', expected '" + join(expectedProcessHistory_) + "'");

      // Parentage
      edm::ProductProvenance const* productProvenance = provenance.productProvenance();
      if (!productProvenance)
        fail("no product provenance (parentage) is available");
      std::vector<std::string> parents;
      for (auto const& parent : productProvenance->parentage().parents()) {
        try {
          edm::StableProvenance const& stable = event.getStableProvenance(parent);
          parents.push_back(stable.moduleLabel() + ":" + stable.productInstanceName() + ":" + stable.processName());
        } catch (cms::Exception const&) {
          parents.push_back("<unknown BranchID " + std::to_string(parent.id()) + ">");
        }
      }
      for (auto const& expected : expectedParents_) {
        if (std::find(parents.begin(), parents.end(), expected) == parents.end())
          fail("recorded parents are '" + join(parents) + "', which do not include '" + expected + "'");
      }
    }

    void checkProducer(edm::ParameterSet const& pset, std::string const& type, std::string const& label) const {
      if (type != expectedModuleType_)
        fail("@module_type of the producer is '" + type + "', expected '" + expectedModuleType_ + "'");
      auto const storedLabel = pset.getParameter<std::string>("@module_label");
      if (storedLabel != label)
        fail("@module_label of the producer is '" + storedLabel + "', expected '" + label + "'");
    }

    [[noreturn]] void fail(std::string const& message) const {
      throw cms::Exception("TestFailure") << "TestProvenanceAnalyzer: " << message;
    }

    static std::string join(std::vector<std::string> const& items) {
      std::string result;
      for (auto const& item : items)
        result += (result.empty() ? "" : " ") + item;
      return result;
    }

    edm::EDGetTokenT<edm4hep::TrackCollection> const token_;
    std::string const expectedModuleLabel_;
    std::string const expectedProcessName_;
    std::string const expectedModuleType_;
    std::string const expectedOriginalModuleLabel_;
    std::vector<std::string> const expectedParents_;
    std::vector<std::string> const expectedProcessHistory_;
  };
}  // namespace c4h

DEFINE_FWK_MODULE(c4h::TestProvenanceAnalyzer);
