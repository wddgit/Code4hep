#include "catch2/catch_all.hpp"
#include "edm4hep/TrackCollection.h"
#include "edm4hep/TrackData.h"
#include "podio/CollectionIDTable.h"
#include <unordered_map>

#include "Code4hep/PodioUtilities/CollectionWrapperConverter.h"

C4H_CONTAINED_CLASS(edm4hep::TrackCollection, std::int32_t);
C4H_CONTAINED_CLASS(edm4hep::TrackCollection, edm4hep::TrackState);
C4H_CONTAINED_CLASS_NAMED(edm4hep::TrackCollection, edm4hep::TrackData, "edm4hep::TrackData");

namespace {

  template <typename T>
  std::unique_ptr<T> copyCollection(T& iCollection) {
    c4h::CollectionWrapperConverter<edm4hep::TrackCollection> converter;
    auto base = converter.copy(iCollection);
    assert(nullptr != dynamic_cast<T*>(base.get()));
    return std::unique_ptr<T>(static_cast<T*>(base.release()));
  }
};  // namespace

TEST_CASE("Copy a podio::Collection ", "[podio_copy]") {
  SECTION("LateIDAssignmentPreservesSelfRelationBuffers") {
    edm4hep::TrackCollection tracks;
    auto track = tracks.create();
    track.setType(21);
    track.addToTracks(track);
    // Event::emplace assigns the ID after relations have been populated.
    podio::CollectionIDTable ids;
    auto const id = ids.add("testRelationsAndLinksProducer21");
    tracks.setID(id);
    REQUIRE(track.getTracks().front().getObjectID().collectionID == id);
    auto copied = copyCollection(tracks);
    REQUIRE(copied->getID() == id);
    REQUIRE(copied->at(0).getObjectID().collectionID == id);
    auto buffers = copied->getBuffers();
    bool foundSelfID = false;
    for (auto const& references : *buffers.references) {
      for (auto const& reference : *references) {
        REQUIRE(reference.collectionID == id);
        REQUIRE(reference.index == 0);
        foundSelfID = true;
      }
    }
    REQUIRE(foundSelfID);
  }

  SECTION("TrackCollection") {
    edm4hep::TrackCollection tracks;
    auto track = tracks.create();
    track.setType(1);
    track.setNholes(1);

    edm4hep::TrackState state;
    track.addToTrackStates(state);

    auto newTrackCol = copyCollection(tracks);
    REQUIRE(tracks.getID() == newTrackCol->getID());
    REQUIRE(tracks.size() == 1);
    REQUIRE(tracks.size() == newTrackCol->size());
    REQUIRE(tracks[0].getType() == (*newTrackCol)[0].getType());
    REQUIRE(tracks[0].getNholes() == (*newTrackCol)[0].getNholes());
  }
}
