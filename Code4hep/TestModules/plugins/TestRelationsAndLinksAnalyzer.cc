// -*- C++ -*-
//
// Package:    Code4hep/TestModules
//
/**\class c4h::TestRelationsAndLinksAnalyzer

 Description: Tests Podio relations and links by reading
 test values through them. One can configure which collections
 to read. One can also configured the test values we expect
 to read from them based on prior knowledge of the
 configuration of modules that produced the collections.

  The test involves reading values through relations
  and links, then throwing an exception if the expected
  value is not read.

*/
//
// Original Author:  W. David Dagenhart
//         Created:  3 April 2026
//

#include "podio/LinkCollection.h"

#include "edm4hep/MCParticleCollection.h"
#include "edm4hep/RecDqdxCollection.h"
#include "edm4hep/ReconstructedParticleCollection.h"
#include "edm4hep/TrackCollection.h"

#include "FWCore/Framework/interface/Event.h"
#include "FWCore/Framework/interface/Frameworkfwd.h"
#include "FWCore/Framework/interface/MakerMacros.h"
#include "FWCore/Framework/interface/global/EDAnalyzer.h"
#include "FWCore/ParameterSet/interface/ConfigurationDescriptions.h"
#include "FWCore/ParameterSet/interface/ParameterSet.h"
#include "FWCore/ParameterSet/interface/ParameterSetDescription.h"
#include "FWCore/Utilities/interface/EDGetToken.h"
#include "FWCore/Utilities/interface/Exception.h"
#include "FWCore/Utilities/interface/InputTag.h"
#include "FWCore/Utilities/interface/StreamID.h"

namespace c4h {
  class TestRelationsAndLinksAnalyzer : public edm::global::EDAnalyzer<> {
  public:
    explicit TestRelationsAndLinksAnalyzer(const edm::ParameterSet& iConfig);
    ~TestRelationsAndLinksAnalyzer() override = default;

    static void fillDescriptions(edm::ConfigurationDescriptions& descriptions);

  private:
    void analyze(edm::StreamID, const edm::Event& iEvent, const edm::EventSetup&) const final;

    edm::EDGetTokenT<edm4hep::TrackCollection> trackCollectionToken_;
    edm::EDGetTokenT<edm4hep::RecDqdxCollection> recDqdxCollectionToken_;
    edm::EDGetTokenT<edm4hep::ReconstructedParticleCollection> reconstructedParticleCollectionToken_;
    edm::EDGetTokenT<podio::LinkCollection<edm4hep::Track,edm4hep::MCParticle>> trackToMCParticleLinkCollectionToken_;
    const int expectedTrackTypeInTrackCollection_;
    const int expectedTrackTypeTrackInRelationOrLink_;
    const int expectedTestValueInMCParticle_;
  };

  TestRelationsAndLinksAnalyzer::TestRelationsAndLinksAnalyzer(const edm::ParameterSet& iConfig)
      : trackCollectionToken_(consumes(iConfig.getUntrackedParameter<edm::InputTag>("trackCollection"))),
        recDqdxCollectionToken_(consumes(iConfig.getUntrackedParameter<edm::InputTag>("recDqdxCollection"))),
        reconstructedParticleCollectionToken_(consumes(iConfig.getUntrackedParameter<edm::InputTag>("reconstructedParticleCollection"))),
        trackToMCParticleLinkCollectionToken_(consumes(iConfig.getUntrackedParameter<edm::InputTag>("trackToMCParticleLinkCollection"))),
        expectedTrackTypeInTrackCollection_(iConfig.getUntrackedParameter<int>("expectedTrackTypeInTrackCollection")),
        expectedTrackTypeTrackInRelationOrLink_(iConfig.getUntrackedParameter<int>("expectedTrackTypeTrackInRelationOrLink")),
        expectedTestValueInMCParticle_(iConfig.getUntrackedParameter<int>("expectedTestValueInMCParticle")) {}

