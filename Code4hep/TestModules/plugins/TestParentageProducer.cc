#include <string>
#include <utility>
#include "Code4hep/PodioUtilities/assigningProductTo.h"
#include "FWCore/Framework/interface/Event.h"
#include "FWCore/Framework/interface/MakerMacros.h"
#include "FWCore/Framework/interface/global/EDProducer.h"
#include "FWCore/ParameterSet/interface/ParameterSet.h"
#include "FWCore/Utilities/interface/InputTag.h"
#include "edm4hep/MCParticleCollection.h"

namespace c4h {
  class TestParentageProducer final : public edm::global::EDProducer<> {
  public:
    explicit TestParentageProducer(edm::ParameterSet const& config)
        : hasInput_(!config.getParameter<edm::InputTag>("input").label().empty()),
          subset_(config.getUntrackedParameter<bool>("subset", false)),
          output_(produces<edm4hep::MCParticleCollection>()) {
      if (hasInput_)
        input_ = consumes<edm4hep::MCParticleCollection>(config.getParameter<edm::InputTag>("input"));
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
}

DEFINE_FWK_MODULE(c4h::TestParentageProducer);
