// -*- C++ -*-
//
// Package:    Code4hep/TestModules
//
/**\class c4h::TestParentageProducer

 Description: Test producer for parentage and subset collection tests.
 It puts one edm4hep::MCParticleCollection with no product instance name.

 - input empty (the default): creates one particle with PDG 11 and reads nothing,
   so the product has no recorded parents.
 - input set: reads that collection, which becomes the product's recorded parent,
   and creates one particle with the PDG of the input's first particle.
 - input set and subset true: puts a subset collection that refers to the
   input's first particle instead of creating a new one.

*/

#include <string>
#include <utility>

#include "Code4hep/PodioUtilities/assigningProductTo.h"
#include "FWCore/Framework/interface/Event.h"
#include "FWCore/Framework/interface/MakerMacros.h"
#include "FWCore/Framework/interface/global/EDProducer.h"
#include "FWCore/ParameterSet/interface/ConfigurationDescriptions.h"
#include "FWCore/ParameterSet/interface/ParameterSet.h"
#include "FWCore/ParameterSet/interface/ParameterSetDescription.h"
#include "FWCore/Utilities/interface/Exception.h"
#include "FWCore/Utilities/interface/InputTag.h"
#include "edm4hep/MCParticleCollection.h"

namespace c4h {
  class TestParentageProducer final : public edm::global::EDProducer<> {
  public:
    explicit TestParentageProducer(edm::ParameterSet const& config)
        : hasInput_(!config.getParameter<edm::InputTag>("input").label().empty()),
          subset_(config.getUntrackedParameter<bool>("subset")),
          output_(produces<edm4hep::MCParticleCollection>()) {
      if (subset_ && !hasInput_)
        throw cms::Exception("Configuration")
            << "TestParentageProducer: subset = True requires a nonempty input collection";
      if (hasInput_)
        input_ = consumes<edm4hep::MCParticleCollection>(config.getParameter<edm::InputTag>("input"));
    }

    static void fillDescriptions(edm::ConfigurationDescriptions& descriptions) {
      edm::ParameterSetDescription desc;
      desc.add<edm::InputTag>("input", edm::InputTag())
          ->setComment(
              "MCParticleCollection to read; it becomes the product's recorded parent. "
              "Empty: read nothing and create a particle with PDG 11.");
      desc.addUntracked<bool>("subset", false)
          ->setComment("Put a subset collection referring to the input's first particle. Requires input.");
      descriptions.addDefault(desc);
    }

  private:
    void produce(edm::StreamID, edm::Event& event, edm::EventSetup const&) const override {
      edm4hep::MCParticleCollection particles;
      if (subset_) {
        particles.setSubsetCollection();
        particles.push_back(event.get(input_).at(0));
      } else {
        particles.create().setPDG(hasInput_ ? event.get(input_).at(0).getPDG() : 11);
      }
      event.emplace(output_, std::move(particles));
    }

    bool const hasInput_;
    bool const subset_;
    edm::EDGetTokenT<edm4hep::MCParticleCollection> input_;
    edm::EDPutTokenT<edm4hep::MCParticleCollection> const output_;
  };
}  // namespace c4h

DEFINE_FWK_MODULE(c4h::TestParentageProducer);