  void TestRelationsAndLinksAnalyzer::analyze(edm::StreamID, const edm::Event& iEvent, const edm::EventSetup&) const {

    // Note the a Track object has a data member called "type" that is simply an int.
    // In this test, we read that integer value in different ways through links
    // and relations, then check that it has the expected value. The purpose is to
    // test that the links and relations are working correctly.
    // Beyond that, we don't actually care about the value of "type" or use it.

    // Test getting a track and then test getting another track
    // through a one-to-many relation in the Track.
    for (const auto& track : iEvent.get(trackCollectionToken_)) {
      if (track.getType() != expectedTrackTypeInTrackCollection_) {
        throw cms::Exception("TestFailure")
            << "TestRelationsAndLinksAnalyzer: expected track type " << expectedTrackTypeInTrackCollection_
            << " but got " << track.getType()
            << " when reading track through track collection";
      }
      auto const& tracksInTrack = track.getTracks();
      if (tracksInTrack.empty()) {
        throw cms::Exception("TestFailure")
            << "TestRelationsAndLinksAnalyzer: expected track to have a track in its relation but it has none";
      }
      if (tracksInTrack.front().getType() != expectedTrackTypeTrackInRelationOrLink_) {
        throw cms::Exception("TestFailure")
            << "TestRelationsAndLinksAnalyzer: expected track type " << expectedTrackTypeTrackInRelationOrLink_
            << " but got " << tracksInTrack.front().getType()
            << " when reading track through relation in track";
      }
    }

    // Test a one-to-one relation by getting a track from a RecDqdx object.
    for (const auto& recDqdx : iEvent.get(recDqdxCollectionToken_)) {
      const edm4hep::Track trackFromOneToOneRelation = recDqdx.getTrack();
      if (trackFromOneToOneRelation.getType() != expectedTrackTypeTrackInRelationOrLink_) {
        throw cms::Exception("TestFailure")
            << "TestRelationsAndLinksAnalyzer: expected track type " << expectedTrackTypeTrackInRelationOrLink_
            << " but got " << trackFromOneToOneRelation.getType()
            << " when reading track through one-to-one relation from RecDqdx";
      }
    }


    // Test a one-to-many relation by getting tracks from ReconstructedParticle objects.
    for (const auto& recoParticle : iEvent.get(reconstructedParticleCollectionToken_)) {
      auto const& tracksInRecoParticle = recoParticle.getTracks();
      if (tracksInRecoParticle.empty()) {
        throw cms::Exception("TestFailure")
            << "TestRelationsAndLinksAnalyzer: expected RecoParticle to have a track in its relation but it has none";
      }
      for (const auto& trackFromOneToManyRelation : tracksInRecoParticle) {
        if (trackFromOneToManyRelation.getType() != expectedTrackTypeTrackInRelationOrLink_) {
          throw cms::Exception("TestFailure")
              << "TestRelationsAndLinksAnalyzer: expected track type " << expectedTrackTypeTrackInRelationOrLink_
              << " but got " << trackFromOneToManyRelation.getType()
              << " when reading track through one-to-many relation from a ReconstructedParticle";
        }
      }
    }

    // Test a link collection by getting the linked MCParticle and Track.
    for (auto const& link : iEvent.get(trackToMCParticleLinkCollectionToken_)) {
      auto const& track = link.get<edm4hep::Track>();
      auto const& mcParticle = link.get<edm4hep::MCParticle>();
      if (track.getType() != expectedTrackTypeTrackInRelationOrLink_) {
        throw cms::Exception("TestFailure")
            << "TestRelationsAndLinksAnalyzer: expected track type " << expectedTrackTypeTrackInRelationOrLink_
            << " but got " << track.getType()
            << " when reading track through LinkCollection";
      }
      if (mcParticle.getPDG() != expectedTestValueInMCParticle_) {
        throw cms::Exception("TestFailure")
            << "TestRelationsAndLinksAnalyzer: expected test value " << expectedTestValueInMCParticle_
            << " but got " << mcParticle.getPDG()
            << " when reading MCParticle through LinkCollection";
      }
    }
  }

  void TestRelationsAndLinksAnalyzer::fillDescriptions(edm::ConfigurationDescriptions& descriptions) {
    edm::ParameterSetDescription desc;
    desc.addUntracked<edm::InputTag>("trackCollection");
    desc.addUntracked<edm::InputTag>("recDqdxCollection");
    desc.addUntracked<edm::InputTag>("reconstructedParticleCollection");
    desc.addUntracked<edm::InputTag>("trackToMCParticleLinkCollection");

    desc.addUntracked<int>("expectedTrackTypeInTrackCollection");
    desc.addUntracked<int>("expectedTrackTypeTrackInRelationOrLink");
    desc.addUntracked<int>("expectedTestValueInMCParticle");
    descriptions.addDefault(desc);
  }
}  // namespace c4h

DEFINE_FWK_MODULE(c4h::TestRelationsAndLinksAnalyzer);
